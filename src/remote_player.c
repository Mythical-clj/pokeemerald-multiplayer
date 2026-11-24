#include "global.h"
#include "event_object_movement.h"
#include "field_player_avatar.h"
#include "fieldmap.h"
#include "main.h"
#include "remote_player.h"
#include "constants/event_object_movement.h"
#include "constants/event_objects.h"
#include "constants/maps.h"

// How often the local position is announced as an absolute SYNC.
#define SYNC_PERIOD_FRAMES      60
// A SYNC further than this from the shown position repositions the remote
// instead of waiting for it to walk there.
#define DRIFT_TOLERANCE         3
// A STEP landing up to this far from the last known tile is made up by
// walking one tile at a time; further than that it becomes a reposition.
#define MEND_MAX                3
#define SPAWN_RETRY_FRAMES      60
// Remote players are only created inside this window around the local player.
#define VIEW_HALF_WIDTH         7
#define VIEW_HALF_HEIGHT        5

#define SLOT_QUEUE_LEN          6
#define RING_LEN                8
#define LOOPBACK_DELAY_FRAMES   90
#define LOOPBACK_RING_LEN       16

struct RemoteSlot
{
    u8 spawned;
    u8 objectId;
    u8 known;           // we have heard from this player since they (re)joined
    u8 gender;
    u8 mapGroup;        // map the player says they are on
    u8 mapNum;
    u8 forceResync;     // next positional event repositions instead of walking
    u8 spawnCooldown;
    s16 targetX;        // where they say they are, in their map's coordinates
    s16 targetY;
    u16 lastSeqTag;     // 0x100 | seq of the last queued STEP/TURN (dedup)
    u8 qHead;
    u8 qTail;
    struct RemoteEvent queue[SLOT_QUEUE_LEN];
};

struct LoopbackEntry
{
    struct RemoteEvent event;
    u16 dueFrame;
};

static EWRAM_DATA struct RemoteSlot sSlots[REMOTE_MAX_PLAYERS] = {0};
static EWRAM_DATA struct RemoteEvent sRxRing[RING_LEN] = {0};
static EWRAM_DATA struct RemoteEvent sTxRing[RING_LEN] = {0};
static EWRAM_DATA u8 sRxHead = 0;
static EWRAM_DATA u8 sRxTail = 0;
static EWRAM_DATA u8 sTxHead = 0;
static EWRAM_DATA u8 sTxTail = 0;
static EWRAM_DATA u8 sTxSeq = 0;
static EWRAM_DATA bool8 sInitialized = FALSE;

// Local player tracking (transmit side).
static EWRAM_DATA bool8 sHavePlayerPos = FALSE;
static EWRAM_DATA s16 sLastPlayerX = 0;
static EWRAM_DATA s16 sLastPlayerY = 0;
static EWRAM_DATA u8 sLastPlayerDir = 0;
static EWRAM_DATA u8 sLastMapGroup = 0;
static EWRAM_DATA u8 sLastMapNum = 0;
static EWRAM_DATA u16 sFramesSinceSync = 0;

#if REMOTE_PLAYER_LOOPBACK
static EWRAM_DATA struct LoopbackEntry sLoopback[LOOPBACK_RING_LEN] = {0};
static EWRAM_DATA u8 sLoopHead = 0;
static EWRAM_DATA u8 sLoopTail = 0;
static EWRAM_DATA u16 sFrameCounter = 0;
#endif

// Movement action families, each ordered DOWN, UP, LEFT, RIGHT, which is the
// same order as DIR_SOUTH, DIR_NORTH, DIR_WEST, DIR_EAST.
static const u8 sWalkFamilyBases[] =
{
    MOVEMENT_ACTION_WALK_SLOW_DOWN,
    MOVEMENT_ACTION_WALK_NORMAL_DOWN,
    MOVEMENT_ACTION_WALK_FAST_DOWN,
    MOVEMENT_ACTION_WALK_FASTER_DOWN,
    MOVEMENT_ACTION_PLAYER_RUN_DOWN,
};

static u8 DirIndex(u8 dir)
{
    return dir - DIR_SOUTH;
}

