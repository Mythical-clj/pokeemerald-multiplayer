#ifndef GUARD_REMOTE_SOCIAL_H
#define GUARD_REMOTE_SOCIAL_H

// Talking to a remote player in the overworld: a menu to ask for a battle or
// a trade, and a chat made of easy chat words. Everything goes over the
// same 12-byte events as the rest of the module, as one event kind with a
// sub type in the second byte.

#define REMOTE_EVENT_SOCIAL 15

#define SOCIAL_REQUEST_BATTLE   1
#define SOCIAL_REQUEST_TRADE    2
#define SOCIAL_REQUEST_MIX      3

struct RemoteEvent;

void RemoteSocial_Init(void);
void RemoteSocial_Tick(void);
void RemoteSocial_HandleRx(u8 slot, const struct RemoteEvent *event);

// What a versus battle needs, agreed in the overworld: who, which session,
// the shared random seed and the team each side saw of the other.
struct VersusRequest
{
    u8 slot;
    u8 requester;   // TRUE for the player who asked
    u8 session;
    u8 myTag;
    u8 peerTag;
    u32 seed;
};

bool8 RemoteSocial_TakeBattleRequest(struct VersusRequest *out);

extern const u8 RemoteSocial_EventScript_Interact[];
extern const u8 RemoteSocial_EventScript_Incoming[];
extern const u8 RemoteSocial_EventScript_ShowChat[];

#endif // GUARD_REMOTE_SOCIAL_H
