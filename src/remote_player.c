#include "global.h"
#include "overworld.h"
#include "event_object_movement.h"
#include "field_effect.h"
#include "field_effect_helpers.h"
#include "field_player_avatar.h"
#include "fieldmap.h"
#include "main.h"
#include "remote_battle.h"
#include "remote_player.h"
#include "remote_sio.h"
#include "remote_social.h"
#include "sprite.h"
#include "constants/event_object_movement.h"
#include "constants/event_objects.h"
#include "constants/field_effects.h"
#include "constants/maps.h"

// How often the local position is announced as an absolute SYNC.
#define SYNC_PERIOD_FRAMES      60
// A SYNC further than this from the shown position repositions the remote
// instead of waiting for it to walk there.
#define DRIFT_TOLERANCE         3
// A STEP landing up to this far from the last known tile is made up by
// walking one tile at a time; further than that it becomes a reposition.
#define MEND_MAX                3
// Queued events at which the remote speeds up to catch up.
#define HURRY_THRESHOLD         4
#define SPAWN_RETRY_FRAMES      60
#define SILENT_DESPAWN_FRAMES   600     // each player speaks at least once a second
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
    u8 avatarState;     // PLAYER_AVATAR_STATE_*, decides the sprite
    u8 blobSpriteId;    // surf blob under a surfing player, MAX_SPRITES = none
    u8 mapGroup;        // map the player says they are on
    u8 mapNum;
    u8 forceResync;     // next positional event repositions instead of walking
    u8 spawnCooldown;
    u16 silentFrames;       // since the last event from this player
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
// Ring indices are volatile: the serial interrupt writes the receive ring and
// reads the transmit ring while the main loop does the opposite.
static EWRAM_DATA struct RemoteEvent sRxRing[RING_LEN] = {0};
static EWRAM_DATA struct RemoteEvent sTxRing[RING_LEN] = {0};
static EWRAM_DATA vu8 sRxHead = 0;
static EWRAM_DATA vu8 sRxTail = 0;
static EWRAM_DATA vu8 sTxHead = 0;
static EWRAM_DATA vu8 sTxTail = 0;
static EWRAM_DATA u8 sTxSeq = 0;
static EWRAM_DATA bool8 sInitialized = FALSE;
// Set when a real event has arrived from the link; the loopback ghost stops.
static EWRAM_DATA vu8 sRealPeerSeen = FALSE;

// Local player tracking (transmit side).
static EWRAM_DATA bool8 sHavePlayerPos = FALSE;
static EWRAM_DATA s16 sLastPlayerX = 0;
static EWRAM_DATA s16 sLastPlayerY = 0;
static EWRAM_DATA u8 sLastPlayerDir = 0;
static EWRAM_DATA u8 sLastAvatarState = 0;
static EWRAM_DATA u8 sLastMapGroup = 0;
static EWRAM_DATA u8 sLastMapNum = 0;
static EWRAM_DATA u16 sFramesSinceSync = 0;

#if REMOTE_PLAYER_LOOPBACK
static EWRAM_DATA struct LoopbackEntry sLoopback[LOOPBACK_RING_LEN] = {0};
static EWRAM_DATA u8 sLoopHead = 0;
static EWRAM_DATA u8 sLoopTail = 0;
static EWRAM_DATA u16 sFrameCounter = 0;
#endif

// Animations in each avatar state's sprite table, indexed by
// PLAYER_AVATAR_STATE_*. An action is only used when its animation exists:
// running and surf jumps need 24, the acro bike 40.
static const u8 sAnimCount[] =
{
    [PLAYER_AVATAR_STATE_NORMAL]     = 24,
    [PLAYER_AVATAR_STATE_MACH_BIKE]  = 20,
    [PLAYER_AVATAR_STATE_ACRO_BIKE]  = 40,
    [PLAYER_AVATAR_STATE_SURFING]    = 24,
    [PLAYER_AVATAR_STATE_UNDERWATER] = 20,
    [PLAYER_AVATAR_STATE_FIELD_MOVE] = 1,
    [PLAYER_AVATAR_STATE_FISHING]    = 12,
    [PLAYER_AVATAR_STATE_WATERING]   = 20,
};

