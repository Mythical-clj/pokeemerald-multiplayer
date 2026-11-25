#include "global.h"
#include "battle.h"
#include "battle_setup.h"
#include "data.h"
#include "load_save.h"
#include "main.h"
#include "pokemon.h"
#include "remote_battle.h"
#include "remote_player.h"
#include "string_util.h"
#include "constants/battle.h"
#include "constants/characters.h"
#include "constants/pokemon.h"
#include "constants/species.h"
#include "constants/trainers.h"

// ---------------------------------------------------------------------------
// Wire format. Both kinds keep the map in bytes 4-5 like every other event,
// because the sync project's relay and client look there.
// ---------------------------------------------------------------------------

#define TEAM_SIZE           3
#define TEAM_BYTES          (TEAM_SIZE * sizeof(struct Pokemon))   // 300
#define NAME_BYTES          8
#define PAYLOAD_BYTES       (TEAM_BYTES + NAME_BYTES)
#define CHUNK_BYTES         6
#define CHUNK_COUNT         ((PAYLOAD_BYTES + CHUNK_BYTES - 1) / CHUNK_BYTES)

#define CHUNK_PERIOD_FRAMES     5       // one chunk per 5 frames: a full cycle in about 4 s
#define REFRESH_PERIOD_FRAMES   30      // how often the local team is read again
#define PARTNER_MAX_DISTANCE    5       // tiles (Manhattan) between the players

#define AWARD_MAX               6
#define AWARD_SENDS             10
#define AWARD_PERIOD_FRAMES     90
#define SEEN_AWARDS             8

struct PartyChunk
{
    u8 type;
    u8 idx;
    u8 tag;
    u8 seq;
    u8 mapGroup;
    u8 mapNum;
    u8 data[CHUNK_BYTES];
};

struct ExpMessage
{
    u8 type;
    u8 expLo;
    u8 expHi;
    u8 id;
    u8 mapGroup;
    u8 mapNum;
    u8 personality[4];
    u8 species[2];
};

STATIC_ASSERT(sizeof(struct PartyChunk) == 12, PartyChunkIs12Bytes);
STATIC_ASSERT(sizeof(struct ExpMessage) == 12, ExpMessageIs12Bytes);

struct PeerTeam
{
    u8 assembly[CHUNK_COUNT * CHUNK_BYTES];
    u8 got[CHUNK_COUNT];
    u8 gotCount;
    u8 asmTag;
    bool8 ready;
    u8 gender;
    u8 name[NAME_BYTES];
    struct Pokemon mons[TEAM_SIZE];
};

struct PendingAward
{
    u32 personality;
    u16 species;
    u16 exp;
    u8 id;
    u8 sendsLeft;
    u8 cooldown;
};

struct SeenAward
{
    u32 personality;
    u8 id;
    bool8 used;
};

EWRAM_DATA bool8 gRemoteBattleActive = FALSE;
EWRAM_DATA bool8 gRemoteSplitTrainer = FALSE;

static EWRAM_DATA struct PeerTeam sPeers[REMOTE_MAX_PLAYERS] = {0};

// What the local player sends.
static EWRAM_DATA struct Pokemon sSendMons[TEAM_SIZE] = {0};
static EWRAM_DATA u8 sSendName[NAME_BYTES] = {0};
static EWRAM_DATA u8 sSendTag = 0;
static EWRAM_DATA bool8 sSendValid = FALSE;
static EWRAM_DATA u8 sSendCursor = 0;
static EWRAM_DATA u16 sFrame = 0;

static EWRAM_DATA struct PendingAward sAwards[AWARD_MAX] = {0};
static EWRAM_DATA struct SeenAward sSeen[SEEN_AWARDS] = {0};
static EWRAM_DATA u8 sSeenNext = 0;
static EWRAM_DATA u8 sNextAwardId = 1;

// State of the battle being set up or just fought.
static EWRAM_DATA bool8 sRematch = FALSE;
static EWRAM_DATA u8 sLockedSlot = REMOTE_MAX_PLAYERS;
static EWRAM_DATA u32 sMoneyBefore = 0;
static EWRAM_DATA bool8 sWrapped = FALSE;
static EWRAM_DATA MainCallback sRealCallback = NULL;
static EWRAM_DATA u8 sPick[TEAM_SIZE] = {0};
static EWRAM_DATA u8 sPickCount = 0;
static EWRAM_DATA u32 sExpBefore[TEAM_SIZE] = {0};
static EWRAM_DATA u8 sPartnerGender = MALE;
static EWRAM_DATA u8 sPartnerName[NAME_BYTES + 1] = {0};
static EWRAM_DATA struct Pokemon sBattleParty[PARTY_SIZE] = {0};

