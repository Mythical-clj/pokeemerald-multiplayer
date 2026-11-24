#include "global.h"
#include "gpu_regs.h"
#include "main.h"
#include "remote_player.h"
#include "remote_sio.h"

#define SIO_FRAME_SYNC      0xA55A
#define SIO_WORD_IDLE       0x7FFF  // neither 0x0000 (dropped by the firmware) nor 0xFFFF (idle line)
#define SIO_TYPE_EVENT      0x01
#define SIO_MAX_WORDS       8
#define EVENT_WORDS         (sizeof(struct RemoteEvent) / 2)

// VBlanks without a Tick before the port is given back to the game.
#define IDLE_VBLANKS_BEFORE_RELEASE 3
// How often the port configuration is checked while we hold it.
#define GUARD_PERIOD_VBLANKS        30
// Without any serial interrupt for this long the port is armed again.
#define SILENCE_VBLANKS_BEFORE_REARM 120

// SIOCNT bits that are configuration (mode, IRQ enable, baud rate); the
// others change with every exchange.
#define SIOCNT_CONFIG_MASK  0x7003
#define SIOCNT_CONFIG       (SIO_MULTI_MODE | SIO_115200_BPS | SIO_INTR_ENABLE)

#if REMOTE_PLAYER_SIO

enum
{
    RX_WAIT_SYNC,
    RX_WAIT_HEAD,
    RX_DATA,
    RX_CHECKSUM,
};

static EWRAM_DATA bool8 sOwned = FALSE;
static EWRAM_DATA bool8 sGameLinkOpen = FALSE;
static EWRAM_DATA u8 sIdleVBlanks = 0;
static EWRAM_DATA u8 sGuardTimer = 0;
static EWRAM_DATA u16 sSilentVBlanks = 0;
static EWRAM_DATA vu32 sIrqCount = 0;
static EWRAM_DATA u32 sLastIrqCount = 0;

static EWRAM_DATA u8 sRxPhase = 0;
static EWRAM_DATA u8 sRxType = 0;
static EWRAM_DATA u8 sRxCount = 0;
static EWRAM_DATA u8 sRxGot = 0;
static EWRAM_DATA u16 sRxSum = 0;
static EWRAM_DATA u16 sRxBuf[SIO_MAX_WORDS] = {0};

static EWRAM_DATA u16 sTxBuf[SIO_MAX_WORDS + 3] = {0};
static EWRAM_DATA vu8 sTxLen = 0;
static EWRAM_DATA vu8 sTxPos = 0;

// Counters for diagnosis; a counter that stays at zero points at a fault.
EWRAM_DATA u32 gRemoteSioFramesRx = 0;
EWRAM_DATA u32 gRemoteSioFramesTx = 0;
EWRAM_DATA u32 gRemoteSioFrameErrors = 0;
EWRAM_DATA u32 gRemoteSioRxDropped = 0;
EWRAM_DATA u32 gRemoteSioRearms = 0;

static u16 PeekWord(void)
{
    if (sTxPos < sTxLen)
        return sTxBuf[sTxPos];
    return SIO_WORD_IDLE;
}

static u16 NextWord(void)
{
    u16 word;

    if (sTxPos < sTxLen)
    {
        word = sTxBuf[sTxPos++];
        if (sTxPos >= sTxLen)
        {
            sTxLen = 0;
            sTxPos = 0;
            gRemoteSioFramesTx++;
        }
        return word;
    }
    return SIO_WORD_IDLE;
}

static void ArmSerial(void)
{
    // The game may have left its own callback behind after closing a link.
    SetSerialCallback(NULL);
    REG_RCNT = 0;
    REG_SIOCNT = SIOCNT_CONFIG;
    REG_SIOMLT_SEND = PeekWord();
    sRxPhase = RX_WAIT_SYNC;
    EnableInterrupts(INTR_FLAG_SERIAL);
}

static void Acquire(void)
{
    sTxLen = 0;
    sTxPos = 0;
    sIdleVBlanks = 0;
    sGuardTimer = 0;
    sSilentVBlanks = 0;
    sLastIrqCount = sIrqCount;
    sOwned = TRUE;
    ArmSerial();
}

// Gives the port back. Touches the registers only when the game is not using
// them: if its link has opened meanwhile, they are already its own.
static void Release(void)
{
    if (!sOwned)
        return;
    sOwned = FALSE;
    if (!sGameLinkOpen)
    {
        DisableInterrupts(INTR_FLAG_SERIAL);
        REG_SIOCNT = SIO_MULTI_MODE;
    }
}

