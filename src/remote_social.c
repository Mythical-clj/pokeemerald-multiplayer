#include "global.h"
#include "easy_chat.h"
#include "evolution_scene.h"
#include "event_data.h"
#include "field_player_avatar.h"
#include "main.h"
#include "overworld.h"
#include "party_menu.h"
#include "pokedex.h"
#include "pokemon.h"
#include "constants/items.h"
#include "constants/songs.h"
#include "mail.h"
#include "random.h"
#include "sound.h"
#include "record_mixing.h"
#include "remote_battle.h"
#include "remote_player.h"
#include "remote_social.h"
#include "remote_spectate.h"
#include "script.h"
#include "strings.h"
#include "string_util.h"
#include "task.h"
#include "constants/characters.h"
#include "constants/easy_chat.h"

// ---------------------------------------------------------------------------
// Wire format: kind 15, the sub type in the second byte, the map in bytes
// 4-5 like every other event.
// ---------------------------------------------------------------------------

#define SUB_MAIL        1   // a = mail number, b = part (0-2: three words each, 3: recipient tag)
#define SUB_REQUEST     2   // a = SOCIAL_REQUEST_*, b = session
#define SUB_REPLY       3   // a = 1 yes / 0 no, b = session
#define SUB_CHUNK       4   // a = index, b = session, data = 6 bytes of a Pokémon (index OFFER_CHUNKS: checksum)
#define SUB_CANCEL      5   // b = session
#define SUB_OK          6   // b = session: "I accept the trade" / "I have your records"
#define SUB_MAILACK     9   // a = mail number, b = 0 delivered / 1 mailbox full, data[0-1] = recipient tag
#define SUB_DROP        10  // the sender landed from Fly
#define SUB_MIXCHUNK    7   // a = index, b = session, data = 8 bytes (the map bytes too) of the record (index = chunk count: checksum)

#define CHAT_WORDS          9
#define MAIL_PARTS          4
#define MAIL_REPEATS        600     // sent again until acknowledged: about 100 s (the other player may still be writing theirs)
#define MAIL_REPEAT_FRAMES  10

#define OFFER_CHUNKS    ((sizeof(struct Pokemon) + 5) / 6)
#define OFFER_MESSAGES  (OFFER_CHUNKS + 1)
#define OFFER_PERIOD    4
#define MIX_PERIOD      1
#define MIX_TIMEOUT_FRAMES      7200
#define REQUEST_RESEND_FRAMES   20
#define REQUEST_TIMEOUT_FRAMES  900     // the other player gets 15 s to answer
#define INCOMING_TIMEOUT_FRAMES 1800    // a request nobody had time to read
#define TRADE_TIMEOUT_FRAMES    7200    // choosing a Pokémon and deciding
#define CLOSING_FRAMES          150     // keep acknowledging after finishing
#define REPLY_REPEAT_FRAMES     10
#define REPLY_REPEATS           6

#define MAX_DISTANCE    12

enum
{
    ST_IDLE,
    ST_REQUESTING,      // asked, waiting for a reply
    ST_ASKED,           // was asked, the player is deciding
    ST_AGREED,          // both said yes
    ST_CLOSING,         // finished, still repeating the last acknowledgement
};

enum
{
    PHASE_NONE,
    PHASE_OFFERED,      // I picked a Pokémon and keep sending it
    PHASE_CONFIRMED,    // I accepted the trade
};

struct MailInbox
{
    u8 number;
    u8 gotMask;
    u8 deliveredNumber;
    u16 tag;
    u16 words[CHAT_WORDS];
};

static EWRAM_DATA u8 sState = ST_IDLE;
static EWRAM_DATA u8 sKind = 0;
static EWRAM_DATA u8 sPeer = 0;
static EWRAM_DATA u8 sSession = 0;
static EWRAM_DATA u8 sSessionCounter = 0;
static EWRAM_DATA u16 sTimer = 0;
static EWRAM_DATA u16 sFrame = 0;
static EWRAM_DATA u8 sReplyResult = 0;      // 0 waiting, 1 yes, 2 no, 3 no answer
static EWRAM_DATA u8 sReplyRepeats = 0;
static EWRAM_DATA u8 sReplyValue = 0;
static EWRAM_DATA bool8 sShownRequest = FALSE;
static EWRAM_DATA bool8 sBattleReady = FALSE;
static EWRAM_DATA bool8 sIsRequester = FALSE;
static EWRAM_DATA u8 sClosedSession = 0;
static EWRAM_DATA u32 sSeed = 0;
static EWRAM_DATA u8 sMyTag = 0;
static EWRAM_DATA u8 sPeerTag = 0;

