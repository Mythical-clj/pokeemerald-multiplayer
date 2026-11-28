#include "global.h"
#include "malloc.h"
#include "bg.h"
#include "battle.h"
#include "data.h"
#include "event_data.h"
#include "gpu_regs.h"
#include "main.h"
#include "menu.h"
#include "overworld.h"
#include "battle_main.h"
#include "link.h"
#include "load_save.h"
#include "pokemon.h"
#include "recorded_battle.h"
#include "player_look.h"
#include "battle_setup.h"
#include "palette.h"
#include "script.h"
#include "sound.h"
#include "sprite.h"
#include "string_util.h"
#include "task.h"
#include "text.h"
#include "window.h"
#include "remote_battle.h"
#include "remote_player.h"
#include "remote_sio.h"
#include "remote_social.h"
#include "remote_spectate.h"
#include "constants/battle.h"
#include "constants/characters.h"
#include "constants/rgb.h"
#include "constants/species.h"
#include "constants/opponents.h"
#include "constants/songs.h"

// Spectating: nobody can join a running battle, so the fighters tell the
// watcher what is on their screen: the four Pokémon (name, level, HP,
// status) and the battle text. A watcher gets all of it the moment it joins,
// including the last few lines, and then follows in real time.

#define LINE_MAX            96      // bytes of one battle text line, with the end mark
#define LINE_PARTS          (LINE_MAX / 8)
#define RECENT_LINES        3       // sent again to a new watcher
#define VIEW_LINES          3       // kept on the watcher's side

#define INFO_PERIOD_FRAMES      60
#define INFO_PERIOD_WATCHED     30
#define JOIN_PERIOD_FRAMES      30
#define WATCHED_FRAMES          150     // somebody asked within the last 2.5 s
#define FIGHT_FRESH_FRAMES      180     // heard from a fighter within 3 s
#define LOST_FRAMES             300     // the fighter said nothing for 5 s: it is over
#define OVER_SHOW_FRAMES        90

// ---------------------------------------------------------------------------
// Wire
// ---------------------------------------------------------------------------

static bool8 SameBytes(const u8 *a, const u8 *b, u8 len)
{
    u8 i;

    for (i = 0; i < len; i++)
    {
        if (a[i] != b[i])
            return FALSE;
    }
    return TRUE;
}

static bool8 SpecSend(u8 sub, u8 a, u8 b, const u8 *data8)
{
    struct RemoteEvent event;
    u8 *bytes = (u8 *)&event;
    u8 i;

    bytes[0] = REMOTE_EVENT_SOCIAL;
    bytes[1] = sub;
    bytes[2] = a;
    bytes[3] = b;
    for (i = 0; i < 8; i++)
        bytes[4 + i] = data8[i];
    return RemotePlayer_TryPushTx(&event);
}

// Same recipe as the mail tag: both ends work it out from the name.
static u16 NameTagOf(const u8 *name)
{
    u16 tag = 0x1234;
    u8 i;

    for (i = 0; i < PLAYER_NAME_LENGTH && name[i] != EOS; i++)
        tag = (u16)(tag * 31 + name[i]);
    return tag;
}

// ---------------------------------------------------------------------------
// Fighter side
// ---------------------------------------------------------------------------

static EWRAM_DATA u8 sLastMon[4][8] = {0};
static EWRAM_DATA bool8 sLastMonValid[4] = {0};
static EWRAM_DATA u8 sRecent[RECENT_LINES][LINE_MAX] = {0};
static EWRAM_DATA u8 sRecentLen[RECENT_LINES] = {0};    // with the end mark
static EWRAM_DATA u8 sRecentSeq[RECENT_LINES] = {0};
static EWRAM_DATA u8 sRecentSent[RECENT_LINES] = {0};   // parts already sent
static EWRAM_DATA u8 sRecentCount = 0;
static EWRAM_DATA u8 sNextSeq = 0;
static EWRAM_DATA u32 sWatchedUntil[REMOTE_MAX_PLAYERS] = {0};   // per watcher (by slot)
static EWRAM_DATA u8 sTurnSlot = 0;
static EWRAM_DATA u32 sLastJoin = 0;
static EWRAM_DATA u32 sLastInfoSent = 0;
static EWRAM_DATA u16 sRecSent[REMOTE_MAX_PLAYERS][4] = {{0}};
static EWRAM_DATA u16 sRecSample[4][6] = {{0}};     // sizes 15 frames apart: only the oldest is sent
static EWRAM_DATA u32 sLastSample = 0;
static EWRAM_DATA bool8 sSetupDone[REMOTE_MAX_PLAYERS] = {0};
static EWRAM_DATA u8 sSetupCursor[REMOTE_MAX_PLAYERS] = {0};
static EWRAM_DATA u16 sPrevAck[REMOTE_MAX_PLAYERS][4] = {{0}};

#define SETUP_MONS      8
#define SETUP_PHRASE_BYTES  24      // the opponent's "won" and "lost" phrases
#define SETUP_CHUNKS    (1 + (SETUP_MONS * sizeof(struct Pokemon) + SETUP_PHRASE_BYTES) / 8)   // 104
#define NPC_HEADER_CHUNKS   3       // seed/flags, battle type + trainers, environment + counts
#define NPC_MAX_MONS        12      // six a side
#define SETUP_CHUNKS_MAX    (NPC_HEADER_CHUNKS + (NPC_MAX_MONS * sizeof(struct Pokemon) + 7) / 8)   // 153

// A fight against the game's own trainers is told the same way as a friendly
// battle between players: only the starting teams and what each side picks
// are sent, and the watcher plays the fight again.
static EWRAM_DATA bool8 sNpcFight = FALSE;
static EWRAM_DATA u8 sNpcPlayerMons = 0;
static EWRAM_DATA u8 sNpcEnemyMons = 0;

bool8 RemoteSpectate_Active(void)
{
    return gRemoteVersus || sNpcFight;
}

static u8 CountSavedMons(bool8 enemy)
{
    u8 n = 0;
    u8 i;

    for (i = 0; i < PARTY_SIZE; i++)
    {
        if (GetMonData((struct Pokemon *)RecordedBattle_GetSavedMon(enemy, i), MON_DATA_SPECIES, NULL) != SPECIES_NONE)
            n = i + 1;
    }
    return n;
}