static bool8 IsValidRemoteAction(u8 act)
{
    // Only actions a player produces: facing, walking, jumping one ledge,
    // faster gaits and running, the surf jump, jumps, acro bike tricks.
    // Excludes things such as SET_INVISIBLE or emotes.
    return act <= 0x0F
        || (act >= 0x15 && act <= 0x38)
        || (act >= 0x3A && act <= 0x3D)
        || (act >= 0x42 && act <= 0x4D)
        || (act >= 0x64 && act <= 0x8B);
}

// The movement action to give a remote for a received action id, or 0xFF when
// nothing sensible can be done. An action the sprite has no animation for
// becomes a normal walk (or a turn); a bad animation index would read garbage.
// Each family of actions is ordered DOWN, UP, LEFT, RIGHT, which is the same
// order as DIR_SOUTH, DIR_NORTH, DIR_WEST, DIR_EAST.
static u8 ActionFor(u8 avatarState, u8 dir, u8 action, bool8 isStep, bool8 hurry)
{
    u8 count;
    u8 d;
    u8 base;
    u8 act;
    u8 need;

    if (avatarState >= ARRAY_COUNT(sAnimCount))
        return 0xFF;
    count = sAnimCount[avatarState];
    if (dir < DIR_SOUTH || dir > DIR_EAST || count < 20)
        return 0xFF;

    d = dir - DIR_SOUTH;
    base = isStep ? MOVEMENT_ACTION_WALK_NORMAL_DOWN : MOVEMENT_ACTION_FACE_DOWN;
    act = action;
    need = 20;

    if (!IsValidRemoteAction(act))
        act = base + d;

    if (act >= 0x64)
        need = 40;
    else if (act >= 0x35 && act <= 0x3D)
        need = 24;
    if (count < need)
        act = base + d;

    // When events pile up the remote moves faster than the gait it was told
    // to use, until it has caught up. Only plain gaits: a hurried jump would
    // no longer be a jump.
    if (hurry && ((u8)(act - MOVEMENT_ACTION_WALK_NORMAL_DOWN) < 4
               || (u8)(act - MOVEMENT_ACTION_WALK_FAST_DOWN) < 4
               || (u8)(act - MOVEMENT_ACTION_PLAYER_RUN_DOWN) < 4))
        act = MOVEMENT_ACTION_WALK_FASTER_DOWN + d;

    return act;
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

// Called from the serial interrupt.
bool8 RemotePlayer_PushRx(const struct RemoteEvent *event)
{
    u8 next = (sRxHead + 1) % RING_LEN;

    if (next == sRxTail)
        return FALSE;
    sRxRing[sRxHead] = *event;
    sRxHead = next;
    sRealPeerSeen = TRUE;
    return TRUE;
}

// For the messages a fight depends on: the ring may fill up completely, as
// the team chunks (which repeat anyway) are what gives way to them.
bool8 RemotePlayer_TryPushTxPriority(const struct RemoteEvent *event)
{
    u8 next = (sTxHead + 1) % RING_LEN;

    if (next == sTxTail)
        return FALSE;
    RingPushTx((struct RemoteEvent *)event);
    return TRUE;
}

bool8 RemotePlayer_TryPushTx(const struct RemoteEvent *event)
{
    u8 used = (sTxHead + RING_LEN - sTxTail) % RING_LEN;

    // Walking events come first: only use the ring while it is mostly empty.
    if (used >= RING_LEN / 2)
        return FALSE;
    RingPushTx((struct RemoteEvent *)event);
    return TRUE;
}

u8 RemotePlayer_NextTxSeq(void)
{
    return sTxSeq++;
}

u8 RemotePlayer_GetGender(u8 slot)
{
    return slot < REMOTE_MAX_PLAYERS ? sSlots[slot].gender : MALE;
}

// During a battle only the battle kinds matter; the rest is dropped.
void RemotePlayer_DrainBattleRx(void)
{
    while (sRxTail != sRxHead)
    {
        u8 kind = REMOTE_EVENT_KIND(sRxRing[sRxTail].type);

        if (kind >= REMOTE_EVENT_BPARTY)
            RemoteBattle_HandleRx(&sRxRing[sRxTail]);
        sRxTail = (sRxTail + 1) % RING_LEN;
    }
}

bool8 RemotePlayer_InLoopback(void)
{
#if REMOTE_PLAYER_LOOPBACK
    return !sRealPeerSeen;
#else
    return FALSE;
#endif
}

void RemotePlayer_Init(void)
{
    u8 i;

    for (i = 0; i < REMOTE_MAX_PLAYERS; i++)
    {
        sSlots[i].spawned = FALSE;
        sSlots[i].objectId = OBJECT_EVENTS_COUNT;
        sSlots[i].blobSpriteId = MAX_SPRITES;
        sSlots[i].avatarState = PLAYER_AVATAR_STATE_NORMAL;
        sSlots[i].known = FALSE;
        sSlots[i].forceResync = TRUE;
        sSlots[i].spawnCooldown = 0;
        sSlots[i].silentFrames = 0;
        sSlots[i].lastSeqTag = 0;
        sSlots[i].qHead = 0;
        sSlots[i].qTail = 0;
    }
    RemoteBattle_Init();
    RemoteSocial_Init();
    RemoteSio_Init();
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

// What the local player is, which decides the sprite others draw for them.
// Running is still the normal state: it is another animation of the same sprite.
static u8 LocalAvatarState(void)
{
    u8 flags = gPlayerAvatar.flags;

    // The HM pose (arm raised) is only a graphics swap on the local player.
    if (gPlayerAvatar.objectEventId < OBJECT_EVENTS_COUNT
     && gObjectEvents[gPlayerAvatar.objectEventId].graphicsId
        == GetPlayerAvatarGraphicsIdByStateId(PLAYER_AVATAR_STATE_FIELD_MOVE))
        return PLAYER_AVATAR_STATE_FIELD_MOVE;
    if (flags & PLAYER_AVATAR_FLAG_UNDERWATER)
        return PLAYER_AVATAR_STATE_UNDERWATER;
    if (flags & PLAYER_AVATAR_FLAG_SURFING)
        return PLAYER_AVATAR_STATE_SURFING;
    if (flags & PLAYER_AVATAR_FLAG_ACRO_BIKE)
        return PLAYER_AVATAR_STATE_ACRO_BIKE;
    if (flags & PLAYER_AVATAR_FLAG_MACH_BIKE)
        return PLAYER_AVATAR_STATE_MACH_BIKE;
    return PLAYER_AVATAR_STATE_NORMAL;
}

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
    event.avatarState = LocalAvatarState();
    RingPushTx(&event);

#if REMOTE_PLAYER_LOOPBACK
    if (!sRealPeerSeen)
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
    u8 state = LocalAvatarState();
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
        sLastAvatarState = state;
        sFramesSinceSync = 0;
        EmitEvent(REMOTE_EVENT_SYNC, player, dir, player->movementActionId);
        return;
    }

    if (x != sLastPlayerX || y != sLastPlayerY)
    {
        EmitEvent(REMOTE_EVENT_STEP, player, dir, player->movementActionId);
    }
    else if (dir != sLastPlayerDir || state != sLastAvatarState)
    {
        // Turning on the spot moves no coordinates, and mounting a bike or
        // diving changes the sprite without a step: say so explicitly.
        EmitEvent(REMOTE_EVENT_TURN, player, dir, player->movementActionId);
    }
    sLastPlayerX = x;
    sLastPlayerY = y;
    sLastPlayerDir = dir;
    sLastAvatarState = state;

    if (++sFramesSinceSync >= SYNC_PERIOD_FRAMES)
    {
        sFramesSinceSync = 0;
        EmitEvent(REMOTE_EVENT_SYNC, player, dir, player->movementActionId);
    }
}

