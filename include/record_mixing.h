#ifndef GUARD_RECORD_MIXING_H
#define GUARD_RECORD_MIXING_H

struct PlayerHallRecords
{
    struct RankingHall1P onePlayer[HALL_FACILITIES_COUNT][FRONTIER_LVL_MODE_COUNT];
    struct RankingHall2P twoPlayers[FRONTIER_LVL_MODE_COUNT];
};

// Overworld record mixing with one other player (see remote_social.c): the
// record holds what the real mixing exchanges for secret bases, TV shows,
// news, the Mauville old man, Dewford trends and the Lilycove lady.
#define REMOTE_MIX_MAX_BYTES 1500

u16 RemoteMix_GetSize(void);
void RemoteMix_Build(void *out);
bool8 RemoteMix_Apply(const void *theirs);

void RecordMixingPlayerSpotTriggered(void);
void GetPlayerHallRecords(struct PlayerHallRecords *dst);

#endif //GUARD_RECORD_MIXING_H