static EWRAM_DATA u8 sPhase = PHASE_NONE;
static EWRAM_DATA u8 sOfferSlot = 0;
static EWRAM_DATA u8 sCursor = 0;
static EWRAM_DATA struct Pokemon sOffer = {0};
static EWRAM_DATA struct Pokemon sTheirs = {0};
static EWRAM_DATA u8 sGot[OFFER_MESSAGES] = {0};
static EWRAM_DATA u8 sGotCount = 0;
static EWRAM_DATA u16 sTheirSum = 0;
static EWRAM_DATA bool8 sHaveTheirs = FALSE;
static EWRAM_DATA bool8 sTheirOk = FALSE;
static EWRAM_DATA bool8 sCancelled = FALSE;

static EWRAM_DATA u32 sMixOut[REMOTE_MIX_MAX_BYTES / 4] = {0};
static EWRAM_DATA u32 sMixIn[REMOTE_MIX_MAX_BYTES / 4] = {0};
static EWRAM_DATA u8 sMixGot[256] = {0};
static EWRAM_DATA u16 sMixGotCount = 0;
static EWRAM_DATA u16 sMixTheirSum = 0;
static EWRAM_DATA u16 sMixNeed = 0;
static EWRAM_DATA bool8 sMixHaveSum = FALSE;
static EWRAM_DATA u8 sMixCursor = 0;
static EWRAM_DATA u16 sMixPieces = 0;       // pieces that are not empty
static EWRAM_DATA bool8 sMixSending = FALSE;
static EWRAM_DATA bool8 sHaveMix = FALSE;

static EWRAM_DATA struct MailInbox sInbox[REMOTE_MAX_PLAYERS] = {{0}};
static EWRAM_DATA u16 sChatOut[CHAT_WORDS] = {0};
static EWRAM_DATA u8 sMailNumber = 0;
static EWRAM_DATA u16 sMailRepeatsLeft = 0;
static EWRAM_DATA u8 sMailTarget = 0;
static EWRAM_DATA u16 sMailTag = 0;
static EWRAM_DATA u8 sMailResult = 0;       // 0 waiting, 1 delivered, 2 mailbox full

void RemoteSocial_Init(void)
{
    u8 i;

    sState = ST_IDLE;
    sSessionCounter = Random() & 0xFF;
    sPhase = PHASE_NONE;
    sReplyRepeats = 0;
    sMailRepeatsLeft = 0;
    sBattleReady = FALSE;
    for (i = 0; i < REMOTE_MAX_PLAYERS; i++)
    {
        sInbox[i].number = 0;
        sInbox[i].gotMask = 0;
        sInbox[i].deliveredNumber = 0;
    }
}

// ---------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------

static bool8 Send(u8 sub, u8 a, u8 b, const u8 *data)
{
    struct RemoteEvent event;
    u8 *bytes = (u8 *)&event;
    u8 i;

    bytes[0] = REMOTE_EVENT_SOCIAL;
    bytes[1] = sub;
    bytes[2] = a;
    bytes[3] = b;
    bytes[4] = gSaveBlock1Ptr->location.mapGroup;
    bytes[5] = gSaveBlock1Ptr->location.mapNum;
    for (i = 0; i < 6; i++)
        bytes[6 + i] = data != NULL ? data[i] : 0;
    // The acknowledgements must not wait behind the data pieces: when the
    // queue is half full of pieces they would never get through.
    if (sub == SUB_OK || sub == SUB_CANCEL)
        return RemotePlayer_TryPushTxPriority(&event);
    return RemotePlayer_TryPushTx(&event);
}

// Mix pieces use the two map bytes as data too: 8 bytes per message.
static bool8 SendWide(u8 sub, u8 a, u8 b, const u8 *data)
{
    struct RemoteEvent event;
    u8 *bytes = (u8 *)&event;
    u8 i;

    bytes[0] = REMOTE_EVENT_SOCIAL;
    bytes[1] = sub;
    bytes[2] = a;
    bytes[3] = b;
    for (i = 0; i < 8; i++)
        bytes[4 + i] = data[i];
    return RemotePlayer_TryPushTx(&event);
}

static void SendRequestMessage(void)
{
    u8 data[6];

    data[0] = sSeed & 0xFF;
    data[1] = (sSeed >> 8) & 0xFF;
    data[2] = (sSeed >> 16) & 0xFF;
    data[3] = sSeed >> 24;
    data[4] = sMyTag;
    data[5] = 0;
    Send(SUB_REQUEST, sKind, sSession, data);
}

static void SendReplyMessage(void)
{
    u8 data[6];
    u8 i;

    data[0] = sMyTag;
    for (i = 1; i < 6; i++)
        data[i] = 0;
    Send(SUB_REPLY, sReplyValue, sSession, data);
}

// A name's tag: both ends can work it out from the name alone.
static u16 NameTag(const u8 *name)
{
    u16 tag = 0x1234;
    u8 i;

    for (i = 0; i < PLAYER_NAME_LENGTH && name[i] != EOS; i++)
        tag = (u16)(tag * 31 + name[i]);
    return tag;
}

