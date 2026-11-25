#include "global.h"
#include "gpu_regs.h"
#include "main.h"
#include "remote_player.h"
#include "remote_sio.h"

#define SIO_FRAME_SYNC      0xA55A
#define SIO_WORD_IDLE       0x7FFF  // neither 0x0000 (dropped by the firmware) nor 0xFFFF (idle line)
#define SIO_TYPE_EVENT      0x01    // from the adapter: the sender slot is already stamped
#define SIO_TYPE_CABLE      0x02    // from a console on the cable: the receiver stamps the slot
#define SIO_LANES           4
#define CABLE_TIMER_RELOAD  (-197)  // 64 cycles each: the master waits about 0.75 ms between exchanges
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

// Emulator mailbox. Under mGBA there is no adapter: a host program (a Lua
// script on desktop, native code in an Android build) finds this struct by its
// magic, drains tx[] into the network and fills rx[] from it. Producer and
// consumer each move only their own index, so no locking is needed.
#define MBOX_SLOTS      16
#define MBOX_MAGIC      0x324D424Fu     // 'OMB2'
#define MBOX_PER_VBLANK 4

struct EmuMailbox
{
    vu32 magic;
    vu32 txHead;        // written by the game
    vu32 txTail;        // written by the host
    u32 txSlots;
    vu32 rxHead;        // written by the host
    vu32 rxTail;        // written by the game
    u32 rxSlots;
    vu32 gameFrames;    // VBlank counter, so the host knows the game is alive
    u8 tx[MBOX_SLOTS][12];
    u8 rx[MBOX_SLOTS][12];
};

static EWRAM_DATA struct EmuMailbox sMailbox = {0};
static EWRAM_DATA bool8 sEmulator = FALSE;

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

// Cable mode: with no adapter the consoles are the Multi-Player bus. The one
// on the master end of the cable starts every exchange; all of them send a
// word in their own lane and every console sees all four lanes.
static EWRAM_DATA bool8 sIsMaster = FALSE;
static EWRAM_DATA u8 sMyId = 0;

// One frame parser per lane.
static EWRAM_DATA u8 sRxPhase[SIO_LANES] = {0};
static EWRAM_DATA u8 sRxType[SIO_LANES] = {0};
static EWRAM_DATA u8 sRxCount[SIO_LANES] = {0};
static EWRAM_DATA u8 sRxGot[SIO_LANES] = {0};
static EWRAM_DATA u16 sRxSum[SIO_LANES] = {0};
static EWRAM_DATA u16 sRxBuf[SIO_LANES][SIO_MAX_WORDS] = {{0}};

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

static void StopCableTimer(void)
{
    REG_TM3CNT_H &= ~TIMER_ENABLE;
}

static void StartCableTimer(void)
{
    REG_TM3CNT_H &= ~TIMER_ENABLE;
    REG_TM3CNT_L = CABLE_TIMER_RELOAD;
    REG_TM3CNT_H = TIMER_64CLK | TIMER_INTR_ENABLE | TIMER_ENABLE;
}

// The master's pacing: one exchange per tick, started only when every console
// on the cable is in Multi-Player mode (SD high) and the last one is over.
static void CableTimerIntr(void)
{
    StopCableTimer();
    if (sOwned && !(REG_SIOCNT & SIO_START) && (REG_SIOCNT & SIO_MULTI_SD))
        REG_SIOCNT |= SIO_START;
    else
        StartCableTimer();
}

static void BecomeMaster(void)
{
    sIsMaster = TRUE;
    gIntrTable[2] = CableTimerIntr;
    EnableInterrupts(INTR_FLAG_TIMER3);
    StartCableTimer();
}

static void StopMaster(void)
{
    if (!sIsMaster)
        return;
    sIsMaster = FALSE;
    StopCableTimer();
    DisableInterrupts(INTR_FLAG_TIMER3);
    RestoreSerialTimer3IntrHandlers();
}