// ---------------------------------------------------------------------------
// Receive side: make the remote objects move as their players do
// ---------------------------------------------------------------------------

static bool8 BlobIsMine(u8 slotId)
{
    struct RemoteSlot *slot = &sSlots[slotId];
    struct Sprite *sprite;

    if (slot->blobSpriteId >= MAX_SPRITES)
        return FALSE;
    sprite = &gSprites[slot->blobSpriteId];
    // The id alone proves nothing: sprites are recycled.
    return sprite->inUse
        && sprite->callback == UpdateSurfBlobFieldEffect
        && sprite->data[2] == slot->objectId;
}

static void DestroyBlob(u8 slotId)
{
    struct RemoteSlot *slot = &sSlots[slotId];

    if (BlobIsMine(slotId))
    {
        DestroySprite(&gSprites[slot->blobSpriteId]);
        // The bobbing wrote an offset into the rider's sprite every frame;
        // with the blob gone nothing would take it back out.
        if (slot->objectId < OBJECT_EVENTS_COUNT && gObjectEvents[slot->objectId].spriteId < MAX_SPRITES)
        {
            struct Sprite *rider = &gSprites[gObjectEvents[slot->objectId].spriteId];

            rider->x2 = 0;
            rider->y2 = 0;
        }
    }
    slot->blobSpriteId = MAX_SPRITES;
}

