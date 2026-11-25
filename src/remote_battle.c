#include "global.h"
#include "battle.h"
#include "battle_setup.h"
#include "data.h"
#include "load_save.h"
#include "main.h"
#include "menu.h"
#include "text.h"
#include "window.h"
#include "pokemon.h"
#include "remote_battle.h"
#include "remote_player.h"
#include "remote_sio.h"
#include "remote_social.h"
#include "battle_transition.h"
#include "constants/opponents.h"
#include "overworld.h"
#include "field_player_avatar.h"
#include "random.h"
#include "script.h"
#include "sound.h"
#include "constants/songs.h"
#include "string_util.h"
#include "constants/battle.h"
#include "constants/characters.h"
#include "constants/opponents.h"
#include "constants/pokemon.h"
#include "constants/species.h"
#include "constants/trainers.h"

static void CB2_RemoteBattleDone(void);

// ---------------------------------------------------------------------------
// Wire format. Both kinds keep the map in bytes 4-5 like every other event,
// because the sync project's relay and client look there.
// ---------------------------------------------------------------------------

#define TEAM_SIZE           3       // co-op: each player brings three
#define SEND_SIZE           4       // what is sent: versus doubles uses four
#define TEAM_BYTES          (SEND_SIZE * sizeof(struct Pokemon))   // 400
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

#define CHOICE_RING             8
#define CHOICE_RESEND_FRAMES    10
#define SYNC_PERIOD_FRAMES      15
#define START_RESEND_FRAMES     20
#define START_TIMEOUT_FRAMES    1200    // 20 s before the game plays the partner
#define NEAR_TIMEOUT_FRAMES     1500    // 25 s for the other player to come within range
#define GUEST_ACCEPT_FRAMES     24      // acks sent before the guest's battle starts
#define GUEST_PENDING_FRAMES    1200     // how long the guest waits for a moment to join
#define REMOTE_WAIT_FRAMES      3600    // contact lost: the remote battler plays itself

#define ROLE_NONE   0
#define ROLE_HOST   1
#define ROLE_GUEST  2

#define CHOICE_TWO  1
#define CHOICE_MON  2
#define CHOICE_ONE  3

#define FLAG_SPLIT  1
#define FLAG_REMATCH 2

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

// Sent by the host until the guest's game says it takes part. Two parts, as
// a message has room for six bytes of data.
//   part 0: seed (4), host team tag, guest team tag
//   part 1: trainer A (2), trainer B (2), flags (1: bit 0 = one trainer plays both)
struct StartMessage
{
    u8 type;
    u8 part;
    u8 pad;
    u8 session;
    u8 mapGroup;
    u8 mapNum;
    u8 data[6];
};

struct SyncMessage
{
    u8 type;
    u8 startAck;    // the guest takes part
    u8 ackN;        // how many choices of the other player have arrived, in order
    u8 session;
    u8 mapGroup;
    u8 mapNum;
    u8 data[6];
};

struct ChoiceMessage
{
    u8 type;
    u8 n;           // position in the sender's choices
    u8 pad;
    u8 session;
    u8 mapGroup;
    u8 mapNum;
    u8 data[6];     // RemoteChoice: kind, a, b, c, d
};

STATIC_ASSERT(sizeof(struct StartMessage) == 12, StartMessageIs12Bytes);
STATIC_ASSERT(sizeof(struct SyncMessage) == 12, SyncMessageIs12Bytes);
STATIC_ASSERT(sizeof(struct ChoiceMessage) == 12, ChoiceMessageIs12Bytes);
STATIC_ASSERT(sizeof(struct PartyChunk) == 12, PartyChunkIs12Bytes);
STATIC_ASSERT(sizeof(struct ExpMessage) == 12, ExpMessageIs12Bytes);