// ---------------------------------------------------------------------------
// The local team
// ---------------------------------------------------------------------------

static bool8 IsUsable(struct Pokemon *mon)
{
    return GetMonData(mon, MON_DATA_SPECIES) != SPECIES_NONE
        && !GetMonData(mon, MON_DATA_IS_EGG)
        && GetMonData(mon, MON_DATA_HP) != 0;
}

// Picks the first usable Pokémon, up to three, in party order.
static u8 BuildMyTeam(u8 *pick)
{
    u8 count = 0;
    u8 i;

    for (i = 0; i < PARTY_SIZE && count < TEAM_SIZE; i++)
    {
        if (IsUsable(&gPlayerParty[i]))
            pick[count++] = i;
    }
    return count;
}

static u8 SendByte(u16 offset)
{
    if (offset < TEAM_BYTES)
        return ((u8 *)sSendMons)[offset];
    if (offset < PAYLOAD_BYTES)
        return sSendName[offset - TEAM_BYTES];
    return 0;
}

static void RefreshSendBuffer(void)
{
    u8 pick[TEAM_SIZE];
    u8 count = BuildMyTeam(pick);
    u16 i;
    u8 tag = 0;

    for (i = 0; i < TEAM_SIZE; i++)
    {
        if (i < count)
            sSendMons[i] = gPlayerParty[pick[i]];
        else
            ZeroMonData(&sSendMons[i]);
    }
    for (i = 0; i < NAME_BYTES; i++)
        sSendName[i] = EOS;
    StringCopyN(sSendName, gSaveBlock2Ptr->playerName, PLAYER_NAME_LENGTH);

    for (i = 0; i < PAYLOAD_BYTES; i++)
        tag = ((tag << 1) | (tag >> 7)) ^ SendByte(i);
    if (tag == 0)
        tag = 1;

    sSendTag = tag;
    sSendValid = count != 0;
}

// ---------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------

static void SendNextChunk(void)
{
    struct PartyChunk chunk;
    u8 i;

    chunk.type = REMOTE_EVENT_BPARTY;
    chunk.idx = sSendCursor;
    chunk.tag = sSendTag;
    chunk.seq = RemotePlayer_NextTxSeq();
    chunk.mapGroup = gSaveBlock1Ptr->location.mapGroup;
    chunk.mapNum = gSaveBlock1Ptr->location.mapNum;
    for (i = 0; i < CHUNK_BYTES; i++)
        chunk.data[i] = SendByte(sSendCursor * CHUNK_BYTES + i);

    if (!RemotePlayer_TryPushTx((const struct RemoteEvent *)&chunk))
        return;
    sSendCursor++;
    if (sSendCursor >= CHUNK_COUNT)
        sSendCursor = 0;
}

static void SendPendingAwards(void)
{
    u8 i;

    for (i = 0; i < AWARD_MAX; i++)
    {
        struct PendingAward *award = &sAwards[i];
        struct ExpMessage msg;

        if (award->sendsLeft == 0)
            continue;
        if (award->cooldown != 0)
        {
            award->cooldown--;
            continue;
        }

        msg.type = REMOTE_EVENT_BEXP;
        msg.expLo = award->exp & 0xFF;
        msg.expHi = award->exp >> 8;
        msg.id = award->id;
        msg.mapGroup = gSaveBlock1Ptr->location.mapGroup;
        msg.mapNum = gSaveBlock1Ptr->location.mapNum;
        msg.personality[0] = award->personality & 0xFF;
        msg.personality[1] = (award->personality >> 8) & 0xFF;
        msg.personality[2] = (award->personality >> 16) & 0xFF;
        msg.personality[3] = award->personality >> 24;
        msg.species[0] = award->species & 0xFF;
        msg.species[1] = award->species >> 8;

        if (!RemotePlayer_TryPushTx((const struct RemoteEvent *)&msg))
            return;
        award->sendsLeft--;
        award->cooldown = AWARD_PERIOD_FRAMES;
    }
}