static void SendMailParts(void)
{
    u8 part;
    u8 data[6];
    u8 i;

    for (part = 0; part < MAIL_PARTS; part++)
    {
        for (i = 0; i < 6; i++)
            data[i] = 0;
        if (part < 3)
        {
            for (i = 0; i < 3; i++)
            {
                data[i * 2] = sChatOut[part * 3 + i] & 0xFF;
                data[i * 2 + 1] = sChatOut[part * 3 + i] >> 8;
            }
        }
        else
        {
            data[0] = sMailTag & 0xFF;
            data[1] = sMailTag >> 8;
        }
        Send(SUB_MAIL, sMailNumber, part, data);
    }
}

// ---------------------------------------------------------------------------
// Receiving
// ---------------------------------------------------------------------------

static u16 Checksum(const u8 *bytes, u16 length)
{
    u16 sum = 0;
    u16 i;

    for (i = 0; i < length; i++)
        sum = (u16)((sum << 3) | (sum >> 13)) + bytes[i] + 1;
    return sum;
}

static u8 MixChunks(void)
{
    return (RemoteMix_GetSize() + 7) / 8;
}

static void ClearMix(void)
{
    u16 i;

    sMixSending = FALSE;
    sHaveMix = FALSE;
    sMixCursor = 0;
    sMixGotCount = 0;
    sMixNeed = 0;
    sMixHaveSum = FALSE;
    memset(sMixIn, 0, sizeof(sMixIn));
    for (i = 0; i < 256; i++)
        sMixGot[i] = FALSE;
}

static void ClearTrade(void)
{
    u8 i;

    ClearMix();
    sPhase = PHASE_NONE;
    sCursor = 0;
    sGotCount = 0;
    sHaveTheirs = FALSE;
    sTheirOk = FALSE;
    sCancelled = FALSE;
    for (i = 0; i < OFFER_MESSAGES; i++)
        sGot[i] = FALSE;
}

static void Reset(void)
{
    if (sState != ST_IDLE)
        sClosedSession = sSession;
    sState = ST_IDLE;
    sShownRequest = FALSE;
    ClearTrade();
}

// Puts the mail in the first free place of the PC mailbox.
static bool8 DeliverMail(u8 slot, const u16 *words)
{
    u8 i;
    const u8 *name = RemoteBattle_GetNameOfSlot(slot);

    for (i = PARTY_SIZE; i < MAIL_COUNT; i++)
    {
        struct Mail *mail = &gSaveBlock1Ptr->mail[i];
        u8 j;

        if (mail->itemId != ITEM_NONE)
            continue;
        ClearMail(mail);
        for (j = 0; j < CHAT_WORDS; j++)
            mail->words[j] = words[j];
        StringCopyN(mail->playerName, name != NULL ? name : gText_Friend, PLAYER_NAME_LENGTH);
        mail->playerName[PLAYER_NAME_LENGTH] = EOS;
        mail->itemId = ITEM_ORANGE_MAIL;
        return TRUE;
    }
    return FALSE;
}

static void SendMailAck(u8 number, u8 result, u16 tag)
{
    u8 reply[6];
    u8 i;

    for (i = 0; i < 6; i++)
        reply[i] = 0;
    reply[0] = tag & 0xFF;
    reply[1] = tag >> 8;
    Send(SUB_MAILACK, number, result, reply);
}

static void StoreMail(u8 slot, u8 number, u8 part, const u8 *data)
{
    struct MailInbox *inbox = &sInbox[slot];
    u8 i;

    if (part >= MAIL_PARTS || number == 0)
        return;
    if (number != inbox->number)
    {
        inbox->number = number;
        inbox->gotMask = 0;
    }
    if (part < 3)
    {
        for (i = 0; i < 3; i++)
            inbox->words[part * 3 + i] = data[i * 2] | (data[i * 2 + 1] << 8);
    }
    else
    {
        inbox->tag = data[0] | (data[1] << 8);
    }
    inbox->gotMask |= 1 << part;
    if (inbox->gotMask == (1 << MAIL_PARTS) - 1)
    {
        inbox->gotMask = 0;
        // Mail for somebody else in the room is none of our business.
        if (inbox->tag != NameTag(gSaveBlock2Ptr->playerName))
            return;
        if (number != inbox->deliveredNumber)
        {
            if (!DeliverMail(slot, inbox->words))
            {
                SendMailAck(number, 1, inbox->tag);
                return;
            }
            inbox->deliveredNumber = number;
            PlaySE(SE_PIN);
        }
        SendMailAck(number, 0, inbox->tag);
    }
}