// The movement action to give a remote for a received action id, or 0xFF when
// nothing sensible can be done. Only actions the plain walking sprite has
// animations for are passed through; anything else becomes a normal walk.
static u8 ActionFor(u8 dir, u8 action, bool8 isStep)
{
    u8 d;
    u8 i;

    if (dir < DIR_SOUTH || dir > DIR_EAST)
        return 0xFF;
    d = DirIndex(dir);

    if (!isStep)
        return MOVEMENT_ACTION_FACE_DOWN + d;

    for (i = 0; i < ARRAY_COUNT(sWalkFamilyBases); i++)
    {
        if (action >= sWalkFamilyBases[i] && action < sWalkFamilyBases[i] + 4)
            return sWalkFamilyBases[i] + d;
    }
    return MOVEMENT_ACTION_WALK_NORMAL_DOWN + d;
}

static void RingPushTx(struct RemoteEvent *event)
{
    u8 next = (sTxHead + 1) % RING_LEN;

    if (next == sTxTail)
        return; // full: dropping the newest is safer than overwriting unread data
    sTxRing[sTxHead] = *event;
    sTxHead = next;
}

bool8 RemotePlayer_PopTx(struct RemoteEvent *event)
{
    if (sTxHead == sTxTail)
        return FALSE;
    *event = sTxRing[sTxTail];
    sTxTail = (sTxTail + 1) % RING_LEN;
    return TRUE;
}

bool8 RemotePlayer_PushRx(const struct RemoteEvent *event)
{
    u8 next = (sRxHead + 1) % RING_LEN;

    if (next == sRxTail)
        return FALSE;
    sRxRing[sRxHead] = *event;
    sRxHead = next;
    return TRUE;
}

void RemotePlayer_Init(void)
{
    u8 i;

    for (i = 0; i < REMOTE_MAX_PLAYERS; i++)
    {
        sSlots[i].spawned = FALSE;
        sSlots[i].objectId = OBJECT_EVENTS_COUNT;
        sSlots[i].known = FALSE;
        sSlots[i].forceResync = TRUE;
        sSlots[i].spawnCooldown = 0;
        sSlots[i].lastSeqTag = 0;
        sSlots[i].qHead = 0;
        sSlots[i].qTail = 0;
    }
    sRxHead = sRxTail = 0;
    sTxHead = sTxTail = 0;
    sHavePlayerPos = FALSE;
#if REMOTE_PLAYER_LOOPBACK
    sLoopHead = sLoopTail = 0;
#endif
}

// ---------------------------------------------------------------------------
// Transmit side: describe what the local player does
// ---------------------------------------------------------------------------

static void EmitEvent(u8 kind, struct ObjectEvent *player, u8 dir, u8 action)
{
    struct RemoteEvent event;

    event.type = kind;
    event.dir = dir;
    event.action = action;
    event.seq = sTxSeq++;
    event.mapGroup = gSaveBlock1Ptr->location.mapGroup;
    event.mapNum = gSaveBlock1Ptr->location.mapNum;
    event.x = player->currentCoords.x;
    event.y = player->currentCoords.y;
    event.gender = gSaveBlock2Ptr->playerGender;
    event.avatarState = PLAYER_AVATAR_STATE_NORMAL;
    RingPushTx(&event);

#if REMOTE_PLAYER_LOOPBACK
    {
        u8 next = (sLoopHead + 1) % LOOPBACK_RING_LEN;

        if (next != sLoopTail)
        {
            sLoopback[sLoopHead].event = event;
            sLoopback[sLoopHead].dueFrame = sFrameCounter + LOOPBACK_DELAY_FRAMES;
            sLoopHead = next;
        }
    }
#endif
}