// Queues one event as a frame; FALSE when the previous frame is still going out.
static bool8 SendEventFrame(const struct RemoteEvent *event)
{
    const u16 *words = (const u16 *)event;
    u16 head = (SIO_TYPE_EVENT << 8) | EVENT_WORDS;
    u16 sum = head;
    u8 i;

    if (sTxLen != 0)
        return FALSE;

    sTxBuf[0] = SIO_FRAME_SYNC;
    sTxBuf[1] = head;
    for (i = 0; i < EVENT_WORDS; i++)
    {
        sTxBuf[2 + i] = words[i];
        sum += words[i];
    }
    sTxBuf[2 + EVENT_WORDS] = -sum;
    sTxPos = 0;
    sTxLen = EVENT_WORDS + 3;
    return TRUE;
}

static void DeliverFrame(void)
{
    struct RemoteEvent event;
    u8 *dst = (u8 *)&event;
    const u8 *src = (const u8 *)sRxBuf;
    u8 i;

    if (sRxType != SIO_TYPE_EVENT || sRxCount != EVENT_WORDS)
        return;     // not ours; ignore without counting it as a fault

    for (i = 0; i < sizeof(event); i++)
        dst[i] = src[i];
    if (!RemotePlayer_PushRx(&event))
        gRemoteSioRxDropped++;
}

static void RxWord(u16 word)
{
    switch (sRxPhase)
    {
    case RX_WAIT_SYNC:
        // Idle words and noise are ignored: the line is mostly at rest.
        if (word == SIO_FRAME_SYNC)
            sRxPhase = RX_WAIT_HEAD;
        break;
    case RX_WAIT_HEAD:
        sRxType = word >> 8;
        sRxCount = word & 0xFF;
        if (sRxCount > SIO_MAX_WORDS || sRxType == 0)
        {
            // The sync word was a coincidence; look for the next one.
            gRemoteSioFrameErrors++;
            sRxPhase = RX_WAIT_SYNC;
            break;
        }
        sRxGot = 0;
        sRxSum = word;
        sRxPhase = sRxCount != 0 ? RX_DATA : RX_CHECKSUM;
        break;
    case RX_DATA:
        sRxBuf[sRxGot++] = word;
        sRxSum += word;
        if (sRxGot >= sRxCount)
            sRxPhase = RX_CHECKSUM;
        break;
    case RX_CHECKSUM:
        if ((u16)(sRxSum + word) != 0)
        {
            gRemoteSioFrameErrors++;
        }
        else
        {
            gRemoteSioFramesRx++;
            DeliverFrame();
        }
        sRxPhase = RX_WAIT_SYNC;
        break;
    }
}

void RemoteSio_OnSerialIrq(void)
{
    if (!sOwned)
        return;

    sIrqCount++;
    // The adapter is the master, so its word is in the first slot.
    RxWord(REG_SIOMULTI0);
    // Load the next word now: the master may start again at any moment.
    REG_SIOMLT_SEND = NextWord();
}

void RemoteSio_Tick(void)
{
    sIdleVBlanks = 0;
    if (!sOwned && !sGameLinkOpen)
        Acquire();
}

void RemoteSio_VBlank(void)
{
    struct RemoteEvent event;

    if (!sOwned)
        return;

    if (sGameLinkOpen || ++sIdleVBlanks >= IDLE_VBLANKS_BEFORE_RELEASE)
    {
        Release();
        return;
    }

    // Keep one frame in flight; pop an event only when there is room for it.
    if (sTxLen == 0 && RemotePlayer_PopTx(&event))
        SendEventFrame(&event);

    // Something else rewrote the port configuration: take it back.
    if (++sGuardTimer >= GUARD_PERIOD_VBLANKS)
    {
        sGuardTimer = 0;
        if ((REG_SIOCNT & SIOCNT_CONFIG_MASK) != SIOCNT_CONFIG || !(REG_IE & INTR_FLAG_SERIAL))
        {
            ArmSerial();
            gRemoteSioRearms++;
        }
    }

    // A master that was there and has gone quiet (cable pulled, adapter
    // restarted): arm again so a returning master finds a clean state.
    if (sIrqCount != sLastIrqCount)
    {
        sLastIrqCount = sIrqCount;
        sSilentVBlanks = 0;
    }
    else if (++sSilentVBlanks >= SILENCE_VBLANKS_BEFORE_REARM)
    {
        sSilentVBlanks = 0;
        ArmSerial();
        gRemoteSioRearms++;
    }
}

void RemoteSio_GameLinkOpened(void)
{
    sGameLinkOpen = TRUE;
    // The game has just reconfigured the port: it is already its own.
    sOwned = FALSE;
}

void RemoteSio_GameLinkClosed(void)
{
    sGameLinkOpen = FALSE;
}

#else // !REMOTE_PLAYER_SIO

void RemoteSio_Tick(void) {}
void RemoteSio_VBlank(void) {}
void RemoteSio_OnSerialIrq(void) {}
void RemoteSio_GameLinkOpened(void) {}
void RemoteSio_GameLinkClosed(void) {}

#endif