// Called as a battle begins. Only a lone player's fight against trainers can
// be watched for now; the link has to be up with somebody on it.
void RemoteSpectate_NpcBattleBegin(void)
{
    sNpcFight = FALSE;
    if (gRemoteVersus || gRemoteHumanBattle)
        return;
    if (!(gBattleTypeFlags & BATTLE_TYPE_TRAINER)
     || (gBattleTypeFlags & (BATTLE_TYPE_RECORDED_INVALID | BATTLE_TYPE_FRONTIER | BATTLE_TYPE_INGAME_PARTNER | BATTLE_TYPE_MULTI)))
        return;
    if (!RemotePlayer_AnyoneKnown())
        return;
    RemoteSpectate_ResetSender();
    sNpcPlayerMons = 0;
    sNpcEnemyMons = 0;
    sNpcFight = TRUE;
}

// A fight with trainers keeps the link going and tells the others about it.
void RemoteSpectate_NpcTick(void)
{
    if (!sNpcFight)
        return;
    RemoteSio_Tick();
    RemotePlayer_DrainBattleRx();
    RemoteSpectate_BroadcastTick();
}

static u8 SetupTotal(void)
{
    if (!sNpcFight)
        return SETUP_CHUNKS;
    return NPC_HEADER_CHUNKS + (u8)(((sNpcPlayerMons + sNpcEnemyMons) * sizeof(struct Pokemon) + 7) / 8);
}

static void BuildNpcChunk(u8 cursor, u8 *chunk)
{
    u8 j;

    for (j = 0; j < 8; j++)
        chunk[j] = 0;
    if (cursor == 0)
    {
        u32 seed = gRecordedBattleRngSeed;

        chunk[0] = seed & 0xFF;
        chunk[1] = (seed >> 8) & 0xFF;
        chunk[2] = (seed >> 16) & 0xFF;
        chunk[3] = seed >> 24;
        chunk[4] = 8 | (gSaveBlock2Ptr->playerGender == FEMALE ? 2 : 0);
        chunk[5] = PlayerLook_Pack();
        chunk[7] = SetupTotal();
    }
    else if (cursor == 1)
    {
        u32 flags = gBattleTypeFlags & ~(BATTLE_TYPE_LINK_IN_BATTLE);

        chunk[0] = flags & 0xFF;
        chunk[1] = (flags >> 8) & 0xFF;
        chunk[2] = (flags >> 16) & 0xFF;
        chunk[3] = flags >> 24;
        chunk[4] = gTrainerBattleOpponent_A & 0xFF;
        chunk[5] = gTrainerBattleOpponent_A >> 8;
        chunk[6] = gTrainerBattleOpponent_B & 0xFF;
        chunk[7] = gTrainerBattleOpponent_B >> 8;
    }
    else if (cursor == 2)
    {
        chunk[0] = gBattleEnvironment;
        chunk[1] = sNpcPlayerMons;
        chunk[2] = sNpcEnemyMons;
    }
    else
    {
        for (j = 0; j < 8; j++)
        {
            u16 at = (cursor - NPC_HEADER_CHUNKS) * 8 + j;
            u16 size = sizeof(struct Pokemon);
            u8 mon = at / size;

            if (mon < sNpcPlayerMons + sNpcEnemyMons)
            {
                bool8 enemy = mon >= sNpcPlayerMons;
                const u8 *src = RecordedBattle_GetSavedMon(enemy, enemy ? mon - sNpcPlayerMons : mon);

                chunk[j] = src[at % size];
            }
        }
    }
}

void RemoteSpectate_ResetSender(void)
{
    u8 i;

    for (i = 0; i < 4; i++)
        sLastMonValid[i] = FALSE;
    sRecentCount = 0;
    sNextSeq = 0;
    sLastJoin = 0;
    sLastInfoSent = 0;
    for (i = 0; i < 4; i++)
    {
        u8 j;

        for (j = 0; j < 6; j++)
            sRecSample[i][j] = 0;
    }
    sLastSample = 0;
    for (i = 0; i < REMOTE_MAX_PLAYERS; i++)
    {
        u8 j;

        sWatchedUntil[i] = 0;
        sSetupDone[i] = FALSE;
        sSetupCursor[i] = 0;
        for (j = 0; j < 4; j++)
        {
            sRecSent[i][j] = 0;
            sPrevAck[i][j] = 0;
        }
    }
}

// Keeps what can be shown and drops anything that would wait for a button.
void RemoteSpectate_NoteText(const u8 *text)
{
    u8 tmp[LINE_MAX];
    u8 len = 0;
    u8 i;

    while (*text != EOS && len < LINE_MAX - 2)
    {
        u8 c = *text;

        if (c == EXT_CTRL_CODE_BEGIN)
        {
            u8 skip = GetExtCtrlCodeLength(text[1]);

            text++;
            text += skip != 0 ? skip : 1;
            continue;
        }
        if (c == CHAR_KEYPAD_ICON)
        {
            text += 2;
            continue;
        }
        if (c == CHAR_EXTRA_SYMBOL)
        {
            if (len < LINE_MAX - 3)
            {
                tmp[len++] = c;
                tmp[len++] = text[1];
            }
            text += 2;
            continue;
        }
        if (c == CHAR_PROMPT_SCROLL || c == CHAR_PROMPT_CLEAR || c == CHAR_NEWLINE)
        {
            if (len != 0 && tmp[len - 1] != CHAR_NEWLINE)
                tmp[len++] = CHAR_NEWLINE;
            text++;
            continue;
        }
        if (c >= 0xF7)
        {
            text++;
            continue;
        }
        tmp[len++] = c;
        text++;
    }
    while (len != 0 && tmp[len - 1] == CHAR_NEWLINE)
        len--;
    if (len == 0)
        return;
    tmp[len++] = EOS;

    if (sRecentCount != 0)
    {
        u8 last = sRecentCount - 1;

        if (sRecentLen[last] == len && SameBytes(sRecent[last], tmp, len))
            return;
    }
    if (sRecentCount == RECENT_LINES)
    {
        for (i = 1; i < RECENT_LINES; i++)
        {
            memcpy(sRecent[i - 1], sRecent[i], LINE_MAX);
            sRecentLen[i - 1] = sRecentLen[i];
            sRecentSeq[i - 1] = sRecentSeq[i];
            sRecentSent[i - 1] = sRecentSent[i];
        }
        sRecentCount--;
    }
    memcpy(sRecent[sRecentCount], tmp, len);
    sRecentLen[sRecentCount] = len;
    sRecentSeq[sRecentCount] = ++sNextSeq;
    sRecentSent[sRecentCount] = 0;
    sRecentCount++;
}