struct PeerTeam
{
    u8 assembly[CHUNK_COUNT * CHUNK_BYTES];
    u8 got[CHUNK_COUNT];
    u8 gotCount;
    u8 asmTag;
    bool8 ready;
    u8 readyTag;
    u8 gender;
    u8 name[NAME_BYTES];
    struct Pokemon mons[SEND_SIZE];
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
EWRAM_DATA bool8 gRemoteHumanBattle = FALSE;
EWRAM_DATA bool8 gRemoteVersus = FALSE;

static EWRAM_DATA struct PeerTeam sPeers[REMOTE_MAX_PLAYERS] = {0};

// What the local player sends.
static EWRAM_DATA struct Pokemon sSendMons[SEND_SIZE] = {0};
static EWRAM_DATA u8 sSendName[NAME_BYTES] = {0};
static EWRAM_DATA u8 sSendTag = 0;
static EWRAM_DATA u8 sSendPick[SEND_SIZE] = {0};
static EWRAM_DATA u8 sSendCount = 0;
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
static EWRAM_DATA u8 sPick[SEND_SIZE] = {0};
static EWRAM_DATA u8 sPickCount = 0;
static EWRAM_DATA u32 sExpBefore[TEAM_SIZE] = {0};
static EWRAM_DATA u8 sPartnerGender = MALE;
static EWRAM_DATA struct Pokemon sVersusEnemy[SEND_SIZE] = {{0}};
static EWRAM_DATA u8 sPartnerName[NAME_BYTES + 1] = {0};
static EWRAM_DATA struct Pokemon sBattleParty[PARTY_SIZE] = {0};

// The human partner battle.
static EWRAM_DATA u8 sRole = ROLE_NONE;
static EWRAM_DATA u8 sSession = 0;
static EWRAM_DATA u8 sSessionCounter = 0;
static EWRAM_DATA u8 sPartnerSlot = 0;
static EWRAM_DATA u32 sSeed = 0;
static EWRAM_DATA bool8 sAwaitingAck = FALSE;
static EWRAM_DATA bool8 sStartAcked = FALSE;
static EWRAM_DATA u16 sStartWait = 0;
static EWRAM_DATA u8 sHostTag = 0;
static EWRAM_DATA u8 sGuestTag = 0;
static EWRAM_DATA u16 sTrainerA = 0;
static EWRAM_DATA u16 sTrainerB = 0;
static EWRAM_DATA u8 sStartFlags = 0;
static EWRAM_DATA u8 sStartParts = 0;        // bit 0 / bit 1: which parts arrived
static EWRAM_DATA u8 sStartSession = 0;
static EWRAM_DATA u8 sStartSlot = 0;
static EWRAM_DATA u16 sGuestPendingAge = 0;
static EWRAM_DATA u16 sGuestAcceptAge = 0;
static EWRAM_DATA bool8 sGuestAccepting = FALSE;
static EWRAM_DATA bool8 sRemoteAI = FALSE;
static EWRAM_DATA u16 sWaitFrames = 0;
static EWRAM_DATA u16 sBattleFrame = 0;
// A fingerprint of the battle at the start of each turn, kept for a few turns
// so it can be compared with the other game's.
#define HASH_TURNS 4
static EWRAM_DATA u8 sHashTurn[HASH_TURNS] = {0};
static EWRAM_DATA u16 sHashValue[HASH_TURNS] = {0};
static EWRAM_DATA bool8 sHashUsed[HASH_TURNS] = {0};
static EWRAM_DATA u8 sLastTurnSeen = 0xFF;
static EWRAM_DATA u8 sLatestHashTurn = 0;
static EWRAM_DATA bool8 sHaveHash = FALSE;
static EWRAM_DATA struct RemoteChoice sOut[CHOICE_RING] = {0};
static EWRAM_DATA u8 sOutNext = 0;           // number of the next choice to be made
static EWRAM_DATA u8 sOutAcked = 0;          // choices before this one have arrived
static EWRAM_DATA u8 sResendCursor = 0;
static EWRAM_DATA struct RemoteChoice sIn[CHOICE_RING] = {0};
static EWRAM_DATA u8 sInNext = 0;            // number of the next choice to arrive
static EWRAM_DATA u8 sInRead = 0;            // number of the next choice to use

// ---------------------------------------------------------------------------
// The local team
// ---------------------------------------------------------------------------

static bool8 IsUsable(struct Pokemon *mon)
{
    return GetMonData(mon, MON_DATA_SPECIES) != SPECIES_NONE
        && !GetMonData(mon, MON_DATA_IS_EGG)
        && GetMonData(mon, MON_DATA_HP) != 0;
}

// Picks the first usable Pokémon, up to max, in party order.
static u8 BuildTeamOf(u8 *pick, u8 max)
{
    u8 count = 0;
    u8 i;

    for (i = 0; i < PARTY_SIZE && count < max; i++)
    {
        if (IsUsable(&gPlayerParty[i]))
            pick[count++] = i;
    }
    return count;
}

static u8 BuildMyTeam(u8 *pick)
{
    return BuildTeamOf(pick, TEAM_SIZE);
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
    u8 pick[SEND_SIZE];
    u8 count = BuildTeamOf(pick, SEND_SIZE);
    u16 i;
    u8 tag = 0;

    for (i = 0; i < SEND_SIZE; i++)
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

    for (i = 0; i < count; i++)
        sSendPick[i] = pick[i];
    sSendCount = count;
    sSendTag = tag;
    sSendValid = count != 0;
}

// ---------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------

// Builds an event byte by byte (the message structs are byte-aligned) and
// queues it for the link. FALSE when the transmit ring is busy.
static bool8 PushMessage(u8 kind, u8 b1, u8 b2, u8 b3, const u8 *data)
{
    struct RemoteEvent event;
    u8 *bytes = (u8 *)&event;
    u8 i;

    bytes[0] = kind;
    bytes[1] = b1;
    bytes[2] = b2;
    bytes[3] = b3;
    bytes[4] = gSaveBlock1Ptr->location.mapGroup;
    bytes[5] = gSaveBlock1Ptr->location.mapNum;
    for (i = 0; i < 6; i++)
        bytes[6 + i] = data[i];
    return RemotePlayer_TryPushTx(&event);
}

static void SendNextChunk(void)
{
    u8 data[CHUNK_BYTES];
    u8 i;

    for (i = 0; i < CHUNK_BYTES; i++)
        data[i] = SendByte(sSendCursor * CHUNK_BYTES + i);
    if (!PushMessage(REMOTE_EVENT_BPARTY, sSendCursor, sSendTag, RemotePlayer_NextTxSeq(), data))
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
        u8 data[6];

        if (award->sendsLeft == 0)
            continue;
        if (award->cooldown != 0)
        {
            award->cooldown--;
            continue;
        }

        data[0] = award->personality & 0xFF;
        data[1] = (award->personality >> 8) & 0xFF;
        data[2] = (award->personality >> 16) & 0xFF;
        data[3] = award->personality >> 24;
        data[4] = award->species & 0xFF;
        data[5] = award->species >> 8;
        if (!PushMessage(REMOTE_EVENT_BEXP, award->exp & 0xFF, award->exp >> 8, award->id, data))
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
    for (i = 0; i < SEND_SIZE; i++)
    {
        struct Pokemon *mon = &peer->mons[i];

        if (GetMonData(mon, MON_DATA_SANITY_IS_BAD_EGG) || GetMonData(mon, MON_DATA_IS_EGG)
         || GetMonData(mon, MON_DATA_SPECIES) == SPECIES_NONE
         || GetMonData(mon, MON_DATA_SPECIES) >= NUM_SPECIES)
            ZeroMonData(mon);
    }
    peer->readyTag = peer->asmTag;
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

static EWRAM_DATA u8 sLastSession = 0;

// Both players talked to the same trainer: each game starts as the host and
// asks the other to join. The game with the lower seed stays host; the other
// gives up its own start and takes part as the guest, inside the script it
// is already running, so the trainer's dialogue and flags play out as usual.
static EWRAM_DATA bool8 sYielded = FALSE;
static EWRAM_DATA bool8 sStartTimedOut = FALSE;
static EWRAM_DATA bool8 sWaitShown = FALSE;
static EWRAM_DATA bool8 sAwaitingNear = FALSE;
static EWRAM_DATA bool8 sNearGaveUp = FALSE;
static EWRAM_DATA u16 sNearWait = 0;

static const u8 sText_WaitingForPlayer[] = _("Waiting for the other player...");
static const u8 sText_WaitingNear[] = _("Waiting for the other player\nto come closer...");
static const u8 sText_NoReply[] = _("No reply. A computer partner\nwill take their place.");

// The message box of the field, as the link rooms use while they wait.
void RemoteBattle_ShowWaiting(u8 which)
{
    if (!sWaitShown)
    {
        LoadMessageBoxAndBorderGfx();
        DrawDialogueFrame(0, FALSE);
        sWaitShown = TRUE;
    }
    FillWindowPixelBuffer(0, PIXEL_FILL(1));
    AddTextPrinterParameterized(0, FONT_NORMAL, which == 2 ? sText_WaitingNear : (which == 1 ? sText_NoReply : sText_WaitingForPlayer), 0, 1, 0, NULL);
    CopyWindowToVram(0, COPYWIN_FULL);
}

void RemoteBattle_HideWaiting(void)
{
    if (!sWaitShown)
        return;
    sWaitShown = FALSE;
    ClearDialogWindowAndFrame(0, TRUE);
}

bool8 RemoteBattle_AwaitingNear(void)
{
    return sAwaitingNear;
}

bool8 RemoteBattle_StartTimedOut(void)
{
    return sStartTimedOut;
}
static EWRAM_DATA u8 sCompSession = 0;
static EWRAM_DATA u8 sCompParts = 0;
static EWRAM_DATA u32 sCompSeed = 0;
static EWRAM_DATA u8 sCompHostTag = 0;
static EWRAM_DATA u8 sCompGuestTag = 0;
static EWRAM_DATA u16 sCompTrainerA = 0;
static EWRAM_DATA u16 sCompTrainerB = 0;
static EWRAM_DATA u8 sCompFlags = 0;

bool8 RemoteBattle_Yielded(void)
{
    return sYielded;
}

static void HandleCompetingStart(u8 slot, const struct StartMessage *msg)
{
    if (msg->session == 0 || slot != sPartnerSlot)
        return;
    if (msg->session != sCompSession)
    {
        sCompSession = msg->session;
        sCompParts = 0;
    }
    if (msg->part == 0)
    {
        sCompSeed = msg->data[0] | (msg->data[1] << 8) | (msg->data[2] << 16) | ((u32)msg->data[3] << 24);
        sCompHostTag = msg->data[4];
        sCompGuestTag = msg->data[5];
        sCompParts |= 1;
    }
    else if (msg->part == 1)
    {
        sCompTrainerA = msg->data[0] | (msg->data[1] << 8);
        sCompTrainerB = msg->data[2] | (msg->data[3] << 8);
        sCompFlags = msg->data[4];
        sCompParts |= 2;
    }
    if (sCompParts != 3)
        return;

    // The same trainer, the same teams, and the other game's seed is lower.
    if (sCompTrainerA != sTrainerA || sCompTrainerB != sTrainerB)
        return;
    if (sCompGuestTag != sSendTag || sCompHostTag != sGuestTag || !sSendValid)
        return;
    if (sCompSeed >= sSeed)
        return;

    // Undo the host's setup and become the guest of the other game's battle.
    LoadPlayerParty();
    CalculatePlayerPartyCount();
    sAwaitingAck = FALSE;
    gRemoteBattleActive = FALSE;
    gRemoteHumanBattle = FALSE;
    sRole = ROLE_NONE;
    sYielded = TRUE;

    sSeed = sCompSeed;
    sHostTag = sCompHostTag;
    sGuestTag = sCompGuestTag;
    sStartFlags = sCompFlags;
    sStartSession = sCompSession;
    sStartSlot = slot;
    sGuestPendingAge = 0;
    sStartParts = 3;
    sCompParts = 0;
}

static void HandleStartMessage(u8 slot, const struct StartMessage *msg)
{
    if (sRole == ROLE_HOST && sAwaitingAck && !sStartAcked && gRemoteHumanBattle)
    {
        HandleCompetingStart(slot, msg);
        return;
    }
    if (sRole != ROLE_NONE || gMain.inBattle || gRemoteBattleActive)
        return;
    if (msg->session == 0 || msg->session == sLastSession)
        return;
    if (msg->session != sStartSession)
    {
        sStartSession = msg->session;
        sStartParts = 0;
        sStartSlot = slot;
        sGuestPendingAge = 0;
    }
    if (msg->part == 0)
    {
        sSeed = msg->data[0] | (msg->data[1] << 8) | (msg->data[2] << 16) | ((u32)msg->data[3] << 24);
        sHostTag = msg->data[4];
        sGuestTag = msg->data[5];
        sStartParts |= 1;
    }
    else if (msg->part == 1)
    {
        sTrainerA = msg->data[0] | (msg->data[1] << 8);
        sTrainerB = msg->data[2] | (msg->data[3] << 8);
        sStartFlags = msg->data[4];
        sStartParts |= 2;
    }
}

static void HandleSyncMessage(const struct SyncMessage *msg)
{
    if (sRole == ROLE_NONE || msg->session != sSession)
        return;
    if (sRole == ROLE_HOST && msg->startAck)
        sStartAcked = TRUE;
    // Only an acknowledgement for choices that were really sent counts.
    if ((u8)(msg->ackN - sOutAcked) <= (u8)(sOutNext - sOutAcked))
        sOutAcked = msg->ackN;

    // Both games must see the same battle at the start of every turn. If they
    // do not, they have drifted apart: stop trusting the other game's choices.
    if (msg->data[3] && gRemoteHumanBattle && !sRemoteAI)
    {
        u8 turn = msg->data[0];
        u8 slot = turn % HASH_TURNS;

        if (sHashUsed[slot] && sHashTurn[slot] == turn
         && sHashValue[slot] != (msg->data[1] | (msg->data[2] << 8)))
        {
            sRemoteAI = TRUE;
            PlaySE(SE_FAILURE);
        }
    }
}

static void HandleChoiceMessage(const struct ChoiceMessage *msg)
{
    struct RemoteChoice *choice;

    if (sRole == ROLE_NONE || msg->session != sSession)
        return;
    // Choices arrive in order; anything else is a repeat or has to wait.
    if (msg->n != sInNext || (u8)(sInNext - sInRead) >= CHOICE_RING)
        return;
    choice = &sIn[sInNext & (CHOICE_RING - 1)];
    choice->kind = msg->data[0];
    choice->a = msg->data[1];
    choice->b = msg->data[2];
    choice->c = msg->data[3];
    choice->d = msg->data[4];
    sInNext++;
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
    else if (kind == REMOTE_EVENT_BSTART)
        HandleStartMessage(slot, (const struct StartMessage *)event);
    else if (kind == REMOTE_EVENT_BSYNC)
        HandleSyncMessage((const struct SyncMessage *)event);
    else if (kind == REMOTE_EVENT_BCHOICE)
        HandleChoiceMessage((const struct ChoiceMessage *)event);
    else if (kind == REMOTE_EVENT_SOCIAL)
        RemoteSocial_HandleRx(slot, event);
}

// ---------------------------------------------------------------------------
// The human partner: messages that go out
// ---------------------------------------------------------------------------

static void ResetChoiceRings(void)
{
    u8 i;

    sOutNext = 0;
    sOutAcked = 0;
    sResendCursor = 0;
    sInNext = 0;
    sInRead = 0;
    sWaitFrames = 0;
    sRemoteAI = FALSE;
    sLastTurnSeen = 0xFF;
    sHaveHash = FALSE;
    for (i = 0; i < HASH_TURNS; i++)
        sHashUsed[i] = FALSE;
}

static void SendSync(void)
{
    u8 data[6];
    u8 i;

    for (i = 0; i < 6; i++)
        data[i] = 0;
    if (sHaveHash)
    {
        data[0] = sLatestHashTurn;
        data[1] = sHashValue[sLatestHashTurn % HASH_TURNS] & 0xFF;
        data[2] = sHashValue[sLatestHashTurn % HASH_TURNS] >> 8;
        data[3] = 1;
    }
    PushMessage(REMOTE_EVENT_BSYNC, sRole == ROLE_GUEST, sInNext, sSession, data);
}

static void SendChoice(u8 n)
{
    struct RemoteChoice *choice = &sOut[n & (CHOICE_RING - 1)];
    u8 data[6];

    data[0] = choice->kind;
    data[1] = choice->a;
    data[2] = choice->b;
    data[3] = choice->c;
    data[4] = choice->d;
    data[5] = 0;
    PushMessage(REMOTE_EVENT_BCHOICE, n, 0, sSession, data);
}

static void SendStart(void)
{
    u8 data[6];

    data[0] = sSeed & 0xFF;
    data[1] = (sSeed >> 8) & 0xFF;
    data[2] = (sSeed >> 16) & 0xFF;
    data[3] = sSeed >> 24;
    data[4] = sHostTag;
    data[5] = sGuestTag;
    PushMessage(REMOTE_EVENT_BSTART, 0, 0, sSession, data);

    data[0] = sTrainerA & 0xFF;
    data[1] = sTrainerA >> 8;
    data[2] = sTrainerB & 0xFF;
    data[3] = sTrainerB >> 8;
    data[4] = sStartFlags;
    data[5] = 0;
    PushMessage(REMOTE_EVENT_BSTART, 1, 0, sSession, data);
}

static void QueueOutgoing(u8 kind, u8 a, u8 b, u8 c, u8 d)
{
    struct RemoteChoice *choice;

    if ((u8)(sOutNext - sOutAcked) >= CHOICE_RING)
        return;     // the other game is far behind; it will give up first
    choice = &sOut[sOutNext & (CHOICE_RING - 1)];
    choice->kind = kind;
    choice->a = a;
    choice->b = b;
    choice->c = c;
    choice->d = d;
    SendChoice(sOutNext);
    sOutNext++;
}

// ---------------------------------------------------------------------------
// Choices made in the battle menu
// ---------------------------------------------------------------------------

// Versus: each game plays its own player as battler 0 against the other's
// team on battler 1, so the one who asked and the one who was asked see the
// same kind of battle. The guest's engine is the mirror image of the host's.
static bool8 IsLocalBattler(u8 battler)
{
    if (gRemoteVersus)
        return !(battler & 1);      // doubles: 0 and 2 are mine, 1 and 3 theirs
    return battler == (sRole == ROLE_GUEST ? 2 : 0);
}

static bool8 IsRemoteBattler(u8 battler)
{
    if (gRemoteVersus)
        return battler & 1;
    return battler == (sRole == ROLE_GUEST ? 0 : 2);
}

bool8 RemoteBattle_IsGuest(void)
{
    return gRemoteHumanBattle && !gRemoteVersus && sRole == ROLE_GUEST;
}

static bool8 IsMirrored(void)
{
    return gRemoteVersus && sRole == ROLE_GUEST;
}

// A speed tie is settled by a coin toss. In the mirrored engine the two
// battlers swap places, so the toss is turned around to pick the same
// Pokémon in both games.
bool8 RemoteBattle_TieRoll(void)
{
    u16 roll = Random() & 1;

    if (IsMirrored())
        roll ^= 1;
    return roll;
}

bool8 RemoteBattle_ControlsBattler(u8 battler)
{
    return gRemoteHumanBattle && !sRemoteAI && IsRemoteBattler(battler);
}

// Nothing is blocked any more: the guest's party menu is laid out from the
// guest's own flank, and item effects are relayed (see below).
bool8 RemoteBattle_BlockAction(u8 cursor)
{
    // The other player's bag cannot be replayed on the opponent's side.
    return gRemoteVersus && cursor == 1;
}

// An item used from the bag changes the user's party in the menu itself. The
// other game never sees that menu, so what the item did is sent along with
// the item and applied there too (the bag count stays with the user).
static EWRAM_DATA u8 sItemParty = 0;    // party id + 1, 0 when nothing was applied
static EWRAM_DATA u8 sItemMove = 0;

void RemoteBattle_ClearItemEffect(void)
{
    sItemParty = 0;
    sItemMove = 0;
}

void RemoteBattle_NoteItemEffect(u8 partyId, u8 moveIndex)
{
    if (!gRemoteHumanBattle || !gMain.inBattle)
        return;
    sItemParty = partyId + 1;
    sItemMove = moveIndex;
}

void RemoteBattle_ApplyItemEffect(u8 battler, u16 item, u8 partyPlus1, u8 moveIndex)
{
    u8 savedActive = gActiveBattler;
    u8 savedMenu = gBattlerInMenuId;

    if (partyPlus1 == 0 || partyPlus1 > PARTY_SIZE)
        return;
    gBattlerInMenuId = battler;
    PokemonUseItemEffects(&gPlayerParty[partyPlus1 - 1], item, partyPlus1 - 1, moveIndex, FALSE);
    gActiveBattler = savedActive;
    gBattlerInMenuId = savedMenu;
}

void RemoteBattle_CaptureTwoReturn(u8 battler, u8 ret8, u16 ret16)
{
    if (!gRemoteHumanBattle || sRemoteAI || !IsLocalBattler(battler))
        return;
    switch (ret8)
    {
    case B_ACTION_USE_MOVE:
    case B_ACTION_USE_ITEM:
    case B_ACTION_SWITCH:
    case B_ACTION_RUN:
    case B_ACTION_CANCEL_PARTNER:
    case B_ACTION_EXEC_SCRIPT:
        QueueOutgoing(CHOICE_TWO, ret8, ret16 & 0xFF, ret16 >> 8, 0);
        break;
    }
}

void RemoteBattle_CaptureChosenMon(u8 battler, u8 partyId, const u8 *order)
{
    if (!gRemoteHumanBattle || sRemoteAI || !IsLocalBattler(battler))
        return;
    if (order != NULL)
        QueueOutgoing(CHOICE_MON, partyId, order[0], order[1], order[2]);
    else
        QueueOutgoing(CHOICE_MON, partyId, 0, 1, 2);
}

void RemoteBattle_CaptureOne(u8 battler, u16 ret)
{
    if (!gRemoteHumanBattle || sRemoteAI || !IsLocalBattler(battler))
        return;
    if (ret == 0)
        RemoteBattle_ClearItemEffect();
    QueueOutgoing(CHOICE_ONE, ret & 0xFF, ret >> 8, sItemParty, sItemMove);
    RemoteBattle_ClearItemEffect();
}

bool8 RemoteBattle_PopChoice(struct RemoteChoice *choice)
{
    if (sInRead == sInNext)
        return FALSE;
    *choice = sIn[sInRead & (CHOICE_RING - 1)];
    sInRead++;
    sWaitFrames = 0;
    return TRUE;
}

bool8 RemoteBattle_WaitTimedOut(void)
{
    return ++sWaitFrames >= REMOTE_WAIT_FRAMES;
}

void RemoteBattle_GiveUpOnRemote(void)
{
    sRemoteAI = TRUE;
}

void RemoteBattle_ApplySeed(void)
{
    if (gRemoteHumanBattle)
        gRngValue = sSeed;
}

void RemoteBattle_BattleTick(void)
{
    if (!gRemoteHumanBattle || sRole == ROLE_NONE)
        return;

    sBattleFrame++;
    if (gBattleResults.battleTurnCounter != sLastTurnSeen
     && gBattlersCount == MAX_BATTLERS_COUNT)
    {
        u16 hash = gBattleResults.battleTurnCounter;
        u8 i;
        u8 slot;

        for (i = 0; i < gBattlersCount; i++)
        {
            u8 b = IsMirrored() ? (i ^ 1) : i;

            hash = hash * 31 + gBattleMons[b].hp + gBattleMons[b].species;
        }
        slot = gBattleResults.battleTurnCounter % HASH_TURNS;
        sHashTurn[slot] = gBattleResults.battleTurnCounter;
        sHashValue[slot] = hash;
        sHashUsed[slot] = TRUE;
        sLatestHashTurn = gBattleResults.battleTurnCounter;
        sHaveHash = TRUE;
        sLastTurnSeen = gBattleResults.battleTurnCounter;
    }
    RemoteSio_Tick();
    RemotePlayer_DrainBattleRx();
    if (sBattleFrame % SYNC_PERIOD_FRAMES == 0)
        SendSync();
    if (sOutAcked != sOutNext && sBattleFrame % CHOICE_RESEND_FRAMES == 0)
    {
        if ((u8)(sResendCursor - sOutAcked) >= (u8)(sOutNext - sOutAcked))
            sResendCursor = sOutAcked;
        SendChoice(sResendCursor);
        sResendCursor++;
    }
}

// ---------------------------------------------------------------------------
// Per frame (overworld)
// ---------------------------------------------------------------------------

void RemoteBattle_Init(void)
{
    u8 i;

    for (i = 0; i < REMOTE_MAX_PLAYERS; i++)
    {
        sPeers[i].ready = FALSE;
        sPeers[i].readyTag = 0;
        sPeers[i].gotCount = 0;
        sPeers[i].asmTag = 0;
    }
    sSendValid = FALSE;
    sSendCursor = 0;
}

bool8 RemoteBattle_AwaitingStartAck(void)
{
    return sAwaitingAck;
}

static void TickHostStart(void)
{
    if (!sAwaitingAck)
        return;
    if (sStartAcked)
    {
        sAwaitingAck = FALSE;
        return;
    }
    if (sStartWait % START_RESEND_FRAMES == 0)
        SendStart();
    if (++sStartWait >= START_TIMEOUT_FRAMES)
    {
        // Nobody answered: the partner is played by the game.
        sStartTimedOut = TRUE;
        sAwaitingAck = FALSE;
        gRemoteHumanBattle = FALSE;
        sRole = ROLE_NONE;
    }
}

static bool8 GuestCanJoin(void)
{
    struct PeerTeam *peer = &sPeers[sStartSlot];

    if (gMain.inBattle)
        return FALSE;
    // A game that was already starting the same fight is inside a script.
    if (!sYielded && (ArePlayerFieldControlsLocked() || ScriptContext_IsEnabled()))
        return FALSE;
    if (gPlayerAvatar.tileTransitionState != T_NOT_MOVING)
        return FALSE;
    if (sTrainerA == 0 || sTrainerA >= TRAINERS_COUNT)
        return FALSE;
    // Both games must build the same teams.
    if (!sSendValid || sSendTag != sGuestTag)
        return FALSE;
    return peer->ready && peer->readyTag == sHostTag;
}

static void StartGuestBattle(void)
{
    struct PeerTeam *peer = &sPeers[sStartSlot];
    u8 i;

    gTrainerBattleOpponent_A = sTrainerA;
    gTrainerBattleOpponent_B = sTrainerB;
    gBattleTypeFlags = BATTLE_TYPE_TRAINER | BATTLE_TYPE_DOUBLE | BATTLE_TYPE_TWO_OPPONENTS
                     | BATTLE_TYPE_MULTI | BATTLE_TYPE_INGAME_PARTNER;
    gRemoteSplitTrainer = (sStartFlags & FLAG_SPLIT) != 0;
    sRematch = (sStartFlags & FLAG_REMATCH) != 0;

    // The same battle party as the host's: their team in front, mine behind.
    SavePlayerParty();
    for (i = 0; i < PARTY_SIZE; i++)
        ZeroMonData(&gPlayerParty[i]);
    for (i = 0; i < TEAM_SIZE; i++)
    {
        gPlayerParty[i] = peer->mons[i];
        gPlayerParty[TEAM_SIZE + i] = sSendMons[i];
    }
    CalculatePlayerPartyCount();
    sPickCount = sSendCount > TEAM_SIZE ? TEAM_SIZE : sSendCount;
    for (i = 0; i < TEAM_SIZE; i++)
        sPick[i] = sSendPick[i];

    for (i = 0; i < NAME_BYTES; i++)
        sPartnerName[i] = peer->name[i];
    sPartnerName[NAME_BYTES] = EOS;
    sPartnerGender = RemotePlayer_GetGender(sStartSlot);
    gPartnerTrainerId = TRAINER_STEVEN_PARTNER;

    gRemoteBattleActive = TRUE;
    gRemoteHumanBattle = TRUE;
    ResetChoiceRings();
    sMoneyBefore = gSaveBlock1Ptr->money;
    if (!sYielded)
    {
        sRealCallback = NULL;
        gMain.savedCallback = CB2_RemoteBattleDone;
        sWrapped = TRUE;
    }
    BattleSetup_StartRemoteGuestBattle();
}

static void TickGuest(void)
{
    if (sRole == ROLE_GUEST && sGuestAccepting)
    {
        // Tell the host, a few times, before this game leaves the overworld.
        if (sGuestAcceptAge % 4 == 0)
            SendSync();
        if (++sGuestAcceptAge >= GUEST_ACCEPT_FRAMES)
        {
            sGuestAccepting = FALSE;
            StartGuestBattle();
        }
        return;
    }

    if (sRole != ROLE_NONE || sStartParts != 3)
        return;
    if (!sYielded && ++sGuestPendingAge > GUEST_PENDING_FRAMES)
    {
        sStartParts = 0;
        return;
    }
    if (!GuestCanJoin())
        return;

    sRole = ROLE_GUEST;
    sSession = sStartSession;
    sLastSession = sStartSession;
    sStartParts = 0;
    sGuestAccepting = TRUE;
    sGuestAcceptAge = 0;
    RemoteBattle_ShowWaiting(0);
    LockPlayerFieldControls();
}

static bool8 FindPartner(u8 *slot);

void RemoteBattle_Tick(void)
{
    sFrame++;
    if (!REMOTE_BATTLE_ENABLED)
        return;

    if (sFrame % REFRESH_PERIOD_FRAMES == 1 && !sAwaitingAck && !sGuestAccepting)
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

    if (sAwaitingNear)
    {
        u8 slot;

        if (FindPartner(&slot))
        {
            sAwaitingNear = FALSE;
            RemoteBattle_PrepareTrainerBattle();
        }
        else if (++sNearWait >= NEAR_TIMEOUT_FRAMES)
        {
            sAwaitingNear = FALSE;
            sNearGaveUp = TRUE;
            RemoteBattle_PrepareTrainerBattle();
        }
    }
    TickHostStart();
    TickGuest();
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
    return gTrainers[gTrainerBattleOpponent_A].partySize >= 1;
}

bool8 RemoteBattle_ShouldRematch(void)
{
    u8 slot;
    u8 pick[TEAM_SIZE];

    if (!REMOTE_BATTLE_ENABLED)
        return FALSE;
    if (gTrainerBattleOpponent_A == 0 || gTrainerBattleOpponent_A == TRAINER_SECRET_BASE)
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
    bool8 guest = sRole == ROLE_GUEST;

    if (gRemoteBattleActive)
    {
        // The party in use was the battle party: put the player's Pokémon
        // back where they were, with what they earned.
        for (i = 0; i < PARTY_SIZE; i++)
            sBattleParty[i] = gPlayerParty[i];
        LoadPlayerParty();
        for (i = 0; i < sPickCount; i++)
            gPlayerParty[sPick[i]] = sBattleParty[guest ? TEAM_SIZE + i : i];

        // Against the game's partner, the partner's experience goes by message.
        // With a person, their own game earned it.
        if (!gRemoteHumanBattle && !RemotePlayer_InLoopback())
        {
            for (i = 0; i < TEAM_SIZE; i++)
            {
                if (GetMonData(&sBattleParty[TEAM_SIZE + i], MON_DATA_SPECIES) != SPECIES_NONE)
                    QueueAward(&sBattleParty[TEAM_SIZE + i], sExpBefore[i]);
            }
        }
    }

    if (sRematch)
    {
        // A battle against a trainer already beaten pays nothing; a first
        // fight pays both players.
        if (gSaveBlock1Ptr->money > sMoneyBefore)
            gSaveBlock1Ptr->money = sMoneyBefore;
        sRematch = FALSE;
    }

    gRemoteBattleActive = FALSE;
    gRemoteSplitTrainer = FALSE;
    gRemoteHumanBattle = FALSE;
    sRole = ROLE_NONE;
    sRemoteAI = FALSE;
    sLockedSlot = REMOTE_MAX_PLAYERS;
    sWrapped = FALSE;

    if (sYielded)
    {
        // This game was inside the trainer's script: carry on with it.
        sYielded = FALSE;
        SetMainCallback2(sRealCallback);
    }
    else if (guest)
    {
        UnlockPlayerFieldControls();
        SetMainCallback2(gBattleOutcome == B_OUTCOME_WON ? CB2_ReturnToField : CB2_WhiteOut);
    }
    else
    {
        SetMainCallback2(sRealCallback);
    }
}

bool8 RemoteBattle_PrepareTrainerBattle(void)
{
    u8 slot;
    u8 i;
    struct PeerTeam *peer;
    bool8 human;

    gRemoteBattleActive = FALSE;
    gRemoteSplitTrainer = FALSE;
    gRemoteHumanBattle = FALSE;
    sRole = ROLE_NONE;
    sAwaitingAck = FALSE;
    sYielded = FALSE;
    sStartTimedOut = FALSE;
    sMoneyBefore = gSaveBlock1Ptr->money;

    sAwaitingNear = FALSE;
    if (REMOTE_BATTLE_ENABLED && !sNearGaveUp && !FindPartner(&slot) && TrainerFitsPartnerBattle())
    {
        u8 other;
        u8 otherGender;

        // Someone is on this map but too far away: give them time to walk up.
        if (RemotePlayer_FindNearby(60, &other, &otherGender) && sPeers[other].ready)
        {
            sAwaitingNear = TRUE;
            sNearWait = 0;
            return FALSE;
        }
    }
    sNearGaveUp = FALSE;

    if (!REMOTE_BATTLE_ENABLED || !FindPartner(&slot) || !TrainerFitsPartnerBattle())
        goto finish;
    peer = &sPeers[slot];

    // With a person on the other end both games build the battle from the
    // team snapshots that were sent; otherwise the live party is used.
    human = !RemotePlayer_InLoopback() && sSendValid && peer->readyTag != 0;
    if (human)
    {
        sPickCount = sSendCount > TEAM_SIZE ? TEAM_SIZE : sSendCount;
        for (i = 0; i < TEAM_SIZE; i++)
            sPick[i] = sSendPick[i];
    }
    else
    {
        sPickCount = BuildMyTeam(sPick);
    }
    if (sPickCount == 0)
        goto finish;

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
        gPlayerParty[i] = human ? sSendMons[i] : gSaveBlock1Ptr->playerParty[sPick[i]];
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

    if (human)
    {
        // Ask the other game to run the same battle.
        sRole = ROLE_HOST;
        sSessionCounter++;
        if (sSessionCounter == 0)
            sSessionCounter = 1;
        sSession = sSessionCounter;
        sSeed = Random() | ((u32)Random() << 16);
        sHostTag = sSendTag;
        sGuestTag = peer->readyTag;
        sTrainerA = gTrainerBattleOpponent_A;
        sTrainerB = gTrainerBattleOpponent_B;
        sStartFlags = (gRemoteSplitTrainer ? FLAG_SPLIT : 0) | (sRematch ? FLAG_REMATCH : 0);
        sPartnerSlot = slot;
        sStartAcked = FALSE;
        sStartWait = 0;
        sAwaitingAck = TRUE;
        gRemoteHumanBattle = TRUE;
        ResetChoiceRings();
    }

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

u8 RemoteBattle_GetTeamTag(void)
{
    return sSendValid ? sSendTag : 0;
}

static const u8 sVersusClass[] = _("TRAINER");

const u8 *RemoteBattle_VersusClassName(void)
{
    return sVersusClass;
}

u8 RemoteBattle_VersusTrainerPic(void)
{
    return sPartnerGender == FEMALE ? TRAINER_PIC_MAY : TRAINER_PIC_BRENDAN;
}

// The other player's team plays the opponent's side.
void RemoteBattle_FillVersusParty(void)
{
    u8 i;

    ZeroEnemyPartyMons();
    for (i = 0; i < SEND_SIZE; i++)
        gEnemyParty[i] = sVersusEnemy[i];
}

static void CB2_VersusDone(void)
{
    // A friendly battle: nothing that happened in it stays.
    LoadPlayerParty();
    CalculatePlayerPartyCount();
    gSaveBlock1Ptr->money = sMoneyBefore;

    gRemoteHumanBattle = FALSE;
    gRemoteVersus = FALSE;
    gRemoteBattleActive = FALSE;
    sRole = ROLE_NONE;
    sRemoteAI = FALSE;
    gTrainerBattleOpponent_A = 0;
    gTrainerBattleOpponent_B = 0;
    SetMainCallback2(CB2_ReturnToFieldContinueScriptPlayMapMusic);
}

bool8 RemoteBattle_BeginVersus(const struct VersusRequest *request)
{
    struct PeerTeam *peer;
    u8 i;

    if (!REMOTE_BATTLE_ENABLED || request->slot >= REMOTE_MAX_PLAYERS || gMain.inBattle)
        return FALSE;
    peer = &sPeers[request->slot];
    // Both games must build the battle from the teams both sides saw.
    if (!sSendValid || !peer->ready || request->myTag == 0
     || sSendTag != request->myTag || peer->readyTag != request->peerTag)
        return FALSE;

    for (i = 0; i < SEND_SIZE; i++)
        sVersusEnemy[i] = peer->mons[i];
    for (i = 0; i < NAME_BYTES; i++)
        sPartnerName[i] = peer->name[i];
    sPartnerName[NAME_BYTES] = EOS;
    sPartnerGender = RemotePlayer_GetGender(request->slot);
    sMoneyBefore = gSaveBlock1Ptr->money;

    SavePlayerParty();
    for (i = 0; i < PARTY_SIZE; i++)
        ZeroMonData(&gPlayerParty[i]);
    for (i = 0; i < SEND_SIZE; i++)
        gPlayerParty[i] = sSendMons[i];
    CalculatePlayerPartyCount();

    sRole = request->requester ? ROLE_HOST : ROLE_GUEST;
    sSession = request->session;
    sSeed = request->seed;
    sPartnerSlot = request->slot;
    sAwaitingAck = FALSE;
    sStartAcked = TRUE;
    ResetChoiceRings();
    gRemoteBattleActive = FALSE;
    gRemoteSplitTrainer = FALSE;
    gRemoteHumanBattle = TRUE;
    gRemoteVersus = TRUE;

    gTrainerBattleOpponent_A = TRAINER_SAWYER_1;
    gTrainerBattleOpponent_B = 0;
    gBattleTypeFlags = BATTLE_TYPE_TRAINER | BATTLE_TYPE_DOUBLE;
    gMain.savedCallback = CB2_VersusDone;
    LockPlayerFieldControls();
    ScriptContext_Stop();
    CreateBattleStartTask(B_TRANSITION_BLUR, 0);
    return TRUE;
}

const u8 *RemoteBattle_GetNameOfSlot(u8 slot)
{
    if (slot >= REMOTE_MAX_PLAYERS || !sPeers[slot].ready)
        return NULL;
    return sPeers[slot].name;
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
