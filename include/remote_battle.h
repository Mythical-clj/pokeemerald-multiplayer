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
#define REMOTE_EVENT_BSTART 12  // host: "fight this trainer together" (seed, trainers, teams)
#define REMOTE_EVENT_BSYNC  13  // acknowledgements, both ways, for the start and for choices
#define REMOTE_EVENT_BCHOICE 14 // one menu choice of the sender's player

#ifndef REMOTE_BATTLE_ENABLED
#define REMOTE_BATTLE_ENABLED 1
#endif

struct RemoteEvent;
struct VersusRequest;

extern bool8 gRemoteBattleActive;   // the current battle has a remote partner
extern bool8 gRemoteSplitTrainer;   // one trainer plays both opponent slots

// One thing a player's battle menu told the engine. The other game replays it
// on the battler that stands for that player.
struct RemoteChoice
{
    u8 kind;    // 1: two return values, 2: chosen Pokémon, 3: one return value
    u8 a;
    u8 b;
    u8 c;
    u8 d;
};

extern bool8 gRemoteHumanBattle;    // the partner is a person, both games run the battle

void RemoteBattle_Init(void);
void RemoteBattle_Tick(void);
void RemoteBattle_HandleRx(const struct RemoteEvent *event);

// Called when a trainer battle is about to start. Returns TRUE when a remote
// partner joined (battle flags, parties and callbacks are set up).
bool8 RemoteBattle_PrepareTrainerBattle(void);

// A trainer that was already beaten battles again when a remote partner
// stands nearby and the player talks to them. Returns TRUE to ignore the flag.
bool8 RemoteBattle_ShouldRematch(void);

// While a human partner battle is waiting for the partner's game to accept.
bool8 RemoteBattle_AwaitingStartAck(void);
bool8 RemoteBattle_Yielded(void);
bool8 RemoteBattle_StartTimedOut(void);
void RemoteBattle_ShowWaiting(u8 which);   // 0 waiting, 1 no reply, 2 come closer
bool8 RemoteBattle_AwaitingNear(void);
void RemoteBattle_HideWaiting(void);
u8 RemoteBattle_GetPartnerLook(void);
u8 RemoteBattle_GetVersusLook(void);
void RemoteBattle_VersusPhraseToString(u8 which);

// Battle side. Tick runs every frame of the battle.
void RemoteBattle_BattleTick(void);
void RemoteBattle_ApplySeed(void);
bool8 RemoteBattle_IsGuest(void);
// A versus battle against the person in a slot, agreed in the overworld.
bool8 RemoteBattle_BeginVersus(const struct VersusRequest *request);
u8 RemoteBattle_GetTeamTag(void);
extern bool8 gRemoteVersus;         // the opponent is a person, both games run the battle
void RemoteBattle_FillVersusParty(void);
bool8 RemoteBattle_TieRoll(void);
u8 RemoteBattle_VersusTrainerPic(void);
const u8 *RemoteBattle_VersusClassName(void);
// The player's own battle menu: a menu entry the rules take away.
bool8 RemoteBattle_BlockAction(u8 cursor);
void RemoteBattle_ClearItemEffect(void);
void RemoteBattle_NoteItemEffect(u8 partyId, u8 moveIndex);
void RemoteBattle_ApplyItemEffect(u8 battler, u16 item, u8 partyPlus1, u8 moveIndex);
// Copies what the local player's menu emits to the other game.
void RemoteBattle_CaptureTwoReturn(u8 battler, u8 ret8, u16 ret16);
void RemoteBattle_CaptureChosenMon(u8 battler, u8 partyId, const u8 *order);
void RemoteBattle_CaptureOne(u8 battler, u16 ret);
// The partner battler: replay what the other player chose.
bool8 RemoteBattle_ControlsBattler(u8 battler);
bool8 RemoteBattle_PopChoice(struct RemoteChoice *choice);
bool8 RemoteBattle_WaitTimedOut(void);
void RemoteBattle_GiveUpOnRemote(void);

// Strings and picture for the partner in battle.
const u8 *RemoteBattle_GetPartnerName(void);
void RemoteBattle_SetSpectateMirror(bool8 mirrored);
u32 RemoteBattle_GetSeed(void);
void RemoteBattle_SetSpectatePartner(const u8 *name, u8 gender, u8 look);
u8 RemoteBattle_GetPartnerGender(void);
bool8 RemoteBattle_IsCoopFight(void);
void RemoteBattle_GetCoopSides(const u8 *names[2], u8 genders[2], u8 looks[2]);
u16 RemoteBattle_GetOppPhraseWord(u8 which, u8 idx);
void RemoteBattle_SetSpectatePhrase(u8 which, u8 idx, u16 word);
void RemoteBattle_SetSpectateOwnPhrase(u8 which, u8 idx, u16 word);
void RemoteBattle_AppendWatchedPhrase(u8 which);
u16 RemoteBattle_GetOwnPhraseWord(u8 which, u8 idx);
bool8 RemoteBattle_IsMirroredVersus(void);
const u8 *RemoteBattle_GetNameOfSlot(u8 slot);
u8 RemoteBattle_GetPartnerBackPic(void);

#endif // GUARD_REMOTE_BATTLE_H