static void BuildMon(u8 battler, u8 *out)
{
    struct BattlePokemon *m = &gBattleMons[battler];
    u8 st = 0;

    memset(out, 0, 8);
    if (m->species == SPECIES_NONE || (m->hp == 0 && m->maxHP == 0))
        return;
    if (m->status1 & STATUS1_SLEEP)
        st = 1;
    else if (m->status1 & STATUS1_PSN_ANY)
        st = 2;
    else if (m->status1 & STATUS1_BURN)
        st = 3;
    else if (m->status1 & STATUS1_PARALYSIS)
        st = 4;
    else if (m->status1 & STATUS1_FREEZE)
        st = 5;
    out[0] = m->species & 0xFF;
    out[1] = m->species >> 8;
    out[2] = m->level;
    out[3] = m->hp & 0xFF;
    out[4] = m->hp >> 8;
    out[5] = m->maxHP & 0xFF;
    out[6] = m->maxHP >> 8;
    out[7] = st;
}

// Every frame of a versus battle.
void RemoteSpectate_BroadcastTick(void)
{
    u32 now = gMain.vblankCounter1;
    bool8 watched = FALSE;
    u8 w = 0;
    u8 i;

    // Watchers are served in turns: one message per frame, for one of them.
    for (i = 1; i <= REMOTE_MAX_PLAYERS; i++)
    {
        u8 cand = (sTurnSlot + i) % REMOTE_MAX_PLAYERS;

        if (sWatchedUntil[cand] != 0 && now < sWatchedUntil[cand])
        {
            if (!watched)
                w = cand;
            watched = TRUE;
        }
    }
    sTurnSlot = w;

    // Only what can no longer be taken back is sent: while the players are
    // still choosing, the newest bytes may still be cancelled.
    if (!BattleIsSelectingActions())
    {
        for (i = 0; i < 4; i++)
            sRecSample[i][0] = RecordedBattle_RecordSize(i);
    }
    if (now - sLastInfoSent >= (watched ? INFO_PERIOD_WATCHED : INFO_PERIOD_FRAMES))
    {
        u8 name[8];
        const u8 *opp = sNpcFight ? gTrainers[gTrainerBattleOpponent_A].trainerName : RemoteBattle_GetPartnerName();

        for (i = 0; i < 8; i++)
            name[i] = (opp != NULL && i < PLAYER_NAME_LENGTH) ? opp[i] : EOS;
        // the name ends with an end mark inside the 8 bytes
        for (i = 0, name[7] = EOS; i < 7; i++)
        {
            if (name[i] == EOS)
                break;
        }
        if (SpecSend(SUB_SPEC_INFO, 1, 0, name))
            sLastInfoSent = now;
        return;
    }
    if (!watched)
        return;

    for (i = 0; i < 4; i++)
    {
        u8 mon[8];

        BuildMon(i, mon);
        if (!sLastMonValid[i] || !SameBytes(mon, sLastMon[i], 8))
        {
            if (SpecSend(SUB_SPEC_MON, i, 0, mon))
            {
                memcpy(sLastMon[i], mon, 8);
                sLastMonValid[i] = TRUE;
            }
            return;
        }
    }
    if (!sSetupDone[w])
    {
        u8 chunk[8];
        u8 j;

        if (sNpcFight)
        {
            if (sNpcPlayerMons == 0)
            {
                sNpcPlayerMons = CountSavedMons(FALSE);
                sNpcEnemyMons = CountSavedMons(TRUE);
                if (sNpcPlayerMons + sNpcEnemyMons > NPC_MAX_MONS)
                    sNpcEnemyMons = NPC_MAX_MONS - sNpcPlayerMons;
            }
            BuildNpcChunk(sSetupCursor[w], chunk);
        }
        else if (sSetupCursor[w] == 0)
        {
            u32 seed = RemoteBattle_GetSeed();

            for (j = 0; j < 8; j++)
                chunk[j] = 0;
            chunk[0] = seed & 0xFF;
            chunk[1] = (seed >> 8) & 0xFF;
            chunk[2] = (seed >> 16) & 0xFF;
            chunk[3] = seed >> 24;
            chunk[4] = (RemoteBattle_IsMirroredVersus() ? 1 : 0)
                     | (gSaveBlock2Ptr->playerGender == FEMALE ? 2 : 0)
                     | (RemoteBattle_GetPartnerGender() == FEMALE ? 4 : 0);
            chunk[5] = PlayerLook_Pack();
            chunk[6] = RemoteBattle_GetVersusLook();
            chunk[7] = SETUP_CHUNKS;
        }
        else
        {
            for (j = 0; j < 8; j++)
            {
                u16 at = (sSetupCursor[w] - 1) * 8 + j;

                if (at < SETUP_MONS * sizeof(struct Pokemon))
                {
                    u8 mon = at / sizeof(struct Pokemon);
                    const u8 *src = RecordedBattle_GetSavedMon(mon >= 4, mon & 3);

                    chunk[j] = src[at % sizeof(struct Pokemon)];
                }
                else
                {
                    u8 p = at - SETUP_MONS * sizeof(struct Pokemon);
                    u16 word = RemoteBattle_GetOppPhraseWord(p / 12, (p % 12) / 2);

                    chunk[j] = (p & 1) ? word >> 8 : word & 0xFF;
                }
            }
        }
        if (SpecSend(SUB_SPEC_SETUP, sSetupCursor[w], 0, chunk))
            sSetupCursor[w] = (sSetupCursor[w] + 1) % SetupTotal();
        return;
    }
    for (i = 0; i < 4; i++)
    {
        u16 stable = sRecSample[i][0];

        if (sRecSent[w][i] > stable)
            sRecSent[w][i] = stable;
        if (sRecSent[w][i] < stable)
        {
            u8 data[8];
            u8 n = stable - sRecSent[w][i];
            u8 j;

            if (n > 6)
                n = 6;
            data[0] = sRecSent[w][i] & 0xFF;
            data[1] = sRecSent[w][i] >> 8;
            for (j = 0; j < 6; j++)
                data[2 + j] = j < n ? RecordedBattle_RecordByte(i, sRecSent[w][i] + j) : 0;
            if (SpecSend(SUB_SPEC_REC, i, n, data))
                sRecSent[w][i] += n;
            return;
        }
    }
    for (i = 0; i < sRecentCount; i++)
    {
        u8 parts = (sRecentLen[i] + 7) / 8;

        if (sRecentSent[i] < parts)
        {
            u8 chunk[8];
            u8 j;

            for (j = 0; j < 8; j++)
            {
                u8 at = sRecentSent[i] * 8 + j;

                chunk[j] = at < sRecentLen[i] ? sRecent[i][at] : EOS;
            }
            if (SpecSend(SUB_SPEC_TEXT, sRecentSeq[i], sRecentSent[i], chunk))
                sRecentSent[i]++;
            return;
        }
    }
}

