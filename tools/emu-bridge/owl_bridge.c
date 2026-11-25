// Emulator side of the native Emerald multiplayer link.
//
// The ROM (src/remote_sio.c) exposes a mailbox in EWRAM when it sees mGBA.
// This file finds it, drains the game's outgoing events to the relay and
// writes incoming events back. It speaks the relay's UDP "OWL1" datagrams
// (net/protocol.py in gen3-poke-multiplayer-overworld-sync), so it joins the
// same rooms as real GBAs and browsers.
//
// NOT BUILT OR RUN YET: written against mGBA's public C API from memory.
// Expect to fix small API differences when it is first compiled.

#include "owl_bridge.h"

#include <mgba/core/core.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <unistd.h>
#include <time.h>

#define EWRAM_BASE   0x02000000u
#define EWRAM_SIZE   0x00040000u
#define MBOX_MAGIC   0x324D424Fu
#define MBOX_SLOTS   16
#define EVENT_SIZE   12

// struct EmuMailbox layout (src/remote_sio.c)
#define MB_MAGIC     0x00
#define MB_TXHEAD    0x04
#define MB_TXTAIL    0x08
#define MB_RXHEAD    0x10
#define MB_RXTAIL    0x14
#define MB_FRAMES    0x1C
#define MB_TX        0x20
#define MB_RX        (MB_TX + MBOX_SLOTS * EVENT_SIZE)

// OWL1 datagram header: magic[4] version kind peer(le16) room(le16) seq
#define OWL_HEADER   11
#define T_HELLO      0
#define T_EVENT      1
#define T_PING       2
#define T_BYE        4

#define MAX_FRIENDS  3

static int sSock = -1;
static uint32_t sBox = 0;            // mailbox address, 0 = not found
static uint32_t sLastFrames = 0;
static uint32_t sStale = 0;
static uint16_t sPeer = 1, sRoom = 1;
static uint8_t sSeq = 0;
static uint32_t sCode;
static uint16_t sFriends[MAX_FRIENDS];
static int sFriendCount = 0;
static time_t sLastHello = 0;
static char sHost[128] = "127.0.0.1";
static char sPort[16] = "9000";

static void putle16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static uint16_t getle16(const uint8_t *p) { return p[0] | (p[1] << 8); }

static void send_dgram(uint8_t kind, uint8_t seq, const uint8_t *body, size_t n)
{
    uint8_t buf[OWL_HEADER + 64];
    if (sSock < 0 || n > 64)
        return;
    memcpy(buf, "OWL1", 4);
    buf[4] = 1;                      // version
    buf[5] = kind;
    putle16(buf + 6, sPeer);
    putle16(buf + 8, sRoom);
    buf[10] = seq;
    if (n)
        memcpy(buf + OWL_HEADER, body, n);
    send(sSock, buf, OWL_HEADER + n, 0);
}

static void send_hello(void)
{
    uint8_t code[4] = { sCode, sCode >> 8, sCode >> 16, sCode >> 24 };
    send_dgram(T_HELLO, 0, code, 4);
    sLastHello = time(NULL);
}

static void load_config(const char *dir)
{
    char path[512];
    char line[256];
    FILE *f;

    snprintf(path, sizeof(path), "%s/owl.txt", dir ? dir : ".");
    f = fopen(path, "r");
    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        char v[128];
        if (sscanf(line, "host=%127s", v) == 1) snprintf(sHost, sizeof(sHost), "%s", v);
        else if (sscanf(line, "port=%15s", v) == 1) snprintf(sPort, sizeof(sPort), "%s", v);
        else if (sscanf(line, "room=%127s", v) == 1) sRoom = (uint16_t) atoi(v);
        else if (sscanf(line, "peer=%127s", v) == 1) sPeer = (uint16_t) atoi(v);
    }
    fclose(f);
}

static void open_socket(void)
{
    struct addrinfo hints, *res;

    if (sSock >= 0)
        close(sSock);
    sSock = -1;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(sHost, sPort, &hints, &res) != 0)
        return;
    sSock = socket(res->ai_family, res->ai_socktype, 0);
    if (sSock >= 0) {
        if (connect(sSock, res->ai_addr, res->ai_addrlen) != 0) {
            close(sSock);
            sSock = -1;
        } else {
            fcntl(sSock, F_SETFL, fcntl(sSock, F_GETFL) | O_NONBLOCK);
        }
    }
    freeaddrinfo(res);
}

