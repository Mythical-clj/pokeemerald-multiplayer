#ifndef GUARD_REMOTE_SOCIAL_H
#define GUARD_REMOTE_SOCIAL_H

// Talking to a remote player in the overworld: a menu to ask for a battle or
// a trade, and a chat made of easy chat words. Everything goes over the
// same 12-byte events as the rest of the module, as one event kind with a
// sub type in the second byte.

#define REMOTE_EVENT_SOCIAL 15

#define SOCIAL_REQUEST_BATTLE   1
#define SOCIAL_REQUEST_TRADE    2

struct RemoteEvent;

void RemoteSocial_Init(void);
void RemoteSocial_Tick(void);
void RemoteSocial_HandleRx(u8 slot, const struct RemoteEvent *event);

// A versus battle was agreed on; the battle module takes it from here.
bool8 RemoteSocial_TakeBattleRequest(u8 *slot, bool8 *isRequester, u8 *session);

extern const u8 RemoteSocial_EventScript_Interact[];
extern const u8 RemoteSocial_EventScript_Incoming[];
extern const u8 RemoteSocial_EventScript_ShowChat[];

#endif // GUARD_REMOTE_SOCIAL_H