#define MAX_SPECTATORS 4       // everyone else in a 6-player room

static void HandleJoin(u8 slot, const u8 *data)
{
    u32 now = gMain.vblankCounter1;
    u16 tag = data[0] | (data[1] << 8);
    u8 i;

    if (!RemoteSpectate_Active() || !gMain.inBattle)
        return;
    if (tag != NameTagOf(gSaveBlock2Ptr->playerName))
        return;
    // Up to four spectators (the other players in a 6-player room), served in turns.
    if (sWatchedUntil[slot] == 0 || now >= sWatchedUntil[slot])
    {
        u8 active = 0;

        for (i = 0; i < REMOTE_MAX_PLAYERS; i++)
        {
            if (i != slot && sWatchedUntil[i] != 0 && now < sWatchedUntil[i])
                active++;
        }
        if (active >= MAX_SPECTATORS)
            return;
    }
    // A watcher that was away for a moment gets everything again.
    if (sWatchedUntil[slot] == 0 || now >= sWatchedUntil[slot])
    {
        for (i = 0; i < 4; i++)
            sLastMonValid[i] = FALSE;
        for (i = 0; i < RECENT_LINES; i++)
            sRecentSent[i] = 0;
        for (i = 0; i < 4; i++)
        {
            sRecSent[slot][i] = 0;
            sPrevAck[slot][i] = 0;
        }
        sSetupDone[slot] = FALSE;
        sSetupCursor[slot] = 0;
    }
    sWatchedUntil[slot] = now + WATCHED_FRAMES;
    sLastJoin = now;
}

// ---------------------------------------------------------------------------
// Watcher side
// ---------------------------------------------------------------------------

struct ViewMon
{
    u16 species;
    u8 level;
    u8 status;
    u16 hp;
    u16 maxHp;
};

static EWRAM_DATA u32 sFightHeard[REMOTE_MAX_PLAYERS] = {0};
static EWRAM_DATA bool8 sWatching = FALSE;
static EWRAM_DATA u8 sWatchSlot = 0;
static EWRAM_DATA u8 sTargetName[PLAYER_NAME_LENGTH + 1] = {0};
static EWRAM_DATA u8 sOppName[PLAYER_NAME_LENGTH + 1] = {0};
static EWRAM_DATA struct ViewMon sView[4] = {0};
static EWRAM_DATA u8 sLines[VIEW_LINES][LINE_MAX] = {0};
static EWRAM_DATA u8 sLineSeq[VIEW_LINES] = {0};
static EWRAM_DATA u8 sLineCount = 0;
static EWRAM_DATA u8 sRxBuf[LINE_MAX] = {0};
static EWRAM_DATA u8 sRxSeq = 0;
static EWRAM_DATA u8 sRxNext = 0;
static EWRAM_DATA bool8 sRxActive = FALSE;
static EWRAM_DATA bool8 sDirty = FALSE;
static EWRAM_DATA bool8 sGotData = FALSE;
static EWRAM_DATA u32 sLastHeard = 0;
static EWRAM_DATA u8 sSetupMask[(SETUP_CHUNKS_MAX + 7) / 8] = {0};
static EWRAM_DATA u8 sSetupTotal = 0;
static EWRAM_DATA u8 sWatchEnv = 0;
static EWRAM_DATA u8 sSetupGot = 0;
static EWRAM_DATA bool8 sSetupReady = FALSE;
static EWRAM_DATA u16 sRecRecv[4] = {0};
static EWRAM_DATA bool8 sLaunched = FALSE;
static EWRAM_DATA bool8 sInBattleWatch = FALSE;
static EWRAM_DATA bool8 sQuit = FALSE;
static EWRAM_DATA u16 sFastTurn = 0;
static EWRAM_DATA u16 sFastFrames = 0;

bool8 RemoteSpectate_IsFighting(u8 slot)
{
    if (slot >= REMOTE_MAX_PLAYERS || sFightHeard[slot] == 0)
        return FALSE;
    return gMain.vblankCounter1 - sFightHeard[slot] < FIGHT_FRESH_FRAMES;
}

static void AddLine(u8 seq)
{
    u8 i;

    for (i = 0; i < sLineCount; i++)
    {
        if (sLineSeq[i] == seq)
            return;
    }
    if (sLineCount == VIEW_LINES)
    {
        for (i = 1; i < VIEW_LINES; i++)
        {
            memcpy(sLines[i - 1], sLines[i], LINE_MAX);
            sLineSeq[i - 1] = sLineSeq[i];
        }
        sLineCount--;
    }
    memcpy(sLines[sLineCount], sRxBuf, LINE_MAX);
    sLineSeq[sLineCount] = seq;
    sLineCount++;
    sDirty = TRUE;
}