static void TrackPlayer(void)
{
    struct ObjectEvent *player = &gObjectEvents[gPlayerAvatar.objectEventId];
    s16 x = player->currentCoords.x;
    s16 y = player->currentCoords.y;
    u8 dir = player->facingDirection;
    u8 mapGroup = gSaveBlock1Ptr->location.mapGroup;
    u8 mapNum = gSaveBlock1Ptr->location.mapNum;

    if (!sHavePlayerPos || mapGroup != sLastMapGroup || mapNum != sLastMapNum)
    {
        // First sample, or the map changed: tell everyone where we are. A map
        // change is never a step, however close the coordinates look.
        sHavePlayerPos = TRUE;
        sLastMapGroup = mapGroup;
        sLastMapNum = mapNum;
        sLastPlayerX = x;
        sLastPlayerY = y;
        sLastPlayerDir = dir;
        sFramesSinceSync = 0;
        EmitEvent(REMOTE_EVENT_SYNC, player, dir, player->movementActionId);
        return;
    }

    if (x != sLastPlayerX || y != sLastPlayerY)
    {
        EmitEvent(REMOTE_EVENT_STEP, player, dir, player->movementActionId);
    }
    else if (dir != sLastPlayerDir)
    {
        EmitEvent(REMOTE_EVENT_TURN, player, dir, player->movementActionId);
    }
    sLastPlayerX = x;
    sLastPlayerY = y;
    sLastPlayerDir = dir;

    if (++sFramesSinceSync >= SYNC_PERIOD_FRAMES)
    {
        sFramesSinceSync = 0;
        EmitEvent(REMOTE_EVENT_SYNC, player, dir, player->movementActionId);
    }
}

// ---------------------------------------------------------------------------
// Receive side: make the remote objects move as their players do
// ---------------------------------------------------------------------------

static bool8 SlotIsAlive(u8 slotId)
{
    struct RemoteSlot *slot = &sSlots[slotId];
    struct ObjectEvent *object;

    if (!slot->spawned)
        return FALSE;
    if (slot->objectId >= OBJECT_EVENTS_COUNT)
    {
        slot->spawned = FALSE;
        return FALSE;
    }
    object = &gObjectEvents[slot->objectId];
    // The game may have removed or recycled the object (leaving view, warping).
    if (!object->active || object->localId != REMOTE_LOCAL_ID + slotId)
    {
        slot->spawned = FALSE;
        return FALSE;
    }
    return TRUE;
}

static void DespawnSlot(u8 slotId)
{
    struct RemoteSlot *slot = &sSlots[slotId];

    if (SlotIsAlive(slotId))
        RemoveRemotePlayerObjectEvent(slot->objectId);
    slot->spawned = FALSE;
    slot->objectId = OBJECT_EVENTS_COUNT;
}

static bool8 OnMyMap(const struct RemoteEvent *event)
{
    return event->mapGroup == gSaveBlock1Ptr->location.mapGroup
        && event->mapNum == gSaveBlock1Ptr->location.mapNum;
}

static bool8 InsideView(s16 x, s16 y)
{
    struct ObjectEvent *player = &gObjectEvents[gPlayerAvatar.objectEventId];
    s16 dx = x - player->currentCoords.x;
    s16 dy = y - player->currentCoords.y;

    return dx >= -VIEW_HALF_WIDTH && dx <= VIEW_HALF_WIDTH
        && dy >= -VIEW_HALF_HEIGHT && dy <= VIEW_HALF_HEIGHT;
}

static void TrySpawn(u8 slotId, s16 x, s16 y, u8 dir)
{
    struct RemoteSlot *slot = &sSlots[slotId];
    struct ObjectEvent *player = &gObjectEvents[gPlayerAvatar.objectEventId];
    struct ObjectEvent *object;
    u8 gfx;
    u8 id;

    if (!player->active || !InsideView(x, y))
        return;

    gfx = GetRivalAvatarGraphicsIdByStateIdAndGender(PLAYER_AVATAR_STATE_NORMAL, slot->gender);
    id = SpawnSpecialObjectEventParameterized(gfx, MOVEMENT_TYPE_NONE, REMOTE_LOCAL_ID + slotId,
                                              x, y, player->currentElevation);
    if (id >= OBJECT_EVENTS_COUNT)
    {
        slot->spawnCooldown = SPAWN_RETRY_FRAMES;
        return;
    }

    object = &gObjectEvents[id];
    // Created while the camera is scrolling, the sprite can land off the tile
    // grid; placing it again puts it right.
    MoveObjectEventToMapCoords(object, x, y);
    if (dir >= DIR_SOUTH && dir <= DIR_EAST)
        ObjectEventTurn(object, dir);

    slot->objectId = id;
    slot->spawned = TRUE;
    // Spawning is already an absolute placement.
    slot->forceResync = FALSE;
}

static bool8 ReadyForMovement(struct ObjectEvent *object)
{
    if (!ObjectEventIsHeldMovementActive(object))
        return TRUE;
    if (ObjectEventCheckHeldMovementStatus(object))
    {
        ObjectEventClearHeldMovement(object);
        return TRUE;
    }
    return FALSE;
}

