#ifndef GUARD_REMOTE_SPECTATE_H
#define GUARD_REMOTE_SPECTATE_H

#include "global.h"

struct RemoteEvent;

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

#endif // GUARD_REMOTE_SPECTATE_H