void RemoteSpectate_HandleRx(u8 slot, const struct RemoteEvent *event)
{
    const u8 *m = (const u8 *)event;
    u8 sub = m[1];
    u8 a = m[2];
    u8 b = m[3];
    const u8 *data = m + 4;
    u32 now = gMain.vblankCounter1;
    u8 i;

    if (slot >= REMOTE_MAX_PLAYERS)
        return;
    if (sub == SUB_SPEC_JOIN)
    {
        HandleJoin(slot, data);
        return;
    }
    if (sub == SUB_SPEC_ACK)
    {
        if (RemoteSpectate_Active() && gMain.inBattle && b == (NameTagOf(gSaveBlock2Ptr->playerName) & 0xFF))
        {
            sWatchedUntil[slot] = now + WATCHED_FRAMES;
            if (a != 0)
                sSetupDone[slot] = TRUE;
            for (i = 0; i < 4; i++)
            {
                u16 got = data[i * 2] | (data[i * 2 + 1] << 8);

                // The same number twice in a row, and short of what was sent: lost on the way.
                if (got < sRecSent[slot][i] && got == sPrevAck[slot][i])
                    sRecSent[slot][i] = got;
                sPrevAck[slot][i] = got;
            }
        }
        return;
    }
    if (sub == SUB_SPEC_INFO)
    {
        sFightHeard[slot] = a != 0 ? (now | 1) : 0;
        if (sWatching && slot == sWatchSlot)
        {
            for (i = 0; i < PLAYER_NAME_LENGTH; i++)
                sOppName[i] = i < 8 ? data[i] : EOS;
            sOppName[PLAYER_NAME_LENGTH] = EOS;
            sLastHeard = now;
            sDirty = TRUE;
        }
        return;
    }
    if (!sWatching || slot != sWatchSlot)
        return;
    sLastHeard = now;
    if (sub == SUB_SPEC_SETUP)
    {
        u8 *buf = RemoteSocial_ScratchBuffer();

        if (a < SETUP_CHUNKS_MAX && !(sSetupMask[a / 8] & (1 << (a % 8))))
        {
            for (i = 0; i < 8; i++)
                buf[a * 8 + i] = data[i];
            sSetupMask[a / 8] |= 1 << (a % 8);
            sSetupGot++;
            if (a == 0 && data[7] != 0 && data[7] <= SETUP_CHUNKS_MAX)
                sSetupTotal = data[7];
            if ((sSetupMask[0] & 1) && sSetupGot >= sSetupTotal)
                sSetupReady = TRUE;
        }
        return;
    }
    if (sub == SUB_SPEC_REC)
    {
        u16 pos = data[0] | (data[1] << 8);

        if (a < 4 && b <= 6 && pos == sRecRecv[a])
        {
            for (i = 0; i < b; i++)
                RecordedBattle_LivePut(a, pos + i, data[2 + i]);
            sRecRecv[a] += b;
        }
        return;
    }
    if (sub == SUB_SPEC_MON)
    {
        if (a < 4)
        {
            sView[a].species = data[0] | (data[1] << 8);
            sView[a].level = data[2];
            sView[a].hp = data[3] | (data[4] << 8);
            sView[a].maxHp = data[5] | (data[6] << 8);
            sView[a].status = data[7];
            sGotData = TRUE;
            sDirty = TRUE;
        }
    }
    else if (sub == SUB_SPEC_TEXT)
    {
        if (b == 0)
        {
            sRxSeq = a;
            sRxNext = 0;
            sRxActive = TRUE;
        }
        if (!sRxActive || a != sRxSeq || b != sRxNext || b >= LINE_PARTS)
        {
            sRxActive = FALSE;
            return;
        }
        for (i = 0; i < 8; i++)
            sRxBuf[b * 8 + i] = data[i];
        sRxNext++;
        for (i = 0; i < 8; i++)
        {
            if (data[i] == EOS)
            {
                sRxActive = FALSE;
                AddLine(sRxSeq);
                sGotData = TRUE;
                break;
            }
        }
        if (sRxActive && sRxNext >= LINE_PARTS)
            sRxActive = FALSE;
    }
}

// ---------------------------------------------------------------------------
// The screen
// ---------------------------------------------------------------------------

#define COL_BG      0
#define COL_TEXT    1
#define COL_SHADOW  2
#define COL_GREEN   4
#define COL_YELLOW  5
#define COL_RED     6
#define COL_BAR_BG  7

static const struct BgTemplate sSpecBgTemplates[] =
{
    {
        .bg = 0,
        .charBaseIndex = 0,
        .mapBaseIndex = 31,
        .screenSize = 0,
        .paletteMode = 0,
        .priority = 0,
        .baseTile = 0,
    },
};

static const struct WindowTemplate sSpecWindows[] =
{
    {
        .bg = 0,
        .tilemapLeft = 0,
        .tilemapTop = 0,
        .width = 30,
        .height = 20,
        .paletteNum = 15,
        .baseBlock = 1,
    },
    DUMMY_WIN_TEMPLATE
};

static const u16 sSpecPalette[16] =
{
    RGB(31, 31, 31),    // 0 background
    RGB(2, 2, 3),       // 1 text
    RGB(20, 20, 22),    // 2 shadow
    RGB(10, 10, 12),    // 3
    RGB(6, 24, 8),      // 4 green
    RGB(28, 24, 4),     // 5 yellow
    RGB(28, 6, 6),      // 6 red
    RGB(24, 24, 25),    // 7 bar background
};

static const u8 sText_Watching[] = _("Watching ");
static const u8 sText_StopHint[] = _("B: Stop");
static const u8 sText_Lv[] = _("Lv");
static const u8 sText_Slash[] = _("/");
static const u8 sText_Waiting[] = _("Loading the battle...");
static const u8 sText_Over[] = _("The battle is over.");
static const u8 sText_Slp[] = _("SLP");
static const u8 sText_Psn[] = _("PSN");
static const u8 sText_Brn[] = _("BRN");
static const u8 sText_Par[] = _("PAR");
static const u8 sText_Frz[] = _("FRZ");