// The console on the master end of the cable sees SD high and SI low. With an
// adapter, or on a slave end, SI is high and nothing is started from here.
static void UpdateRole(void)
{
    bool8 master = (REG_SIOCNT & (SIO_MULTI_SD | SIO_MULTI_SI)) == SIO_MULTI_SD;

    if (master && !sIsMaster)
        BecomeMaster();
    else if (!master && sIsMaster)
        StopMaster();
}

static void ResetParsers(void)
{
    u8 i;

    for (i = 0; i < SIO_LANES; i++)
        sRxPhase[i] = RX_WAIT_SYNC;
}

static void ArmSerial(void)
{
    // The game may have left its own callback behind after closing a link.
    SetSerialCallback(NULL);
    REG_RCNT = 0;
    REG_SIOCNT = SIOCNT_CONFIG;
    REG_SIOMLT_SEND = PeekWord();
    ResetParsers();
    EnableInterrupts(INTR_FLAG_SERIAL);
    UpdateRole();
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
    StopMaster();
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
    u16 head = ((sIsMaster ? SIO_TYPE_CABLE : SIO_TYPE_EVENT) << 8) | EVENT_WORDS;
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

static void DeliverFrame(u8 lane)
{
    struct RemoteEvent event;
    u8 *dst = (u8 *)&event;
    const u8 *src = (const u8 *)sRxBuf[lane];
    u8 i;

    if (sRxCount[lane] != EVENT_WORDS
     || (sRxType[lane] != SIO_TYPE_EVENT && sRxType[lane] != SIO_TYPE_CABLE))
        return;     // not ours; ignore without counting it as a fault

    for (i = 0; i < sizeof(event); i++)
        dst[i] = src[i];

    // The adapter (lane 0, type 1) has stamped the sender already. Anything
    // else comes from a console: its slot is its lane, counted without ours.
    if (!(lane == 0 && sRxType[lane] == SIO_TYPE_EVENT))
    {
        u8 slot = lane < sMyId ? lane : lane - 1;

        if (slot >= REMOTE_MAX_PLAYERS)
            return;
        event.type = (event.type & 0x0F) | (slot << 4);
    }
    if (!RemotePlayer_PushRx(&event))
        gRemoteSioRxDropped++;
}

static void RxWord(u8 lane, u16 word)
{
    switch (sRxPhase[lane])
    {
    case RX_WAIT_SYNC:
        // Idle words and noise are ignored: the line is mostly at rest.
        if (word == SIO_FRAME_SYNC)
            sRxPhase[lane] = RX_WAIT_HEAD;
        break;
    case RX_WAIT_HEAD:
        sRxType[lane] = word >> 8;
        sRxCount[lane] = word & 0xFF;
        if (sRxCount[lane] > SIO_MAX_WORDS || sRxType[lane] == 0)
        {
            // The sync word was a coincidence; look for the next one.
            gRemoteSioFrameErrors++;
            sRxPhase[lane] = RX_WAIT_SYNC;
            break;
        }
        sRxGot[lane] = 0;
        sRxSum[lane] = word;
        sRxPhase[lane] = sRxCount[lane] != 0 ? RX_DATA : RX_CHECKSUM;
        break;
    case RX_DATA:
        sRxBuf[lane][sRxGot[lane]++] = word;
        sRxSum[lane] += word;
        if (sRxGot[lane] >= sRxCount[lane])
            sRxPhase[lane] = RX_CHECKSUM;
        break;
    case RX_CHECKSUM:
        if ((u16)(sRxSum[lane] + word) != 0)
        {
            gRemoteSioFrameErrors++;
        }
        else
        {
            gRemoteSioFramesRx++;
            DeliverFrame(lane);
        }
        sRxPhase[lane] = RX_WAIT_SYNC;
        break;
    }
}

void RemoteSio_Init(void)
{
    u8 i;

    sEmulator = FALSE;
    // mGBA's debug register answers 0x1DEA once 0xC0DE is written to it;
    // real hardware ignores the write and does not answer that.
    *(vu16 *)0x04FFF780 = 0xC0DE;
    if (*(vu16 *)0x04FFF780 != 0x1DEA)
        return;

    sEmulator = TRUE;
    sMailbox.txHead = sMailbox.txTail = 0;
    sMailbox.rxHead = sMailbox.rxTail = 0;
    sMailbox.txSlots = MBOX_SLOTS;
    sMailbox.rxSlots = MBOX_SLOTS;
    sMailbox.gameFrames = 0;
    for (i = 0; i < 12; i++)
    {
        sMailbox.tx[0][i] = 0;
        sMailbox.rx[0][i] = 0;
    }
    sMailbox.magic = MBOX_MAGIC;    // last: the host looks for this
}

// Moves events between the game's rings and the mailbox.
static void PumpMailbox(void)
{
    struct RemoteEvent event;
    u8 n;
    u8 i;

    sMailbox.gameFrames++;

    for (n = 0; n < MBOX_PER_VBLANK; n++)
    {
        u32 head = sMailbox.txHead;
        u32 next = (head + 1) % MBOX_SLOTS;

        if (next == sMailbox.txTail || !RemotePlayer_PopTx(&event))
            break;
        for (i = 0; i < 12; i++)
            sMailbox.tx[head][i] = ((const u8 *)&event)[i];
        sMailbox.txHead = next;
    }

    for (n = 0; n < MBOX_PER_VBLANK; n++)
    {
        u32 tail = sMailbox.rxTail;

        if (tail == sMailbox.rxHead)
            break;
        for (i = 0; i < 12; i++)
            ((u8 *)&event)[i] = sMailbox.rx[tail][i];
        if (!RemotePlayer_PushRx(&event))
        {
            gRemoteSioRxDropped++;
            break;
        }
        sMailbox.rxTail = (tail + 1) % MBOX_SLOTS;
    }
}

void RemoteSio_OnSerialIrq(void)
{
    u8 lane;

    if (!sOwned)
        return;

    sIrqCount++;
    sMyId = (REG_SIOCNT & SIO_ID) >> 4;
    // Every console sees all four lanes. With an adapter it is the master in
    // lane 0; with a cable the other consoles are in theirs. Our own lane
    // only echoes what we sent (the console in lane 0 of an adapter is not us).
    for (lane = 0; lane < SIO_LANES; lane++)
    {
        if (sIsMaster ? lane == 0 : (lane == sMyId && lane != 0))
            continue;
        RxWord(lane, REG_SIOMULTI(lane));
    }
    // Load the next word now: the master may start again at any moment.
    REG_SIOMLT_SEND = NextWord();
    if (sIsMaster)
        StartCableTimer();
}

void RemoteSio_Tick(void)
{
    sIdleVBlanks = 0;
    if (!sEmulator && !sOwned && !sGameLinkOpen)
        Acquire();
}

void RemoteSio_VBlank(void)
{
    struct RemoteEvent event;

    if (sEmulator)
    {
        PumpMailbox();
        return;
    }

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

    UpdateRole();
    // The master's pacing timer stops after every exchange; make sure it runs.
    if (sIsMaster && !(REG_TM3CNT_H & TIMER_ENABLE) && !(REG_SIOCNT & SIO_START))
        StartCableTimer();

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
    StopMaster();
}

void RemoteSio_GameLinkClosed(void)
{
    sGameLinkOpen = FALSE;
}

#else // !REMOTE_PLAYER_SIO

void RemoteSio_Init(void) {}
void RemoteSio_Tick(void) {}
void RemoteSio_VBlank(void) {}
void RemoteSio_OnSerialIrq(void) {}
void RemoteSio_GameLinkOpened(void) {}
void RemoteSio_GameLinkClosed(void) {}

#endif
