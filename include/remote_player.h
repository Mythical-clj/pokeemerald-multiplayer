#ifndef GUARD_REMOTE_PLAYER_H
#define GUARD_REMOTE_PLAYER_H

// Remote players shown in the overworld as object events.
//
// Ported from gen3-poke-multiplayer-overworld-sync (payload/main.c), which
// injects equivalent code into a retail cartridge. Here it is compiled into
// the ROM, so it calls the game's own functions directly.
//
// The wire format is the sync project's 12-byte event, so its relay and
// client can be reused unchanged.

// Remote players that can be shown at once (a room holds at most 4 players).
#define REMOTE_MAX_PLAYERS  3

// Object events spawned for remote players use localIds REMOTE_LOCAL_ID + slot.
#define REMOTE_LOCAL_ID     0xE0
#define IS_REMOTE_LOCAL_ID(id) ((u8)((id) - REMOTE_LOCAL_ID) < REMOTE_MAX_PLAYERS)

// Event kinds (low nibble of RemoteEvent.type). The sender's slot sits in
// bits 4-5, stamped by the receiving client.
#define REMOTE_EVENT_STEP   1
#define REMOTE_EVENT_SYNC   2
#define REMOTE_EVENT_TURN   3
#define REMOTE_EVENT_LEAVE  4
#define REMOTE_EVENT_KIND(t) ((t) & 0x0F)
#define REMOTE_EVENT_SLOT(t) (((t) >> 4) & 0x03)

// 1 = a ghost of the local player replays its own steps about 1.5 s late,
// so the module can be tested with no link hardware. Set to 0 when a
// transport feeds RemotePlayer_PushRx.
#ifndef REMOTE_PLAYER_LOOPBACK
#define REMOTE_PLAYER_LOOPBACK 0
#endif

struct RemoteEvent
{
    u8 type;
    u8 dir;          // DIR_*
    u8 action;       // sender's movement action id
    u8 seq;
    u8 mapGroup;
    u8 mapNum;
    s16 x;           // object event coordinates (map coordinates + MAP_OFFSET)
    s16 y;
    u8 gender;       // MALE / FEMALE
    u8 avatarState;  // PLAYER_AVATAR_STATE_*
};

STATIC_ASSERT(sizeof(struct RemoteEvent) == 12, RemoteEventIs12Bytes);

struct ObjectEvent;

void RemotePlayer_Init(void);
void RemotePlayer_Tick(void);

// Transport side. PushRx returns FALSE when the queue is full; PopTx returns
// FALSE when there is nothing to send.
bool8 RemotePlayer_PushRx(const struct RemoteEvent *event);
bool8 RemotePlayer_PopTx(struct RemoteEvent *event);

// Used by the battle module: a spare transmit slot, the next sequence number,
// whether the ghost (no real peer) is standing in, and where the remotes are.
bool8 RemotePlayer_TryPushTxPriority(const struct RemoteEvent *event);
bool8 RemotePlayer_TryPushTx(const struct RemoteEvent *event);
u8 RemotePlayer_NextTxSeq(void);
bool8 RemotePlayer_InLoopback(void);
u8 RemotePlayer_GetGender(u8 slot);
void RemotePlayer_DrainBattleRx(void);
bool8 RemotePlayer_SlotIsNear(u8 slot, u8 maxDistance, u8 *gender);
u8 RemotePlayer_FindObjectNear(s16 x, s16 y, u8 direction);
bool8 RemotePlayer_FindNearby(u8 maxDistance, u8 *slot, u8 *gender);

// Overwrites a save-file object event slot so it looks never used.
void ClearRemotePlayerSaveEntry(struct ObjectEvent *entry);

#endif // GUARD_REMOTE_PLAYER_H