static void StoreChunk(u8 index, const u8 *data)
{
    u8 *dest = (u8 *)&sTheirs;
    u8 i;

    if (index >= OFFER_MESSAGES || sGot[index])
        return;
    if (index == OFFER_CHUNKS)
    {
        sTheirSum = data[0] | (data[1] << 8);
    }
    else
    {
        for (i = 0; i < 6; i++)
        {
            u16 at = index * 6 + i;

            if (at < sizeof(struct Pokemon))
                dest[at] = data[i];
        }
    }
    sGot[index] = TRUE;
    sGotCount++;
    if (sGotCount == OFFER_MESSAGES)
    {
        if (Checksum((u8 *)&sTheirs, sizeof(struct Pokemon)) == sTheirSum
         && !GetMonData(&sTheirs, MON_DATA_SANITY_IS_BAD_EGG)
         && GetMonData(&sTheirs, MON_DATA_SPECIES) != SPECIES_NONE
         && GetMonData(&sTheirs, MON_DATA_SPECIES) < NUM_SPECIES)
        {
            sHaveTheirs = TRUE;
        }
        else
        {
            // Damaged on the way: ask for everything again.
            sGotCount = 0;
            for (i = 0; i < OFFER_MESSAGES; i++)
                sGot[i] = FALSE;
        }
    }
}

// A record is mostly zeros (empty TV shows, empty secret bases), so only the
// 6-byte pieces that are not all zero are sent, plus a last message with the
// checksum and how many pieces to expect. The receiver starts from zeros.
static bool8 MixPieceIsEmpty(const u8 *bytes, u16 size, u8 index)
{
    u8 i;

    for (i = 0; i < 8; i++)
    {
        u16 at = index * 8 + i;

        if (at < size && bytes[at] != 0)
            return FALSE;
    }
    return TRUE;
}

static void ResetMixReceive(void)
{
    u16 i;

    sMixGotCount = 0;
    sMixNeed = 0;
    sMixHaveSum = FALSE;
    for (i = 0; i < ARRAY_COUNT(sMixGot); i++)
        sMixGot[i] = FALSE;
    memset(sMixIn, 0, sizeof(sMixIn));
}

static void StoreMixChunk(u8 index, const u8 *data)
{
    u8 *dest = (u8 *)sMixIn;
    u16 size = RemoteMix_GetSize();
    u8 chunks = MixChunks();
    u8 i;

    if (index > chunks || sMixGot[index] || sHaveMix)
        return;
    if (index == chunks)
    {
        sMixTheirSum = data[0] | (data[1] << 8);
        sMixNeed = data[2] | (data[3] << 8);
        sMixHaveSum = TRUE;
    }
    else
    {
        for (i = 0; i < 8; i++)
        {
            u16 at = index * 8 + i;

            if (at < size)
                dest[at] = data[i];
        }
        sMixGotCount++;
    }
    sMixGot[index] = TRUE;
    if (sMixHaveSum && sMixGotCount >= sMixNeed)
    {
        if (Checksum(dest, size) == sMixTheirSum)
            sHaveMix = TRUE;
        else
            ResetMixReceive();
    }
}