static void CreateBlob(u8 slotId)
{
    struct RemoteSlot *slot = &sSlots[slotId];
    struct ObjectEvent *object = &gObjectEvents[slot->objectId];
    s32 saved[3];
    u32 spriteId;

    if (BlobIsMine(slotId))
        return;

    // The field effect takes its parameters from a shared array the game also
    // uses to pass parameters between its own steps; leave it as found.
    saved[0] = gFieldEffectArguments[0];
    saved[1] = gFieldEffectArguments[1];
    saved[2] = gFieldEffectArguments[2];
    gFieldEffectArguments[0] = object->currentCoords.x;
    gFieldEffectArguments[1] = object->currentCoords.y;
    gFieldEffectArguments[2] = slot->objectId;
    spriteId = FieldEffectStart(FLDEFF_SURF_BLOB);
    gFieldEffectArguments[0] = saved[0];
    gFieldEffectArguments[1] = saved[1];
    gFieldEffectArguments[2] = saved[2];

    if (spriteId >= MAX_SPRITES)
        return;
    object->fieldEffectSpriteId = spriteId;
    slot->blobSpriteId = spriteId;
    SetSurfBlob_BobState(spriteId, BOB_PLAYER_AND_MON);
}

static s16 Abs16(s16 v);

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
        DestroyBlob(slotId);
        slot->spawned = FALSE;
        return FALSE;
    }
    return TRUE;
}

static void DespawnSlot(u8 slotId)
{
    struct RemoteSlot *slot = &sSlots[slotId];

    if (SlotIsAlive(slotId))
    {
        DestroyBlob(slotId);
        RemoveRemotePlayerObjectEvent(slot->objectId);
    }
    slot->spawned = FALSE;
    slot->objectId = OBJECT_EVENTS_COUNT;
}

bool8 RemotePlayer_SlotIsNear(u8 slot, u8 maxDistance, u8 *gender)
{
    struct ObjectEvent *player = &gObjectEvents[gPlayerAvatar.objectEventId];
    struct ObjectEvent *object;
    s16 dist;

    if (slot >= REMOTE_MAX_PLAYERS || !SlotIsAlive(slot))
        return FALSE;
    object = &gObjectEvents[sSlots[slot].objectId];
    dist = Abs16(object->currentCoords.x - player->currentCoords.x)
         + Abs16(object->currentCoords.y - player->currentCoords.y);
    if (dist > maxDistance)
        return FALSE;
    *gender = sSlots[slot].gender;
    return TRUE;
}