static EWRAM_DATA u8 *sTilemap = NULL;
static EWRAM_DATA u8 sState = 0;
static EWRAM_DATA u32 sLastJoinSent = 0;
static EWRAM_DATA u32 sOverAt = 0;
static EWRAM_DATA bool8 sOver = FALSE;

static void SpecVBlank(void)
{
    LoadOam();
    ProcessSpriteCopyRequests();
    TransferPlttBuffer();
}

static void PrintAt(const u8 *text, u16 x, u16 y)
{
    u8 color[3] = { COL_BG, COL_TEXT, COL_SHADOW };

    AddTextPrinterParameterized4(0, FONT_NORMAL, x, y, 0, 0, color, 0, text);
}

static void DrawMonRow(u8 battler, u16 y, bool8 own)
{
    const struct ViewMon *v = &sView[battler];
    u8 buf[24];
    u8 *p;
    u16 fill;
    u8 color;

    if (v->species == SPECIES_NONE)
        return;
    PrintAt(gSpeciesNames[v->species], 0, y);
    p = StringCopy(buf, sText_Lv);
    ConvertIntToDecimalStringN(p, v->level, STR_CONV_MODE_LEFT_ALIGN, 3);
    PrintAt(buf, 80, y);
    switch (v->status)
    {
    case 1: PrintAt(sText_Slp, 112, y); break;
    case 2: PrintAt(sText_Psn, 112, y); break;
    case 3: PrintAt(sText_Brn, 112, y); break;
    case 4: PrintAt(sText_Par, 112, y); break;
    case 5: PrintAt(sText_Frz, 112, y); break;
    }
    FillWindowPixelRect(0, PIXEL_FILL(COL_BAR_BG), 142, y + 4, 40, 6);
    fill = v->maxHp != 0 ? (v->hp * 40) / v->maxHp : 0;
    if (fill == 0 && v->hp != 0)
        fill = 1;
    if (fill * 2 > 40)
        color = COL_GREEN;
    else if (fill * 5 > 40)
        color = COL_YELLOW;
    else
        color = COL_RED;
    if (fill != 0)
        FillWindowPixelRect(0, PIXEL_FILL(color), 142, y + 4, fill, 6);
    if (own)
    {
        p = ConvertIntToDecimalStringN(buf, v->hp, STR_CONV_MODE_LEFT_ALIGN, 3);
        p = StringCopy(p, sText_Slash);
        ConvertIntToDecimalStringN(p, v->maxHp, STR_CONV_MODE_LEFT_ALIGN, 3);
        PrintAt(buf, 186, y);
    }
}

static void Redraw(void)
{
    u8 buf[40];
    u8 *p;

    FillWindowPixelBuffer(0, PIXEL_FILL(COL_BG));
    p = StringCopy(buf, sText_Watching);
    StringCopy(p, sTargetName);
    PrintAt(buf, 0, 0);
    PrintAt(sText_StopHint, 190, 0);

    PrintAt(sOppName, 0, 20);
    DrawMonRow(1, 36, FALSE);
    DrawMonRow(3, 52, FALSE);
    FillWindowPixelRect(0, PIXEL_FILL(COL_SHADOW), 0, 74, 240, 1);
    PrintAt(sTargetName, 0, 80);
    DrawMonRow(0, 96, TRUE);
    DrawMonRow(2, 112, TRUE);
    FillWindowPixelRect(0, PIXEL_FILL(COL_SHADOW), 0, 132, 240, 1);
    if (sOver)
        PrintAt(sText_Over, 0, 136);
    else if (sLineCount != 0)
        PrintAt(sLines[sLineCount - 1], 0, 136);
    else
        PrintAt(sText_Waiting, 0, 136);
    CopyWindowToVram(0, COPYWIN_FULL);
    sDirty = FALSE;
}

static EWRAM_DATA bool8 sAckDue = FALSE;
static EWRAM_DATA u8 sSavedName[PLAYER_NAME_LENGTH + 1] = {0};
static EWRAM_DATA u8 sSavedGender = 0;
static EWRAM_DATA u8 sOppLook = 0;

bool8 RemoteSpectate_IsWatching(void)
{
    return sInBattleWatch;
}

u8 RemoteSpectate_OppLook(void)
{
    return sOppLook;
}

static void SendJoin(void)
{
    u8 data[8] = {0};
    u16 tag = NameTagOf(sTargetName);

    data[0] = tag & 0xFF;
    data[1] = tag >> 8;
    SpecSend(SUB_SPEC_JOIN, 0, 0, data);
}

static bool8 SendAck(void)
{
    u8 data[8];
    u8 i;

    for (i = 0; i < 4; i++)
    {
        data[i * 2] = sRecRecv[i] & 0xFF;
        data[i * 2 + 1] = sRecRecv[i] >> 8;
    }
    return SpecSend(SUB_SPEC_ACK, sSetupReady ? 1 : 0, NameTagOf(sTargetName) & 0xFF, data);
}

// Tells the fighters somebody is watching and what has arrived so far.
static void SendHeartbeats(u32 now)
{
    if (now - sLastJoinSent >= JOIN_PERIOD_FRAMES)
    {
        SendJoin();
        sLastJoinSent = now;
        sAckDue = TRUE;
    }
    else if (sAckDue && SendAck())
    {
        sAckDue = FALSE;
    }
}

static void CB2_SpectateDone(void)
{
    u8 i;

    sInBattleWatch = FALSE;
    sLaunched = FALSE;
    sWatching = FALSE;
    sFastTurn = 0;
    RecordedBattle_LiveEnd();
    RemoteBattle_SetSpectateMirror(FALSE);
    gRemoteVersus = FALSE;
    for (i = 0; i < PLAYER_NAME_LENGTH + 1; i++)
        gSaveBlock2Ptr->playerName[i] = sSavedName[i];
    gSaveBlock2Ptr->playerGender = sSavedGender;
    gHitMarker &= ~HITMARKER_NO_ANIMATIONS;
    LoadPlayerParty();
    CalculatePlayerPartyCount();
    gBattleTypeFlags = 0;
    gTrainerBattleOpponent_A = 0;
    gTrainerBattleOpponent_B = 0;
    SetMainCallback2(CB2_ReturnToFieldContinueScriptPlayMapMusic);
}