void RemoteSocial_HandleRx(u8 slot, const struct RemoteEvent *event)
{
    const u8 *m = (const u8 *)event;
    u8 sub = m[1];
    u8 a = m[2];
    u8 b = m[3];
    const u8 *data = m + 6;

    if (slot >= REMOTE_MAX_PLAYERS)
        return;
    if (sub >= SUB_SPEC_MON)
    {
        RemoteSpectate_HandleRx(slot, event);
        return;
    }

    // Until this player has a Pokemon, nothing from other players is taken.
    if (gPlayerPartyCount == 0 && (sub == SUB_MAIL || sub == SUB_REQUEST))
        return;

    switch (sub)
    {
    case SUB_MAIL:
        StoreMail(slot, a, b, data);
        break;
    case SUB_MAILACK:
        if (sMailRepeatsLeft != 0 && a == sMailNumber && slot == sMailTarget
         && (data[0] | (data[1] << 8)) == sMailTag)
        {
            sMailResult = b == 0 ? 1 : 2;
            sMailRepeatsLeft = 0;
        }
        break;
    case SUB_DROP:
        RemotePlayer_StartDrop(slot);
        break;
    case SUB_REQUEST:
        if (b == sClosedSession && sState == ST_IDLE)
        {
            // An old copy of a request that was already answered.
        }
        else if (sState == ST_IDLE)
        {
            sState = ST_ASKED;
            sKind = a;
            sPeer = slot;
            sSession = b;
            sTimer = 0;
            sShownRequest = FALSE;
            sSeed = data[0] | (data[1] << 8) | (data[2] << 16) | ((u32)data[3] << 24);
            sPeerTag = data[4];
            ClearTrade();
        }
        else if (!(sSession == b && sPeer == slot))
        {
            // Busy with something else: say no.
            Send(SUB_REPLY, 0, b, NULL);
        }
        break;
    case SUB_REPLY:
        if (sState == ST_REQUESTING && b == sSession && slot == sPeer)
        {
            sPeerTag = data[0];
            sReplyResult = a != 0 ? 1 : 2;
        }
        break;
    case SUB_CHUNK:
        if (sState == ST_AGREED && b == sSession && slot == sPeer && sKind == SOCIAL_REQUEST_TRADE)
            StoreChunk(a, data);
        break;
    case SUB_MIXCHUNK:
        if (sState == ST_AGREED && b == sSession && slot == sPeer && sKind == SOCIAL_REQUEST_MIX)
            StoreMixChunk(a, m + 4);
        break;
    case SUB_CANCEL:
        if (b == sSession && slot == sPeer && sState != ST_IDLE)
            sCancelled = TRUE;
        break;
    case SUB_OK:
        if (b == sSession && slot == sPeer && sState != ST_IDLE)
            sTheirOk = TRUE;
        break;
    }
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

static bool8 OverworldIsFree(void)
{
    return gMain.callback2 == CB2_Overworld
        && !ScriptContext_IsEnabled()
        && !ArePlayerFieldControlsLocked()
        && gPlayerAvatar.tileTransitionState == T_NOT_MOVING;
}

void RemoteSocial_SendDrop(void)
{
    Send(SUB_DROP, 0, 0, NULL);
}

static EWRAM_DATA bool8 sLeftPending = FALSE;
static EWRAM_DATA u8 sLeftName[PLAYER_NAME_LENGTH + 1] = {0};

// The link says this player is gone for good (not just busy in a menu).
void RemoteSocial_NotePlayerLeft(u8 slot)
{
    const u8 *name = RemoteBattle_GetNameOfSlot(slot);

    StringCopy(sLeftName, name != NULL ? name : gText_Friend);
    sLeftPending = TRUE;
}

void RemoteSocial_Tick(void)
{
    sFrame++;

    if (sLeftPending && sState == ST_IDLE && OverworldIsFree())
    {
        sLeftPending = FALSE;
        StringCopy(gStringVar1, sLeftName);
        ScriptContext_SetupScript(RemoteSocial_EventScript_PlayerLeft);
    }

    if (sMailRepeatsLeft != 0 && sFrame % MAIL_REPEAT_FRAMES == 0)
    {
        SendMailParts();
        sMailRepeatsLeft--;
    }

    if (sReplyRepeats != 0 && sFrame % REPLY_REPEAT_FRAMES == 0)
    {
        SendReplyMessage();
        sReplyRepeats--;
    }

    switch (sState)
    {
    case ST_REQUESTING:
        sTimer++;
        if (sTimer % REQUEST_RESEND_FRAMES == 1)
            SendRequestMessage();
        if (sTimer >= REQUEST_TIMEOUT_FRAMES && sReplyResult == 0)
            sReplyResult = 3;
        break;
    case ST_ASKED:
        sTimer++;
        if (!sShownRequest && OverworldIsFree())
        {
            sShownRequest = TRUE;
            ScriptContext_SetupScript(RemoteSocial_EventScript_Incoming);
        }
        else if (!sShownRequest && sTimer >= INCOMING_TIMEOUT_FRAMES)
        {
            Reset();
        }
        break;
    case ST_AGREED:
        sTimer++;
        if (sKind == SOCIAL_REQUEST_TRADE)
        {
            if (sPhase != PHASE_NONE && sFrame % OFFER_PERIOD == 0)
            {
                u8 data[6];
                u8 i;

                if (sCursor == OFFER_CHUNKS)
                {
                    u16 sum = Checksum((u8 *)&sOffer, sizeof(struct Pokemon));

                    data[0] = sum & 0xFF;
                    data[1] = sum >> 8;
                    for (i = 2; i < 6; i++)
                        data[i] = 0;
                }
                else
                {
                    for (i = 0; i < 6; i++)
                    {
                        u16 at = sCursor * 6 + i;

                        data[i] = at < sizeof(struct Pokemon) ? ((u8 *)&sOffer)[at] : 0;
                    }
                }
                if (Send(SUB_CHUNK, sCursor, sSession, data))
                    sCursor = (sCursor + 1) % OFFER_MESSAGES;
            }
            if (sPhase == PHASE_CONFIRMED && sFrame % REPLY_REPEAT_FRAMES == 0)
                Send(SUB_OK, 0, sSession, NULL);
            if (sTimer >= TRADE_TIMEOUT_FRAMES)
                sCancelled = TRUE;
        }
        else if (sKind == SOCIAL_REQUEST_MIX)
        {
            if (sMixSending && !sTheirOk && sFrame % MIX_PERIOD == 0)
            {
                u8 data[8];
                u8 i;
                u8 chunks = MixChunks();
                u16 size = RemoteMix_GetSize();

                if (sMixCursor == chunks)
                {
                    u16 sum = Checksum((u8 *)sMixOut, size);

                    data[0] = sum & 0xFF;
                    data[1] = sum >> 8;
                    data[2] = sMixPieces & 0xFF;
                    data[3] = sMixPieces >> 8;
                    data[4] = 0;
                    data[5] = 0;
                    data[6] = 0;
                    data[7] = 0;
                }
                else
                {
                    for (i = 0; i < 8; i++)
                    {
                        u16 at = sMixCursor * 8 + i;

                        data[i] = at < size ? ((u8 *)sMixOut)[at] : 0;
                    }
                }
                if (SendWide(SUB_MIXCHUNK, sMixCursor, sSession, data))
                {
                    // Next piece that is not empty, then the checksum, then again.
                    u8 next = sMixCursor + 1;

                    while (next < chunks && MixPieceIsEmpty((u8 *)sMixOut, size, next))
                        next++;
                    if (sMixCursor == chunks)
                    {
                        next = 0;
                        while (next < chunks && MixPieceIsEmpty((u8 *)sMixOut, size, next))
                            next++;
                    }
                    sMixCursor = next;
                }
            }
            // "I have yours": the other side stops sending when it hears this.
            if (sHaveMix && sFrame % REPLY_REPEAT_FRAMES == 0)
                Send(SUB_OK, 0, sSession, NULL);
            if (sTimer >= MIX_TIMEOUT_FRAMES)
                sCancelled = TRUE;
        }
        break;
    case ST_CLOSING:
        sTimer++;
        if (sFrame % REPLY_REPEAT_FRAMES == 0)
            Send(SUB_OK, 0, sSession, NULL);
        if (sTimer >= CLOSING_FRAMES)
            Reset();
        break;
    }

}

// ---------------------------------------------------------------------------
// Specials called by the scripts. Results go to VAR_RESULT.
// ---------------------------------------------------------------------------

// Who was talked to, and is anything else going on.
void RemoteSocial_Prepare(void)
{
    u8 slot = gSpecialVar_LastTalked - REMOTE_LOCAL_ID;
    const u8 *name;

    gSpecialVar_Result = 0;
    if (slot >= REMOTE_MAX_PLAYERS)
        return;
    sPeer = slot;
    name = RemoteBattle_GetNameOfSlot(slot);
    StringCopy(gStringVar1, name != NULL ? name : gText_Friend);
    // 1: free, 2: busy, 3: the other player's team has not arrived yet
    if (gPlayerPartyCount == 0)
        gSpecialVar_Result = 4;     // no Pokemon yet: nothing to do with other players
    else if (sState != ST_IDLE)
        gSpecialVar_Result = 2;
    else if (name == NULL)
        gSpecialVar_Result = 3;
    else if (RemoteSpectate_IsFighting(slot))
        gSpecialVar_Result = 5;     // in a fight: can be watched
    else
        gSpecialVar_Result = 1;
}

void RemoteSocial_SendRequest(void)
{
    sKind = gSpecialVar_0x8004;
    sSessionCounter++;
    if (sSessionCounter == 0)
        sSessionCounter = 1;
    sSession = sSessionCounter;
    sReplyResult = 0;
    sTimer = 0;
    sIsRequester = TRUE;
    sState = ST_REQUESTING;
    ClearTrade();
    sSeed = Random() | ((u32)Random() << 16);
    sMyTag = RemoteBattle_GetTeamTag();
    SendRequestMessage();
}

static void Task_WaitReply(u8 taskId)
{
    if (JOY_NEW(B_BUTTON) && sReplyResult == 0)
    {
        Send(SUB_CANCEL, 0, sSession, NULL);
        Reset();
        gSpecialVar_Result = 4;
        ScriptContext_Enable();
        DestroyTask(taskId);
        return;
    }
    if (sReplyResult != 0)
    {
        gSpecialVar_Result = sReplyResult;
        if (sReplyResult == 1)
        {
            sState = ST_AGREED;
            sTimer = 0;
            if (sKind == SOCIAL_REQUEST_BATTLE)
                sBattleReady = TRUE;
        }
        else
        {
            Reset();
        }
        ScriptContext_Enable();
        DestroyTask(taskId);
    }
}

void RemoteSocial_WaitReply(void)
{
    CreateTask(Task_WaitReply, 80);
}

// The other player asked: names and what they want.
void RemoteSocial_PrepareIncoming(void)
{
    const u8 *name = RemoteBattle_GetNameOfSlot(sPeer);

    StringCopy(gStringVar1, name != NULL ? name : gText_Friend);
    gSpecialVar_0x8004 = sKind;
}

// 0x8005: 1 = yes.
void RemoteSocial_Answer(void)
{
    sIsRequester = FALSE;
    sReplyValue = gSpecialVar_0x8005 != 0;
    sReplyRepeats = REPLY_REPEATS;
    sMyTag = RemoteBattle_GetTeamTag();
    SendReplyMessage();
    if (sReplyValue)
    {
        sState = ST_AGREED;
        sTimer = 0;
        if (sKind == SOCIAL_REQUEST_BATTLE)
            sBattleReady = TRUE;
    }
    else
    {
        Reset();
    }
}

bool8 RemoteSocial_TakeBattleRequest(struct VersusRequest *out)
{
    if (!sBattleReady)
        return FALSE;
    sBattleReady = FALSE;
    out->slot = sPeer;
    out->session = sSession;
    out->requester = sIsRequester;
    out->myTag = sMyTag;
    out->peerTag = sPeerTag;
    out->seed = sSeed;
    return TRUE;
}

// What was agreed: 1 battle, 2 trade.
void RemoteSocial_GetAgreedKind(void)
{
    gSpecialVar_Result = sKind;
}

// Both agreed to battle: hand over to the battle module. Reaches the script's
// waitstate only when the battle could not start.
void RemoteSocial_StartBattle(void)
{
    struct VersusRequest request;

    if (RemoteSocial_TakeBattleRequest(&request) && RemoteBattle_BeginVersus(&request))
    {
        Reset();
        return;
    }
    Reset();
    ScriptContext_Enable();
}

// ---- Trade -----------------------------------------------------------------

// 0x8004: party slot chosen (0xFF = none).
void RemoteSocial_OfferMon(void)
{
    u8 slot = gSpecialVar_0x8004;

    gSpecialVar_Result = FALSE;
    if (slot >= PARTY_SIZE || GetMonData(&gPlayerParty[slot], MON_DATA_SPECIES) == SPECIES_NONE)
    {
        Send(SUB_CANCEL, 0, sSession, NULL);
        Reset();
        return;
    }
    sOfferSlot = slot;
    sOffer = gPlayerParty[slot];
    sCursor = 0;
    sPhase = PHASE_OFFERED;
    gSpecialVar_Result = TRUE;
}

static void Task_WaitOffer(u8 taskId)
{
    if (sCancelled || (JOY_NEW(B_BUTTON) && !sHaveTheirs))
    {
        Send(SUB_CANCEL, 0, sSession, NULL);
        Reset();
        gSpecialVar_Result = 0;
        ScriptContext_Enable();
        DestroyTask(taskId);
    }
    else if (sHaveTheirs)
    {
        GetSpeciesName(gStringVar2, GetMonData(&sTheirs, MON_DATA_SPECIES));
        ConvertIntToDecimalStringN(gStringVar3, GetMonData(&sTheirs, MON_DATA_LEVEL), STR_CONV_MODE_LEFT_ALIGN, 3);
        gSpecialVar_Result = 1;
        ScriptContext_Enable();
        DestroyTask(taskId);
    }
}

void RemoteSocial_WaitOffer(void)
{
    CreateTask(Task_WaitOffer, 80);
}

// 0x8005: 1 = I accept.
void RemoteSocial_Confirm(void)
{
    if (gSpecialVar_0x8005 != 0)
    {
        sPhase = PHASE_CONFIRMED;
        Send(SUB_OK, 0, sSession, NULL);
    }
    else
    {
        Send(SUB_CANCEL, 0, sSession, NULL);
        Send(SUB_CANCEL, 0, sSession, NULL);
        Reset();
    }
}

static void Task_WaitConfirm(u8 taskId)
{
    if (sCancelled || sState == ST_IDLE)
    {
        Reset();
        gSpecialVar_Result = 0;
        ScriptContext_Enable();
        DestroyTask(taskId);
    }
    else if (sTheirOk)
    {
        gSpecialVar_Result = 1;
        ScriptContext_Enable();
        DestroyTask(taskId);
    }
}

void RemoteSocial_WaitConfirm(void)
{
    CreateTask(Task_WaitConfirm, 80);
}

// Both accepted: swap. The Pokémon in the slot must still be the one offered.
void RemoteSocial_FinishTrade(void)
{
    struct Pokemon *mon = &gPlayerParty[sOfferSlot];
    u16 species;
    u32 personality;

    gSpecialVar_Result = FALSE;
    if (!sHaveTheirs
     || GetMonData(mon, MON_DATA_PERSONALITY) != GetMonData(&sOffer, MON_DATA_PERSONALITY)
     || GetMonData(mon, MON_DATA_OT_ID) != GetMonData(&sOffer, MON_DATA_OT_ID))
    {
        Send(SUB_CANCEL, 0, sSession, NULL);
        Reset();
        return;
    }

    *mon = sTheirs;
    species = GetMonData(mon, MON_DATA_SPECIES);
    personality = GetMonData(mon, MON_DATA_PERSONALITY);
    GetSetPokedexFlag(SpeciesToNationalPokedexNum(species), FLAG_SET_SEEN);
    HandleSetPokedexFlag(SpeciesToNationalPokedexNum(species), FLAG_SET_CAUGHT, personality);
    GetSpeciesName(gStringVar2, species);
    CalculatePlayerPartyCount();

    sState = ST_CLOSING;
    sTimer = 0;
    sPhase = PHASE_CONFIRMED;
    gSpecialVar_Result = TRUE;
}

// A traded Pokemon that evolves by trade does so now, but only after the
// closing acknowledgements are done (the evolution scene stops this module's
// per-frame work, and the other player may still be waiting for our "OK").
static void Task_TradeEvolve(u8 taskId)
{
    struct Pokemon *mon = &gPlayerParty[sOfferSlot];
    u16 target;

    if (sState != ST_IDLE)
        return;
    DestroyTask(taskId);
    target = GetEvolutionTargetSpecies(mon, EVO_MODE_TRADE, ITEM_NONE);
    if (target == SPECIES_NONE)
    {
        ScriptContext_Enable();
        return;
    }
    gCB2_AfterEvolution = CB2_ReturnToFieldContinueScriptPlayMapMusic;
    BeginEvolutionScene(mon, target, FALSE, sOfferSlot);
}

void RemoteSocial_TradeEvolve(void)
{
    CreateTask(Task_TradeEvolve, 80);
}

// ---- Record mixing --------------------------------------------------------

void RemoteSocial_StartMix(void)
{
    {
        u8 i, chunks;
        u16 size;

        RemoteMix_Build(sMixOut);
        size = RemoteMix_GetSize();
        chunks = MixChunks();
        sMixPieces = 0;
        sMixCursor = chunks;        // the checksum goes first, then the pieces
        for (i = 0; i < chunks; i++)
        {
            if (!MixPieceIsEmpty((u8 *)sMixOut, size, i))
                sMixPieces++;
        }
    }
    sMixSending = TRUE;
    sTimer = 0;
    StringCopy(gStringVar1, RemoteBattle_GetNameOfSlot(sPeer) != NULL ? RemoteBattle_GetNameOfSlot(sPeer) : gText_Friend);
}

static void Task_WaitMix(u8 taskId)
{
    if (sCancelled || sState == ST_IDLE || (JOY_NEW(B_BUTTON) && !sTheirOk))
    {
        Send(SUB_CANCEL, 0, sSession, NULL);
        Reset();
        gSpecialVar_Result = 0;
        ScriptContext_Enable();
        DestroyTask(taskId);
    }
    else if (sHaveMix && sTheirOk)
    {
        gSpecialVar_Result = 1;
        ScriptContext_Enable();
        DestroyTask(taskId);
    }
}

void RemoteSocial_WaitMix(void)
{
    CreateTask(Task_WaitMix, 80);
}

// Both sides have both records: mix them, then keep acknowledging for a while.
void RemoteSocial_FinishMix(void)
{
    gSpecialVar_Result = sHaveMix && RemoteMix_Apply(sMixIn);
    if (!gSpecialVar_Result)
    {
        Send(SUB_CANCEL, 0, sSession, NULL);
        Reset();
        return;
    }
    sMixSending = FALSE;
    sState = ST_CLOSING;
    sTimer = 0;
}

// ---- Chat ------------------------------------------------------------------

void RemoteSocial_ComposeChat(void)
{
    u8 i;

    for (i = 0; i < CHAT_WORDS; i++)
        sChatOut[i] = EC_EMPTY_WORD;
    CleanupOverworldWindowsAndTilemaps();
    DoEasyChatScreen(EASY_CHAT_TYPE_MAIL, sChatOut, CB2_ReturnToFieldContinueScript, EASY_CHAT_PERSON_DISPLAY_NONE);
}

// After the screen: send what was written to the player that was talked to.
void RemoteSocial_SendChat(void)
{
    u8 i;
    bool8 any = FALSE;
    const u8 *name = RemoteBattle_GetNameOfSlot(sPeer);

    for (i = 0; i < CHAT_WORDS; i++)
    {
        if (sChatOut[i] != EC_EMPTY_WORD)
            any = TRUE;
    }
    gSpecialVar_Result = any && name != NULL;
    if (!gSpecialVar_Result)
        return;
    // After a restart the numbers begin again: start somewhere else each time
    // so a mail is never taken for one that was already delivered.
    if (sMailNumber == 0)
        sMailNumber = (u8)(sFrame ^ (sFrame >> 8));
    sMailNumber++;
    if (sMailNumber == 0)
        sMailNumber = 1;
    sMailTarget = sPeer;
    sMailTag = NameTag(name);
    sMailResult = 0;
    SendMailParts();
    sMailRepeatsLeft = MAIL_REPEATS;
}

// Waits for the other game to say the mail is in its mailbox.
static void Task_WaitMail(u8 taskId)
{
    if (JOY_NEW(B_BUTTON))
    {
        sMailRepeatsLeft = 0;
        gSpecialVar_Result = 0;
        ScriptContext_Enable();
        DestroyTask(taskId);
    }
    else if (sMailResult != 0)
    {
        gSpecialVar_Result = sMailResult;
        ScriptContext_Enable();
        DestroyTask(taskId);
    }
    else if (sMailRepeatsLeft == 0)
    {
        gSpecialVar_Result = 0;
        ScriptContext_Enable();
        DestroyTask(taskId);
    }
}

void RemoteSocial_WaitMail(void)
{
    CreateTask(Task_WaitMail, 80);
}