bool8 RemotePlayer_FindNearby(u8 maxDistance, u8 *slot, u8 *gender)
{
    u8 i;

    for (i = 0; i < REMOTE_MAX_PLAYERS; i++)
    {
        if (RemotePlayer_SlotIsNear(i, maxDistance, gender))
        {
            *slot = i;
            return TRUE;
        }
    }
    return FALSE;
}

// The remote player the avatar means when pressing A towards (x, y): on the
// tile itself or one tile further on the same line, so being a tile out of
// step with the other game does not make them unreachable.
u8 RemotePlayer_FindObjectNear(s16 x, s16 y, u8 direction)
{
    u8 i;
    s16 farX = x + gDirectionToVectors[direction].x;
    s16 farY = y + gDirectionToVectors[direction].y;

    for (i = 0; i < REMOTE_MAX_PLAYERS; i++)
    {
        struct ObjectEvent *object;

        if (!SlotIsAlive(i))
            continue;
        object = &gObjectEvents[sSlots[i].objectId];
        if ((object->currentCoords.x == x && object->currentCoords.y == y)
         || (object->currentCoords.x == farX && object->currentCoords.y == farY))
            return sSlots[i].objectId;
    }
    return OBJECT_EVENTS_COUNT;
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

    gfx = GetRivalAvatarGraphicsIdByStateIdAndGender(slot->avatarState, slot->gender);
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

// Makes the sprite match the player's avatar state and gender. The sprites
// are not all the same size (256 bytes on foot, 512 on a bike or surfing), and
// changing graphics does not reallocate tiles, so a size change recreates the
// object instead.
static void SyncGraphics(u8 slotId)
{
    struct RemoteSlot *slot = &sSlots[slotId];
    struct ObjectEvent *object = &gObjectEvents[slot->objectId];
    u8 want = GetRivalAvatarGraphicsIdByStateIdAndGender(slot->avatarState, slot->gender);
    u8 have = object->graphicsId;

    if (want == have)
        return;

    if (GetObjectEventGraphicsInfo(want)->size != GetObjectEventGraphicsInfo(have)->size)
    {
        s16 x = object->currentCoords.x;
        s16 y = object->currentCoords.y;
        u8 dir = object->facingDirection;

        DespawnSlot(slotId);
        // Same frame, no cooldown: the tile is already on screen.
        slot->spawnCooldown = 0;
        TrySpawn(slotId, x, y, dir);
        return;
    }

    // A running action in progress would keep pushing the animation index out
    // of range for its whole duration.
    ObjectEventClearHeldMovementIfActive(object);
    ObjectEventSetGraphicsId(object, want);
    if (sAnimCount[slot->avatarState] >= 20)
        ObjectEventTurn(object, object->movementDirection);
    else
        StartSpriteAnim(&gSprites[object->spriteId], ANIM_FIELD_MOVE);
}

static void SyncBlob(u8 slotId)
{
    if (sSlots[slotId].avatarState == PLAYER_AVATAR_STATE_SURFING)
        CreateBlob(slotId);
    else
        DestroyBlob(slotId);
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

static u8 QueueLength(struct RemoteSlot *slot)
{
    return (slot->qHead + SLOT_QUEUE_LEN - slot->qTail) % SLOT_QUEUE_LEN;
}

// Gives the remote a movement action. The acro bike's side jump moves the
// player without turning them; the sender's event says which way they look
// and the action which way they go, so when the two differ the facing is
// locked for the action's duration, as it is on the sender.
static void StartMovement(struct ObjectEvent *object, u8 dir, u8 act, bool8 isStep)
{
    u8 family = act;

    if (act >= 0x10 && act < 0x64)
        family = act - (act <= 0x38 ? 1 : 2);
    object->facingDirectionLocked = isStep && (family & 3) != (u8)(dir - DIR_SOUTH);
    ObjectEventSetHeldMovement(object, act);
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
        slot->avatarState = event->avatarState < ARRAY_COUNT(sAnimCount) ? event->avatarState : PLAYER_AVATAR_STATE_NORMAL;

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
            // Nothing on screen: any positional event places them directly,
            // and is consumed by doing so.
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
            struct ObjectEvent *object;

            // The sprite follows the avatar state (bike, surf, dive).
            SyncGraphics(slotId);
            if (!SlotIsAlive(slotId))
            {
                slot->qTail = next;
                continue;
            }
            object = &gObjectEvents[slot->objectId];
            SyncBlob(slotId);

            if (kind == REMOTE_EVENT_STEP && !slot->forceResync)
            {
                s16 gapX = x - slot->targetX;
                s16 gapY = y - slot->targetY;
                s16 gap = Abs16(gapX) + Abs16(gapY);
                bool8 hurry = QueueLength(slot) >= HURRY_THRESHOLD;

                if (!ReadyForMovement(object))
                    return;

                if (gap > 1 && gap <= MEND_MAX && (gapX == 0 || gapY == 0))
                {
                    // Lost steps in a straight line: walk one tile and look
                    // at the same event again.
                    u8 mendDir;
                    u8 mendAct;

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
                    mendAct = ActionFor(slot->avatarState, mendDir, MOVEMENT_ACTION_WALK_FASTER_DOWN, TRUE, TRUE);
                    if (mendAct != 0xFF)
                        StartMovement(object, mendDir, mendAct, TRUE);
                    return;
                }
                if (gap > MEND_MAX)
                {
                    kind = REMOTE_EVENT_SYNC;   // not a step: reposition
                }
                else
                {
                    u8 act = ActionFor(slot->avatarState, event->dir, event->action, TRUE, hurry);

                    if (act != 0xFF)
                        StartMovement(object, event->dir, act, TRUE);
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
                u8 act = ActionFor(slot->avatarState, event->dir, event->action, FALSE, FALSE);

                if (!ReadyForMovement(object))
                    return;
                if (act != 0xFF)
                    StartMovement(object, event->dir, act, FALSE);
                slot->qTail = next;
                continue;
            }

            if (kind == REMOTE_EVENT_SYNC)
            {
                s16 curX = object->currentCoords.x;
                s16 curY = object->currentCoords.y;
                s16 drift = Abs16(x - curX) + Abs16(y - curY);

                // Standing still there is nothing to forgive: a remote that is
                // off its tile cannot be talked to.
                if (slot->forceResync || drift > (ObjectEventIsHeldMovementActive(object) ? DRIFT_TOLERANCE : 0))
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
        u8 kind = REMOTE_EVENT_KIND(sRxRing[sRxTail].type);

        if (REMOTE_EVENT_SLOT(sRxRing[sRxTail].type) < REMOTE_MAX_PLAYERS)
            sSlots[REMOTE_EVENT_SLOT(sRxRing[sRxTail].type)].silentFrames = 0;
        if (kind >= REMOTE_EVENT_BPARTY)
            RemoteBattle_HandleRx(&sRxRing[sRxTail]);
        else
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
        // Queued directly, not through the receive ring: that ring belongs
        // to the serial interrupt.
        QueueEvent(&event);
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

    RemoteSio_Tick();
    RemoteBattle_Tick();
    RemoteSocial_Tick();

#if REMOTE_PLAYER_LOOPBACK
    sFrameCounter++;
    if (!sRealPeerSeen)
        DrainLoopback();
#endif

    TrackPlayer();
    DrainRx();
    for (i = 0; i < REMOTE_MAX_PLAYERS; i++)
    {
        if (sSlots[i].spawnCooldown != 0)
            sSlots[i].spawnCooldown--;
        ConsumeSlotEvents(i);
        // Somebody who has said nothing for a while is gone (or in a place
        // the link cannot reach); a frozen sprite helps nobody. The next
        // event from them places them again.
        if (sSlots[i].silentFrames < SILENT_DESPAWN_FRAMES)
        {
            sSlots[i].silentFrames++;
        }
        else if (sSlots[i].known)
        {
            DespawnSlot(i);
            sSlots[i].known = FALSE;
            sSlots[i].forceResync = TRUE;
            sSlots[i].qHead = sSlots[i].qTail = 0;
        }
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