// A fight against the game's trainers: the same replay, with the real
// trainers, battle type and teams the fighter had.
static void LaunchNpcBattle(const u8 *buf)
{
    u32 seed = buf[0] | (buf[1] << 8) | (buf[2] << 16) | ((u32)buf[3] << 24);
    u32 flags = buf[8] | (buf[9] << 8) | (buf[10] << 16) | ((u32)buf[11] << 24);
    u8 playerMons = buf[17] > PARTY_SIZE ? PARTY_SIZE : buf[17];
    u8 enemyMons = buf[18] > PARTY_SIZE ? PARTY_SIZE : buf[18];
    u8 i;
    u16 j;

    RemoteBattle_SetSpectateMirror(FALSE);
    for (i = 0; i < PLAYER_NAME_LENGTH + 1; i++)
        sSavedName[i] = gSaveBlock2Ptr->playerName[i];
    sSavedGender = gSaveBlock2Ptr->playerGender;
    for (i = 0; i < PLAYER_NAME_LENGTH + 1; i++)
        gSaveBlock2Ptr->playerName[i] = sTargetName[i];
    gSaveBlock2Ptr->playerGender = (buf[4] & 2) ? FEMALE : MALE;
    gLinkPlayers[0].gender = gSaveBlock2Ptr->playerGender;
    gRecordedBattleMultiplayerId = 0;
    sOppLook = 0;
    sWatchEnv = buf[16];
    gRemoteVersus = FALSE;
    SavePlayerParty();
    ZeroPlayerPartyMons();
    ZeroEnemyPartyMons();
    for (i = 0; i < playerMons; i++)
    {
        u8 *mine = (u8 *)&gPlayerParty[i];

        for (j = 0; j < sizeof(struct Pokemon); j++)
            mine[j] = buf[24 + i * sizeof(struct Pokemon) + j];
    }
    for (i = 0; i < enemyMons; i++)
    {
        u8 *theirs = (u8 *)&gEnemyParty[i];

        for (j = 0; j < sizeof(struct Pokemon); j++)
            theirs[j] = buf[24 + (playerMons + i) * sizeof(struct Pokemon) + j];
    }
    CalculatePlayerPartyCount();
    CalculateEnemyPartyCount();
    gRecordedBattleRngSeed = seed;
    gTrainerBattleOpponent_A = buf[12] | (buf[13] << 8);
    gTrainerBattleOpponent_B = buf[14] | (buf[15] << 8);
    gBattleTypeFlags = (flags & ~(BATTLE_TYPE_LINK | BATTLE_TYPE_LINK_IN_BATTLE)) | BATTLE_TYPE_RECORDED;
    gPreBattleCallback1 = gMain.callback1;
    gMain.savedCallback = CB2_SpectateDone;
    sLaunched = TRUE;
    sInBattleWatch = TRUE;
    sQuit = FALSE;
    sFastTurn = 0;
    sLastHeard = gMain.vblankCounter1;
    FreeAllWindowBuffers();
    Free(sTilemap);
    sTilemap = NULL;
    SetMainCallback2(CB2_InitBattle);
}

u8 RemoteSpectate_WatchEnv(void)
{
    return sWatchEnv;
}

// Everything needed has arrived: play the battle from its start, the same
// way a recorded battle is played back. The recorded actions keep arriving
// while it runs; the controllers wait whenever they have caught up.
static void LaunchBattle(void)
{
    u8 *buf = RemoteSocial_ScratchBuffer();
    u32 seed = buf[0] | (buf[1] << 8) | (buf[2] << 16) | ((u32)buf[3] << 24);
    u8 i;
    u16 j;

    if (buf[4] & 8)
    {
        LaunchNpcBattle(buf);
        return;
    }
    RemoteBattle_SetSpectateMirror((buf[4] & 1) != 0);
    // The battle is shown as the watched player's: their name and looks stand
    // in for this player's until it is over.
    for (i = 0; i < PLAYER_NAME_LENGTH + 1; i++)
        sSavedName[i] = gSaveBlock2Ptr->playerName[i];
    sSavedGender = gSaveBlock2Ptr->playerGender;
    for (i = 0; i < PLAYER_NAME_LENGTH + 1; i++)
        gSaveBlock2Ptr->playerName[i] = sTargetName[i];
    gSaveBlock2Ptr->playerGender = (buf[4] & 2) ? FEMALE : MALE;
    gLinkPlayers[0].gender = gSaveBlock2Ptr->playerGender;
    gLinkPlayers[1].gender = (buf[4] & 4) ? FEMALE : MALE;
    gRecordedBattleMultiplayerId = 0;
    sOppLook = buf[6];
    for (i = 0; i < 2; i++)
    {
        for (j = 0; j < 6; j++)
        {
            u16 at = 8 + SETUP_MONS * sizeof(struct Pokemon) + i * 12 + j * 2;

            RemoteBattle_SetSpectatePhrase(i, j, buf[at] | (buf[at + 1] << 8));
        }
    }
    RemoteBattle_SetSpectatePartner(sOppName, gLinkPlayers[1].gender, buf[6]);
    gRemoteVersus = TRUE;
    SavePlayerParty();
    ZeroPlayerPartyMons();
    ZeroEnemyPartyMons();
    for (i = 0; i < 4; i++)
    {
        u8 *mine = (u8 *)&gPlayerParty[i];
        u8 *theirs = (u8 *)&gEnemyParty[i];

        for (j = 0; j < sizeof(struct Pokemon); j++)
        {
            mine[j] = buf[8 + i * sizeof(struct Pokemon) + j];
            theirs[j] = buf[8 + (4 + i) * sizeof(struct Pokemon) + j];
        }
    }
    CalculatePlayerPartyCount();
    gRecordedBattleRngSeed = seed;
    gTrainerBattleOpponent_A = TRAINER_SAWYER_1;
    gTrainerBattleOpponent_B = 0;
    gBattleTypeFlags = BATTLE_TYPE_TRAINER | BATTLE_TYPE_DOUBLE | BATTLE_TYPE_RECORDED;
    gPreBattleCallback1 = gMain.callback1;
    gMain.savedCallback = CB2_SpectateDone;
    sLaunched = TRUE;
    sInBattleWatch = TRUE;
    sQuit = FALSE;
    sFastTurn = 0;
    sLastHeard = gMain.vblankCounter1;
    FreeAllWindowBuffers();
    Free(sTilemap);
    sTilemap = NULL;
    SetMainCallback2(CB2_InitBattle);
}