static void Reposition(u8 slotId, s16 x, s16 y, u8 dir)
{
    struct ObjectEvent *object = &gObjectEvents[sSlots[slotId].objectId];

    ObjectEventClearHeldMovementIfActive(object);
    MoveObjectEventToMapCoords(object, x, y);
    if (dir >= DIR_SOUTH && dir <= DIR_EAST)
        ObjectEventTurn(object, dir);
}

static s16 Abs16(s16 v)
{
    return v < 0 ? -v : v;
}

// Handles queued events for one slot. Returns when the remote is still busy
// with a previous movement; that event stays queued.
static void ConsumeSlotEvents(u8 slotId)
{
    struct RemoteSlot *slot = &sSlots[slotId];

    while (slot->qTail != slot->qHead)
    {
        struct RemoteEvent *event = &slot->queue[slot->qTail];
        u8 kind = REMOTE_EVENT_KIND(event->type);
        u8 next = (slot->qTail + 1) % SLOT_QUEUE_LEN;
        bool8 alive = SlotIsAlive(slotId);
        bool8 sameMap = OnMyMap(event);
        s16 x = event->x;
        s16 y = event->y;

        if (!slot->known || slot->mapGroup != event->mapGroup || slot->mapNum != event->mapNum)
            slot->forceResync = TRUE;

        slot->known = TRUE;
        slot->gender = event->gender;

        if (!sameMap)
        {
            // Shown only on the same map. (Neighbouring routes could be drawn
            // through map connections; not done yet.)
            if (alive)
                DespawnSlot(slotId);
            slot->mapGroup = event->mapGroup;
            slot->mapNum = event->mapNum;
            slot->targetX = x;
            slot->targetY = y;
            slot->qTail = next;
            continue;
        }

        if (!alive)
        {
            // Nothing on screen: any positional event places them directly.
            if (slot->spawnCooldown == 0)
                TrySpawn(slotId, x, y, event->dir);
            slot->mapGroup = event->mapGroup;
            slot->mapNum = event->mapNum;
            slot->targetX = x;
            slot->targetY = y;
            // If they could not be placed (out of view, no free object slot)
            // the event is dropped; the next SYNC tries again.
            slot->qTail = next;
            continue;
        }

        {
            struct ObjectEvent *object = &gObjectEvents[slot->objectId];

            if (kind == REMOTE_EVENT_STEP && !slot->forceResync)
            {
                s16 gapX = x - slot->targetX;
                s16 gapY = y - slot->targetY;
                s16 gap = Abs16(gapX) + Abs16(gapY);

                if (!ReadyForMovement(object))
                    return;

                if (gap > 1 && gap <= MEND_MAX && (gapX == 0 || gapY == 0))
                {
                    // Lost steps in a straight line: walk one tile and look
                    // at the same event again.
                    u8 mendDir;

                    if (gapX != 0)
                    {
                        mendDir = gapX > 0 ? DIR_EAST : DIR_WEST;
                        slot->targetX += gapX > 0 ? 1 : -1;
                    }
                    else
                    {
                        mendDir = gapY > 0 ? DIR_SOUTH : DIR_NORTH;
                        slot->targetY += gapY > 0 ? 1 : -1;
                    }
                    ObjectEventSetHeldMovement(object, ActionFor(mendDir, MOVEMENT_ACTION_WALK_FASTER_DOWN, TRUE));
                    return;
                }
                if (gap > MEND_MAX)
                {
                    kind = REMOTE_EVENT_SYNC;   // not a step: reposition
                }
                else
                {
                    u8 act = ActionFor(event->dir, event->action, TRUE);

                    if (act != 0xFF)
                        ObjectEventSetHeldMovement(object, act);
                    slot->targetX = x;
                    slot->targetY = y;
                    slot->qTail = next;
                    continue;
                }
            }
            else if (kind == REMOTE_EVENT_STEP || (kind == REMOTE_EVENT_TURN && slot->forceResync))
            {
                kind = REMOTE_EVENT_SYNC;
            }

            if (kind == REMOTE_EVENT_TURN)
            {
                u8 act = ActionFor(event->dir, event->action, FALSE);

                if (!ReadyForMovement(object))
                    return;
                if (act != 0xFF)
                    ObjectEventSetHeldMovement(object, act);
                slot->qTail = next;
                continue;
            }

            if (kind == REMOTE_EVENT_SYNC)
            {
                s16 curX = object->currentCoords.x;
                s16 curY = object->currentCoords.y;
                s16 drift = Abs16(x - curX) + Abs16(y - curY);

                if (slot->forceResync || drift > DRIFT_TOLERANCE)
                {
                    Reposition(slotId, x, y, event->dir);
                    slot->forceResync = FALSE;
                }
                slot->targetX = x;
                slot->targetY = y;
            }
            slot->qTail = next;
        }
    }
}

