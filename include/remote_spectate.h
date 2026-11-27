#ifndef GUARD_REMOTE_SPECTATE_H
#define GUARD_REMOTE_SPECTATE_H

#include "global.h"

struct RemoteEvent;

#define SUB_SPEC_SETUP  20  // a = chunk, 8 bytes: seed/flags, then the two starting teams
#define SUB_SPEC_REC    21  // a = battler, b = count, 8 bytes: position (2) + up to 6 recorded bytes
#define SUB_SPEC_ACK    22  // watcher to fighters: a = setup done, b = tag low byte, 4 x u16 bytes received

// Message kinds inside REMOTE_EVENT_SOCIAL (sub byte). Anything from here up
// belongs to the spectator module.
#define SUB_SPEC_MON    16  // a = battler (0-3), 8 bytes: species, level, hp, max hp, status
#define SUB_SPEC_INFO   17  // a = 1 "my player is in a fight", 8 bytes: the opponent's name
#define SUB_SPEC_TEXT   18  // a = line number, b = part, 8 bytes of the line
#define SUB_SPEC_JOIN   19  // 8 bytes: [0-1] tag of the watched player's name

void RemoteSpectate_HandleRx(u8 slot, const struct RemoteEvent *event);
bool8 RemoteSpectate_IsFighting(u8 slot);

// Fighter side
void RemoteSpectate_ResetSender(void);
void RemoteSpectate_BroadcastTick(void);
void RemoteSpectate_NoteText(const u8 *text);

// Script special: starts watching the player that was talked to.
void RemoteSpectate_Start(void);

// Called every battle frame: keeps the link going for a watcher and returns
// how many extra frames to run (the R button skips ahead).
u8 RemoteSpectate_FrameHook(void);
bool8 RemoteSpectate_WantsQuit(void);
bool8 RemoteSpectate_IsWatching(void);
u8 RemoteSpectate_OppLook(void);

#endif // GUARD_REMOTE_SPECTATE_H