u8 RemoteSpectate_FrameHook(void)
{
    u32 now = gMain.vblankCounter1;

    if (!sInBattleWatch)
        return 0;
    RemoteSio_Tick();
    RemotePlayer_DrainBattleRx();
    SendHeartbeats(now);
    if (now - sLastHeard >= 600)
        sQuit = TRUE;       // the fighter has gone quiet for 10 s
    if (JOY_NEW(R_BUTTON))
    {
        sFastTurn = gBattleResults.battleTurnCounter + 1;
        sFastFrames = 0;
    }
    if (sFastTurn != 0)
    {
        if (gBattleResults.battleTurnCounter >= sFastTurn || ++sFastFrames > 900)
        {
            sFastTurn = 0;
            gHitMarker &= ~HITMARKER_NO_ANIMATIONS;
        }
        else
        {
            gHitMarker |= HITMARKER_NO_ANIMATIONS;
            return 7;
        }
    }
    return 0;
}

bool8 RemoteSpectate_WantsQuit(void)
{
    return sInBattleWatch && sQuit;
}

static void CB2_Spectate(void)
{
    u32 now = gMain.vblankCounter1;

    switch (sState)
    {
    case 0:
        SetVBlankCallback(NULL);
        SetGpuReg(REG_OFFSET_DISPCNT, 0);
        ResetPaletteFade();
        ResetSpriteData();
        ResetTasks();
        ResetBgsAndClearDma3BusyFlags(0);
        InitBgsFromTemplates(0, sSpecBgTemplates, ARRAY_COUNT(sSpecBgTemplates));
        sTilemap = Alloc(BG_SCREEN_SIZE);
        memset(sTilemap, 0, BG_SCREEN_SIZE);
        SetBgTilemapBuffer(0, sTilemap);
        InitWindows(sSpecWindows);
        DeactivateAllTextPrinters();
        LoadPalette(sSpecPalette, BG_PLTT_ID(0), sizeof(sSpecPalette));
        LoadPalette(sSpecPalette, BG_PLTT_ID(15), sizeof(sSpecPalette));
        PutWindowTilemap(0);
        sOver = FALSE;
        sLastHeard = now;
        Redraw();
        ShowBg(0);
        BeginNormalPaletteFade(PALETTES_ALL, 0, 16, 0, RGB_BLACK);
        SetVBlankCallback(SpecVBlank);
        SetGpuReg(REG_OFFSET_DISPCNT, DISPCNT_OBJ_ON | DISPCNT_OBJ_1D_MAP);
        sState = 1;
        break;
    case 1:
        RemoteSio_Tick();
        RemotePlayer_DrainBattleRx();
        UpdatePaletteFade();
        if (!gPaletteFade.active)
            sState = 2;
        break;
    case 2:
        RemoteSio_Tick();
        RemotePlayer_DrainBattleRx();
        SendHeartbeats(now);
        if (sSetupReady && !sOver)
        {
            LaunchBattle();
            return;
        }
        if (!sOver && now - sLastHeard >= LOST_FRAMES)
        {
            sOver = TRUE;
            sOverAt = now;
            sDirty = TRUE;
        }
        if (sDirty)
            Redraw();
        if (JOY_NEW(B_BUTTON) || (sOver && now - sOverAt >= OVER_SHOW_FRAMES))
        {
            BeginNormalPaletteFade(PALETTES_ALL, 0, 0, 16, RGB_BLACK);
            sState = 3;
        }
        break;
    case 3:
        RemoteSio_Tick();
        RemotePlayer_DrainBattleRx();
        UpdatePaletteFade();
        if (!gPaletteFade.active)
        {
            sWatching = FALSE;
            RecordedBattle_LiveEnd();
            FreeAllWindowBuffers();
            Free(sTilemap);
            sTilemap = NULL;
            SetMainCallback2(CB2_ReturnToFieldContinueScriptPlayMapMusic);
        }
        break;
    }
    RunTasks();
    AnimateSprites();
    BuildOamBuffer();
}

void RemoteSpectate_Start(void)
{
    u8 slot = gSpecialVar_LastTalked - REMOTE_LOCAL_ID;
    const u8 *name = slot < REMOTE_MAX_PLAYERS ? RemoteBattle_GetNameOfSlot(slot) : NULL;
    u8 i;

    if (name == NULL)
    {
        ScriptContext_Enable();
        return;
    }
    sWatchSlot = slot;
    for (i = 0; i < PLAYER_NAME_LENGTH; i++)
        sTargetName[i] = name[i];
    sTargetName[PLAYER_NAME_LENGTH] = EOS;
    sOppName[0] = EOS;
    memset(sView, 0, sizeof(sView));
    sLineCount = 0;
    sRxActive = FALSE;
    sGotData = FALSE;
    sDirty = TRUE;
    sWatching = TRUE;
    sLastJoinSent = 0;
    sAckDue = FALSE;
    for (i = 0; i < 4; i++)
        sRecRecv[i] = 0;
    for (i = 0; i < sizeof(sSetupMask); i++)
        sSetupMask[i] = 0;
    sSetupGot = 0;
    sSetupTotal = SETUP_CHUNKS;
    sWatchEnv = BATTLE_ENVIRONMENT_BUILDING;
    sSetupReady = FALSE;
    sLaunched = FALSE;
    sInBattleWatch = FALSE;
    RecordedBattle_LiveBegin();
    sState = 0;
    PlayBGM(MUS_VS_TRAINER);
    SetMainCallback2(CB2_Spectate);
}