void owl_bridge_attach(struct mCore *core, const char *configDir)
{
    (void) core;
    load_config(configDir);
    srand((unsigned) time(NULL) ^ (unsigned) getpid());
    sCode = ((uint32_t) rand() << 16) ^ (uint32_t) rand() ^ 1u;
    sFriendCount = 0;
    sBox = 0;
    open_socket();
    send_hello();
}

void owl_bridge_detach(void)
{
    send_dgram(T_BYE, sSeq++, NULL, 0);
    if (sSock >= 0)
        close(sSock);
    sSock = -1;
    sBox = 0;
}

static void find_mailbox(struct mCore *core)
{
    uint32_t a;
    for (a = EWRAM_BASE; a < EWRAM_BASE + EWRAM_SIZE; a += 4) {
        if (core->busRead32(core, a) == MBOX_MAGIC &&
            core->busRead32(core, a + 0x0C) == MBOX_SLOTS) {
            sBox = a;
            sLastFrames = 0;
            sStale = 0;
            return;
        }
    }
}

static int slot_for(uint16_t peer)
{
    int i;
    for (i = 0; i < sFriendCount; i++)
        if (sFriends[i] == peer)
            return i;
    if (sFriendCount >= MAX_FRIENDS)
        return -1;
    sFriends[sFriendCount] = peer;
    return sFriendCount++;
}

static void write_event(struct mCore *core, uint32_t slotAddr, const uint8_t *ev)
{
    int i;
    for (i = 0; i < EVENT_SIZE; i++)
        core->busWrite8(core, slotAddr + i, ev[i]);
}

void owl_bridge_frame(struct mCore *core)
{
    uint8_t dg[OWL_HEADER + 64];
    ssize_t n;
    uint32_t frames, head, tail;

    if (sSock < 0)
        return;

    // The relay forgets quiet peers: say hello again every few seconds.
    if (time(NULL) - sLastHello >= 3)
        send_hello();

    if (!sBox || core->busRead32(core, sBox + MB_MAGIC) != MBOX_MAGIC) {
        sBox = 0;
        find_mailbox(core);          // cheap enough every frame until found
        if (!sBox)
            return;
    }

    // If the game stopped counting (reset, other ROM) look again.
    frames = core->busRead32(core, sBox + MB_FRAMES);
    if (frames == sLastFrames) {
        if (++sStale > 600) {
            sBox = 0;
            return;
        }
    } else {
        sStale = 0;
        sLastFrames = frames;
    }

    // Game -> relay
    head = core->busRead32(core, sBox + MB_TXHEAD);
    tail = core->busRead32(core, sBox + MB_TXTAIL);
    while (tail != head) {
        uint8_t ev[EVENT_SIZE];
        int i;
        for (i = 0; i < EVENT_SIZE; i++)
            ev[i] = core->busRead8(core, sBox + MB_TX + tail * EVENT_SIZE + i);
        send_dgram(T_EVENT, sSeq++, ev, EVENT_SIZE);
        tail = (tail + 1) % MBOX_SLOTS;
    }
    core->busWrite32(core, sBox + MB_TXTAIL, tail);

    // Relay -> game
    head = core->busRead32(core, sBox + MB_RXHEAD);
    tail = core->busRead32(core, sBox + MB_RXTAIL);
    while ((n = recv(sSock, dg, sizeof(dg), 0)) > 0) {
        uint8_t ev[EVENT_SIZE];
        uint32_t next = (head + 1) % MBOX_SLOTS;
        int slot;

        if (n < OWL_HEADER + EVENT_SIZE || memcmp(dg, "OWL1", 4) != 0 || dg[5] != T_EVENT)
            continue;
        slot = slot_for(getle16(dg + 6));
        if (slot < 0 || next == tail)
            continue;                // no avatar slot, or ring full: drop
        memcpy(ev, dg + OWL_HEADER, EVENT_SIZE);
        ev[0] = (ev[0] & 0x0F) | (slot << 4);   // the receiver stamps the sender slot
        write_event(core, sBox + MB_RX + head * EVENT_SIZE, ev);
        head = next;
        core->busWrite32(core, sBox + MB_RXHEAD, head);
        tail = core->busRead32(core, sBox + MB_RXTAIL);
    }
}
