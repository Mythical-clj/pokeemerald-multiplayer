#ifndef GUARD_REMOTE_BATTLE_H
#define GUARD_REMOTE_BATTLE_H

// Trainer battles fought together with a remote player.
//
// Each player keeps sending the first three usable Pokémon of their party in
// small chunks. When a trainer battle starts while a remote player stands
// close by, that team joins as the AI-controlled partner of the game's own
// multi battle (the one used for Steven), and the trainer's team is split
// over both opponent slots. Experience the partner's Pokémon earn is sent
// back, and the remote player's game applies it to the matching Pokémon.

// Event kinds. 5 to 9 belong to the sync project's own events.
#define REMOTE_EVENT_BPARTY 10  // a chunk of the sender's team
#define REMOTE_EVENT_BEXP   11  // experience earned by one of the sender's team

#ifndef REMOTE_BATTLE_ENABLED
#define REMOTE_BATTLE_ENABLED 1
#endif

struct RemoteEvent;

extern bool8 gRemoteBattleActive;   // the current battle has a remote partner
extern bool8 gRemoteSplitTrainer;   // one trainer plays both opponent slots

void RemoteBattle_Init(void);
void RemoteBattle_Tick(void);
void RemoteBattle_HandleRx(const struct RemoteEvent *event);

// Called when a trainer battle is about to start. Returns TRUE when a remote
// partner joined (battle flags, parties and callbacks are set up).
bool8 RemoteBattle_PrepareTrainerBattle(void);

// A trainer that was already beaten battles again when a remote partner
// stands nearby and the player talks to them. Returns TRUE to ignore the flag.
bool8 RemoteBattle_ShouldRematch(void);

// Strings and picture for the partner in battle.
const u8 *RemoteBattle_GetPartnerName(void);
u8 RemoteBattle_GetPartnerBackPic(void);

#endif // GUARD_REMOTE_BATTLE_H