static void QueueAward(struct Pokemon *mon, u32 before)
{
    u32 after = GetMonData(mon, MON_DATA_EXP);
    u32 gained;
    u8 i;

    if (after <= before)
        return;
    gained = after - before;
    if (gained > 0xFFFF)
        gained = 0xFFFF;

    for (i = 0; i < AWARD_MAX; i++)
    {
        if (sAwards[i].sendsLeft == 0)
        {
            sAwards[i].personality = GetMonData(mon, MON_DATA_PERSONALITY);
            sAwards[i].species = GetMonData(mon, MON_DATA_SPECIES);
            sAwards[i].exp = gained;
            sAwards[i].id = sNextAwardId++;
            if (sNextAwardId == 0)
                sNextAwardId = 1;
            sAwards[i].sendsLeft = AWARD_SENDS;
            sAwards[i].cooldown = 0;
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Receiving
// ---------------------------------------------------------------------------

static void CompleteTeam(struct PeerTeam *peer)
{
    u8 i;
    u16 j;
    u8 *dest = (u8 *)peer->mons;

    for (j = 0; j < TEAM_BYTES; j++)
        dest[j] = peer->assembly[j];
    for (i = 0; i < NAME_BYTES; i++)
        peer->name[i] = peer->assembly[TEAM_BYTES + i];
    peer->name[NAME_BYTES - 1] = EOS;

    // Anything that does not look like a Pokémon is dropped.
    for (i = 0; i < TEAM_SIZE; i++)
    {
        struct Pokemon *mon = &peer->mons[i];

        if (GetMonData(mon, MON_DATA_SANITY_IS_BAD_EGG) || GetMonData(mon, MON_DATA_IS_EGG)
         || GetMonData(mon, MON_DATA_SPECIES) == SPECIES_NONE
         || GetMonData(mon, MON_DATA_SPECIES) >= NUM_SPECIES)
            ZeroMonData(mon);
    }
    peer->ready = GetMonData(&peer->mons[0], MON_DATA_SPECIES) != SPECIES_NONE;
}

static void HandlePartyChunk(u8 slot, const struct PartyChunk *chunk)
{
    struct PeerTeam *peer = &sPeers[slot];
    u8 i;

    if (chunk->idx >= CHUNK_COUNT || chunk->tag == 0)
        return;
    if (chunk->tag != peer->asmTag)
    {
        // The sender's team changed: start over.
        peer->asmTag = chunk->tag;
        peer->gotCount = 0;
        for (i = 0; i < CHUNK_COUNT; i++)
            peer->got[i] = FALSE;
    }
    if (peer->got[chunk->idx])
        return;
    for (i = 0; i < CHUNK_BYTES; i++)
        peer->assembly[chunk->idx * CHUNK_BYTES + i] = chunk->data[i];
    peer->got[chunk->idx] = TRUE;
    peer->gotCount++;
    if (peer->gotCount == CHUNK_COUNT)
        CompleteTeam(peer);
}

static void ApplyAward(u32 personality, u16 species, u16 exp)
{
    u8 i;

    for (i = 0; i < PARTY_SIZE; i++)
    {
        struct Pokemon *mon = &gPlayerParty[i];
        u32 current;
        u32 cap;
        u32 total;
        u8 level;
        u8 newLevel;

        if (GetMonData(mon, MON_DATA_SPECIES) != species || GetMonData(mon, MON_DATA_PERSONALITY) != personality)
            continue;
        level = GetMonData(mon, MON_DATA_LEVEL);
        if (level >= MAX_LEVEL)
            return;

        current = GetMonData(mon, MON_DATA_EXP);
        cap = gExperienceTables[gSpeciesInfo[species].growthRate][MAX_LEVEL];
        total = current + exp;
        if (total > cap)
            total = cap;
        SetMonData(mon, MON_DATA_EXP, &total);

        newLevel = GetLevelFromMonExp(mon);
        if (newLevel != level)
        {
            // Stats follow the level; the game keeps the current HP in step
            // when the maximum changes. Evolution and new moves wait for the
            // player to level up the usual way.
            SetMonData(mon, MON_DATA_LEVEL, &newLevel);
            CalculateMonStats(mon);
        }
        return;
    }
}

static void HandleExpMessage(const struct ExpMessage *msg)
{
    u32 personality = msg->personality[0] | (msg->personality[1] << 8)
                    | (msg->personality[2] << 16) | ((u32)msg->personality[3] << 24);
    u16 species = msg->species[0] | (msg->species[1] << 8);
    u16 exp = msg->expLo | (msg->expHi << 8);
    u8 i;

    if (exp == 0 || species == SPECIES_NONE || species >= NUM_SPECIES)
        return;
    // The same award arrives several times; apply each once.
    for (i = 0; i < SEEN_AWARDS; i++)
    {
        if (sSeen[i].used && sSeen[i].id == msg->id && sSeen[i].personality == personality)
            return;
    }
    sSeen[sSeenNext].used = TRUE;
    sSeen[sSeenNext].id = msg->id;
    sSeen[sSeenNext].personality = personality;
    sSeenNext = (sSeenNext + 1) % SEEN_AWARDS;

    ApplyAward(personality, species, exp);
}

void RemoteBattle_HandleRx(const struct RemoteEvent *event)
{
    u8 slot = REMOTE_EVENT_SLOT(event->type);
    u8 kind = REMOTE_EVENT_KIND(event->type);

    if (slot >= REMOTE_MAX_PLAYERS)
        return;
    if (kind == REMOTE_EVENT_BPARTY)
        HandlePartyChunk(slot, (const struct PartyChunk *)event);
    else if (kind == REMOTE_EVENT_BEXP)
        HandleExpMessage((const struct ExpMessage *)event);
}

// ---------------------------------------------------------------------------
// Per frame
// ---------------------------------------------------------------------------

void RemoteBattle_Init(void)
{
    u8 i;

    for (i = 0; i < REMOTE_MAX_PLAYERS; i++)
    {
        sPeers[i].ready = FALSE;
        sPeers[i].gotCount = 0;
        sPeers[i].asmTag = 0;
    }
    sSendValid = FALSE;
    sSendCursor = 0;
}

void RemoteBattle_Tick(void)
{
    sFrame++;
    if (!REMOTE_BATTLE_ENABLED)
        return;

    if (sFrame % REFRESH_PERIOD_FRAMES == 1)
        RefreshSendBuffer();

#if REMOTE_PLAYER_LOOPBACK
    if (RemotePlayer_InLoopback())
    {
        // No link: the ghost's team is a copy of the local one.
        if (sFrame % REFRESH_PERIOD_FRAMES == 1 && sSendValid)
        {
            u8 i;

            for (i = 0; i < TEAM_SIZE; i++)
                sPeers[0].mons[i] = sSendMons[i];
            for (i = 0; i < NAME_BYTES; i++)
                sPeers[0].name[i] = sSendName[i];
            sPeers[0].ready = TRUE;
        }
        return;
    }
#endif

    if (sSendValid && sFrame % CHUNK_PERIOD_FRAMES == 0)
        SendNextChunk();
    SendPendingAwards();
}

// ---------------------------------------------------------------------------
// Starting a battle
// ---------------------------------------------------------------------------

static bool8 FindPartner(u8 *slot)
{
    u8 gender;

    if (sLockedSlot < REMOTE_MAX_PLAYERS && sPeers[sLockedSlot].ready && RemotePlayer_SlotIsNear(sLockedSlot, PARTNER_MAX_DISTANCE + 2, &gender))
    {
        *slot = sLockedSlot;
        sPartnerGender = gender;
        return TRUE;
    }
    if (!RemotePlayer_FindNearby(PARTNER_MAX_DISTANCE, slot, &gender))
        return FALSE;
    if (!sPeers[*slot].ready)
        return FALSE;
    sPartnerGender = gender;
    return TRUE;
}

// A single trainer can be split over both opponent slots when they have at
// least two Pokémon; two trainers need nothing.
static bool8 TrainerFitsPartnerBattle(void)
{
    if (gBattleTypeFlags & BATTLE_TYPE_TWO_OPPONENTS)
        return TRUE;
    if (gTrainerBattleOpponent_A == 0 || gTrainerBattleOpponent_A == TRAINER_SECRET_BASE)
        return FALSE;
    return gTrainers[gTrainerBattleOpponent_A].partySize >= 2;
}

bool8 RemoteBattle_ShouldRematch(void)
{
    u8 slot;
    u8 pick[TEAM_SIZE];

    if (!REMOTE_BATTLE_ENABLED)
        return FALSE;
    if (gTrainerBattleOpponent_A == 0 || gTrainerBattleOpponent_A == TRAINER_SECRET_BASE)
        return FALSE;
    if (gTrainers[gTrainerBattleOpponent_A].partySize < 2 && gTrainerBattleOpponent_B == 0)
        return FALSE;
    if (!FindPartner(&slot) || BuildMyTeam(pick) == 0)
        return FALSE;

    sRematch = TRUE;
    sLockedSlot = slot;
    return TRUE;
}

static void CB2_RemoteBattleDone(void)
{
    u8 i;

    if (gRemoteBattleActive)
    {
        // The party in use was the battle party: put the player's Pokémon
        // back where they were, with what they earned.
        for (i = 0; i < PARTY_SIZE; i++)
            sBattleParty[i] = gPlayerParty[i];
        LoadPlayerParty();
        for (i = 0; i < sPickCount; i++)
            gPlayerParty[sPick[i]] = sBattleParty[i];

        if (!RemotePlayer_InLoopback())
        {
            for (i = 0; i < TEAM_SIZE; i++)
            {
                if (GetMonData(&sBattleParty[TEAM_SIZE + i], MON_DATA_SPECIES) != SPECIES_NONE)
                    QueueAward(&sBattleParty[TEAM_SIZE + i], sExpBefore[i]);
            }
        }
        gRemoteBattleActive = FALSE;
        gRemoteSplitTrainer = FALSE;
    }

    if (sRematch)
    {
        // A battle against a trainer already beaten pays nothing again.
        if (gSaveBlock1Ptr->money > sMoneyBefore)
            gSaveBlock1Ptr->money = sMoneyBefore;
        sRematch = FALSE;
    }
    sLockedSlot = REMOTE_MAX_PLAYERS;
    sWrapped = FALSE;
    SetMainCallback2(sRealCallback);
}

bool8 RemoteBattle_PrepareTrainerBattle(void)
{
    u8 slot;
    u8 i;
    struct PeerTeam *peer;

    gRemoteBattleActive = FALSE;
    gRemoteSplitTrainer = FALSE;
    sMoneyBefore = gSaveBlock1Ptr->money;

    if (!REMOTE_BATTLE_ENABLED || !FindPartner(&slot) || !TrainerFitsPartnerBattle())
        goto finish;
    sPickCount = BuildMyTeam(sPick);
    if (sPickCount == 0)
        goto finish;

    peer = &sPeers[slot];

    if (!(gBattleTypeFlags & BATTLE_TYPE_TWO_OPPONENTS))
    {
        // One trainer plays both opponents.
        gTrainerBattleOpponent_B = gTrainerBattleOpponent_A;
        BattleSetup_CopyTrainerAToB();
        gRemoteSplitTrainer = TRUE;
        gBattleTypeFlags |= BATTLE_TYPE_DOUBLE | BATTLE_TYPE_TWO_OPPONENTS;
    }
    gBattleTypeFlags |= BATTLE_TYPE_MULTI | BATTLE_TYPE_INGAME_PARTNER;

    // The battle party: the player's three in front, the partner's behind.
    SavePlayerParty();
    for (i = 0; i < PARTY_SIZE; i++)
        ZeroMonData(&gPlayerParty[i]);
    for (i = 0; i < sPickCount; i++)
        gPlayerParty[i] = gSaveBlock1Ptr->playerParty[sPick[i]];
    for (i = 0; i < TEAM_SIZE; i++)
    {
        gPlayerParty[TEAM_SIZE + i] = peer->mons[i];
        sExpBefore[i] = GetMonData(&peer->mons[i], MON_DATA_EXP);
    }
    CalculatePlayerPartyCount();

    for (i = 0; i < NAME_BYTES; i++)
        sPartnerName[i] = peer->name[i];
    sPartnerName[NAME_BYTES] = EOS;
    gPartnerTrainerId = TRAINER_STEVEN_PARTNER;
    gRemoteBattleActive = TRUE;

finish:
    if (gRemoteBattleActive || sRematch)
    {
        if (!sWrapped)
        {
            sRealCallback = gMain.savedCallback;
            gMain.savedCallback = CB2_RemoteBattleDone;
            sWrapped = TRUE;
        }
    }
    return gRemoteBattleActive;
}

const u8 *RemoteBattle_GetPartnerName(void)
{
    return sPartnerName;
}

u8 RemoteBattle_GetPartnerBackPic(void)
{
    if (gRemoteBattleActive)
        return sPartnerGender == FEMALE ? TRAINER_BACK_PIC_MAY : TRAINER_BACK_PIC_BRENDAN;
    return TRAINER_BACK_PIC_STEVEN;
}