static void QueueEvent(const struct RemoteEvent *event)
{
    u8 kind = REMOTE_EVENT_KIND(event->type);
    u8 slotId = REMOTE_EVENT_SLOT(event->type);
    struct RemoteSlot *slot;
    u8 next;

    if (slotId >= REMOTE_MAX_PLAYERS)
        return;
    slot = &sSlots[slotId];

    if (kind == REMOTE_EVENT_LEAVE)
    {
        DespawnSlot(slotId);
        slot->known = FALSE;
        slot->forceResync = TRUE;
        slot->qHead = slot->qTail = 0;
        return;
    }

    if (kind != REMOTE_EVENT_STEP && kind != REMOTE_EVENT_SYNC && kind != REMOTE_EVENT_TURN)
        return;

    // Every STEP/TURN may arrive twice (redundancy against loss): the second
    // copy has the same sequence number.
    if (kind != REMOTE_EVENT_SYNC)
    {
        u16 tag = 0x100 | event->seq;

        if (tag == slot->lastSeqTag)
            return;
        slot->lastSeqTag = tag;
    }

    next = (slot->qHead + 1) % SLOT_QUEUE_LEN;
    if (next == slot->qTail)
        return;     // queue full; a SYNC will bring us back in line
    slot->queue[slot->qHead] = *event;
    slot->qHead = next;
}

static void DrainRx(void)
{
    while (sRxTail != sRxHead)
    {
        QueueEvent(&sRxRing[sRxTail]);
        sRxTail = (sRxTail + 1) % RING_LEN;
    }
}

#if REMOTE_PLAYER_LOOPBACK
static void DrainLoopback(void)
{
    while (sLoopTail != sLoopHead)
    {
        struct RemoteEvent event;

        // dueFrame wraps; compare as a signed difference.
        if ((s16)(sFrameCounter - sLoopback[sLoopTail].dueFrame) < 0)
            break;
        event = sLoopback[sLoopTail].event;
        event.type &= 0x0F;     // slot 0
        if (!RemotePlayer_PushRx(&event))
            break;
        sLoopTail = (sLoopTail + 1) % LOOPBACK_RING_LEN;
    }
}
#endif

// Called once per frame from the overworld's main callback, after the game's
// own input handling. Never from an interrupt: creating and moving objects is
// only safe in sequence with the game's own overworld code.
void RemotePlayer_Tick(void)
{
    u8 i;

    if (!sInitialized)
    {
        RemotePlayer_Init();
        sInitialized = TRUE;
    }

    if (!gObjectEvents[gPlayerAvatar.objectEventId].active)
        return;

#if REMOTE_PLAYER_LOOPBACK
    sFrameCounter++;
    DrainLoopback();
#endif

    TrackPlayer();
    DrainRx();
    for (i = 0; i < REMOTE_MAX_PLAYERS; i++)
    {
        if (sSlots[i].spawnCooldown != 0)
            sSlots[i].spawnCooldown--;
        ConsumeSlotEvents(i);
    }
}

// Same result as the game's own ClearObjectEvent: nothing of a remote player
// is left in the save file.
void ClearRemotePlayerSaveEntry(struct ObjectEvent *entry)
{
    u8 *bytes = (u8 *)entry;
    u32 i;

    for (i = 0; i < sizeof(struct ObjectEvent); i++)
        bytes[i] = 0;
    entry->localId = LOCALID_PLAYER;
    entry->mapNum = MAP_NUM(MAP_UNDEFINED);
    entry->mapGroup = MAP_GROUP(MAP_UNDEFINED);
    entry->movementActionId = MOVEMENT_ACTION_NONE;
}
