#include "global.h"
#include "event_data.h"
#include "event_object_movement.h"
#include "field_player_avatar.h"
#include "field_weather.h"
#include "main.h"
#include "palette.h"
#include "sprite.h"
#include "player_look.h"
#include "constants/vars.h"
#include "constants/event_objects.h"

#define LOOK_VAR VAR_UNUSED_0x404E

#define OBJ_EVENT_PAL_TAG_BRENDAN           0x1100
#define OBJ_EVENT_PAL_TAG_MAY               0x1110
#define OBJ_EVENT_PAL_TAG_PLAYER_UNDERWATER 0x1115

// 5-bit colour from 8-bit channels
#define C8(r, g, b) (((r) >> 3) | (((g) >> 3) << 5) | (((b) >> 3) << 10))

// Highlight, middle and shadow of the skin: palette entries 1, 2 and 3.
static const u16 sSkinTones[PLAYER_LOOK_SKINS][3] =
{
    {0, 0, 0},   // default: left as it is
    {C8(232, 184, 140), C8(205, 148, 105), C8(172, 115, 82)},
    {C8(170, 115, 80), C8(140, 88, 58), C8(105, 62, 42)},
};

// Light to dark. Only the first two shades are used (palette entries 12-13,
// the red parts of both costumes).
static const u16 sOutfitRamps[PLAYER_LOOK_OUTFITS][4] =
{
    {0, 0, 0, 0},   // default (red): left as it is
    {C8(190, 225, 255), C8(140, 190, 235), C8(100, 150, 205), C8(70, 110, 170)},  // baby blue
    {C8(255, 165, 60), C8(230, 120, 30), C8(190, 85, 20), C8(140, 55, 15)},       // orange
    {C8(246, 246, 255), C8(222, 222, 235), C8(189, 189, 205), C8(148, 148, 165)}, // light grey
    {C8(82, 82, 90), C8(58, 58, 66), C8(41, 41, 49), C8(24, 24, 32)},             // dark grey
    {C8(255, 156, 206), C8(230, 115, 173), C8(189, 74, 132), C8(140, 41, 90)},    // pink
    {C8(172, 123, 213), C8(139, 90, 181), C8(106, 57, 148), C8(74, 33, 115)},     // purple
};

extern const u16 gObjectEventPal_Brendan[];
extern const u16 gObjectEventPal_May[];
extern const u16 gObjectEventPal_PlayerUnderwater[];
extern const u16 gObjectEventPal_BrendanReflection[];
extern const u16 gObjectEventPal_MayReflection[];

#define LOOK_PAL_TAG_BASE 0xB100
#define PARENT_PAL_TAG_BASE 0xB300      // Mom and Norman wearing the player's skin tone
#define OBJ_EVENT_PAL_TAG_NPC_4_LOCAL 0x1106
extern const u16 gObjectEventPal_Npc4[];

// Look palettes are allocated per look and the sprite palette slots are few,
// so drop the ones nothing is asking for any more; they are rebuilt on demand.
static void FreeLookPalettes(void)
{
    u8 kind, packed;

    for (packed = 1; packed < PLAYER_LOOK_SKINS; packed++)
        FreeSpritePaletteByTag(PARENT_PAL_TAG_BASE + packed);
    for (kind = 0; kind < 3; kind++)
    {
        for (packed = 1; packed < 32; packed++)
        {
            FreeSpritePaletteByTag(LOOK_PAL_TAG_BASE + kind * 32 + packed);
            FreeSpritePaletteByTag(LOOK_PAL_TAG_BASE + 0x100 + kind * 32 + packed);
        }
    }
}

// Frees the look palettes that no sprite is drawn with. Used when a new look
// finds the slots full: the palettes of the players on screen stay.
static void FreeIfUnused(u16 tag, u16 used)
{
    u8 idx = IndexOfSpritePaletteTag(tag);

    if (idx != 0xFF && !(used & (1 << idx)))
        FreeSpritePaletteByTag(tag);
}

static void FreeUnusedLookPalettes(void)
{
    u16 used = 0;
    u8 i, kind, packed;

    for (i = 0; i < MAX_SPRITES; i++)
    {
        if (gSprites[i].inUse)
            used |= 1 << gSprites[i].oam.paletteNum;
    }
    for (packed = 1; packed < PLAYER_LOOK_SKINS; packed++)
        FreeIfUnused(PARENT_PAL_TAG_BASE + packed, used);
    for (kind = 0; kind < 3; kind++)
    {
        for (packed = 1; packed < 32; packed++)
        {
            FreeIfUnused(LOOK_PAL_TAG_BASE + kind * 32 + packed, used);
            FreeIfUnused(LOOK_PAL_TAG_BASE + 0x100 + kind * 32 + packed, used);
        }
    }
}

// Loads a look palette. When the 16 slots are full the unused look palettes
// are dropped (at most twice a second); if that is not enough the caller
// falls back to the plain palette, so a crowd of different looks degrades to
// the default colours instead of flickering or wearing someone else's.
static EWRAM_DATA u32 sNextFreeAt = 0;

static u8 LoadLookPalette(const struct SpritePalette *sp)
{
    u8 idx = LoadSpritePalette(sp);

    if (idx == 0xFF && gMain.vblankCounter1 >= sNextFreeAt)
    {
        sNextFreeAt = gMain.vblankCounter1 + 30;
        FreeUnusedLookPalettes();
        idx = LoadSpritePalette(sp);
    }
    return idx;
}

u8 PlayerLook_Pack(void)
{
    u16 v = VarGet(LOOK_VAR);
    u8 skin = v & 3;
    u8 outfit = (v >> 2) & 7;

    if (skin >= PLAYER_LOOK_SKINS)
        skin = 0;
    if (outfit >= PLAYER_LOOK_OUTFITS)
        outfit = 0;
    return skin | (outfit << 2);
}

void PlayerLook_SetSkin(u8 skin)
{
    u8 p = PlayerLook_Pack();

    if (skin >= PLAYER_LOOK_SKINS)
        skin = 0;
    VarSet(LOOK_VAR, (p & ~3) | skin);
    FreeLookPalettes();
    PlayerLook_UpdateLocal();
}

void PlayerLook_SetOutfit(u8 outfit)
{
    u8 p = PlayerLook_Pack();

    if (outfit >= PLAYER_LOOK_OUTFITS)
        outfit = 0;
    VarSet(LOOK_VAR, (p & 3) | (outfit << 2));
    FreeLookPalettes();
    PlayerLook_UpdateLocal();
}

void PlayerLook_ApplyBuffer(u16 *pal, u8 female, u8 packed)
{
    u8 skin = packed & 3;
    u8 outfit = (packed >> 2) & 7;
    u8 i;

    if (skin > 0 && skin < PLAYER_LOOK_SKINS)
    {
        for (i = 0; i < 3; i++)
            pal[1 + i] = sSkinTones[skin][i];
    }
    if (outfit > 0 && outfit < PLAYER_LOOK_OUTFITS)
    {
        // The red parts of the costume (entries 12-13) take the colour; the
        // black parts and the green headband stay as they are.
        pal[12] = sOutfitRamps[outfit][0];
        pal[13] = sOutfitRamps[outfit][1];
    }
}

// Skin tone only (Norman's battle picture).
void PlayerLook_ApplySkinToLoaded(u16 bufferOffset)
{
    PlayerLook_ApplyToLoaded(bufferOffset, FALSE, PlayerLook_Pack() & 3);
}

void PlayerLook_ApplyToLoaded(u16 bufferOffset, u8 female, u8 packed)
{
    u16 tmp[16];
    u8 i;

    if (packed == 0)
        return;
    for (i = 0; i < 16; i++)
        tmp[i] = gPlttBufferUnfaded[bufferOffset + i];
    PlayerLook_ApplyBuffer(tmp, female, packed);
    for (i = 0; i < 16; i++)
    {
        gPlttBufferUnfaded[bufferOffset + i] = tmp[i];
        gPlttBufferFaded[bufferOffset + i] = tmp[i];
    }
}

// Which base palette an object uses: 0 boy, 1 girl, 2 underwater suit, 0xFF
// anything else (not a player sprite).
static u8 BaseKind(u16 paletteTag)
{
    if (paletteTag == OBJ_EVENT_PAL_TAG_BRENDAN)
        return 0;
    if (paletteTag == OBJ_EVENT_PAL_TAG_MAY)
        return 1;
    if (paletteTag == OBJ_EVENT_PAL_TAG_PLAYER_UNDERWATER)
        return 2;
    return 0xFF;
}

void PlayerLook_ApplyToObject(struct ObjectEvent *object, u8 packed)
{
    const struct ObjectEventGraphicsInfo *info;
    struct Sprite *sprite;
    u8 kind, idx;
    u16 tag;

    if (object == NULL || !object->active || object->spriteId >= MAX_SPRITES)
        return;
    info = GetObjectEventGraphicsInfo(object->graphicsId);
    kind = BaseKind(info->paletteTag);
    if (kind == 0xFF)
        return;
    sprite = &gSprites[object->spriteId];

    if (packed == 0)
    {
        // Back to the game's own palette, if one of ours was put on it.
        idx = IndexOfSpritePaletteTag(info->paletteTag);
        if (idx != 0xFF && sprite->oam.paletteNum != idx)
            sprite->oam.paletteNum = idx;
        return;
    }

    tag = LOOK_PAL_TAG_BASE + kind * 32 + packed;
    idx = IndexOfSpritePaletteTag(tag);
    if (idx == 0xFF)
    {
        u16 buf[16];
        struct SpritePalette sp;
        const u16 *base = kind == 0 ? gObjectEventPal_Brendan
                        : (kind == 1 ? gObjectEventPal_May : gObjectEventPal_PlayerUnderwater);
        u8 i;

        for (i = 0; i < 16; i++)
            buf[i] = base[i];
        PlayerLook_ApplyBuffer(buf, kind == 1, packed);
        sp.data = buf;
        sp.tag = tag;
        idx = LoadLookPalette(&sp);
        if (idx == 0xFF)
        {
            // No slot for this look: show the plain palette.
            idx = IndexOfSpritePaletteTag(info->paletteTag);
            if (idx != 0xFF && sprite->oam.paletteNum != idx)
                sprite->oam.paletteNum = idx;
            return;
        }
        UpdateSpritePaletteWithWeather(idx);
    }
    if (sprite->oam.paletteNum != idx)
        sprite->oam.paletteNum = idx;
}

// The player's parents have the player's skin tone: Mom and Norman in the
// overworld (the shared NPC palette is copied with the skin colours changed).
extern u8 RemotePlayer_HostSkin(void);

void PlayerLook_UpdateParents(void)
{
    u8 skin = PlayerLook_Pack() & 3;
    u8 host = RemotePlayer_HostSkin();
    u8 i;

    if (host != 0xFF && host < PLAYER_LOOK_SKINS)
        skin = host;

    for (i = 0; i < OBJECT_EVENTS_COUNT; i++)
    {
        struct ObjectEvent *object = &gObjectEvents[i];
        struct Sprite *sprite;
        u8 idx;

        if (!object->active || object->isPlayer || object->spriteId >= MAX_SPRITES
         || (object->graphicsId != OBJ_EVENT_GFX_MOM && object->graphicsId != OBJ_EVENT_GFX_NORMAN))
            continue;
        sprite = &gSprites[object->spriteId];

        if (skin == 0)
        {
            idx = IndexOfSpritePaletteTag(OBJ_EVENT_PAL_TAG_NPC_4_LOCAL);
            if (idx != 0xFF && sprite->oam.paletteNum != idx)
                sprite->oam.paletteNum = idx;
            continue;
        }

        idx = IndexOfSpritePaletteTag(PARENT_PAL_TAG_BASE + skin);
        if (idx == 0xFF)
        {
            u16 buf[16];
            struct SpritePalette sp;
            u8 j;

            for (j = 0; j < 16; j++)
                buf[j] = gObjectEventPal_Npc4[j];
            PlayerLook_ApplyBuffer(buf, FALSE, skin);
            sp.data = buf;
            sp.tag = PARENT_PAL_TAG_BASE + skin;
            idx = LoadLookPalette(&sp);
            if (idx == 0xFF)
                continue;
            UpdateSpritePaletteWithWeather(idx);
        }
        if (sprite->oam.paletteNum != idx)
            sprite->oam.paletteNum = idx;
    }
}

void PlayerLook_UpdateLocal(void)
{
    if (gPlayerAvatar.objectEventId >= OBJECT_EVENTS_COUNT)
        return;
    PlayerLook_ApplyToObject(&gObjectEvents[gPlayerAvatar.objectEventId], PlayerLook_Pack());
}

// Specials for the appearance menu: the chosen entry arrives in VAR_0x8004.
void PlayerLook_SetSkinFromVar(void)
{
    PlayerLook_SetSkin(gSpecialVar_0x8004);
}

void PlayerLook_SetOutfitFromVar(void)
{
    PlayerLook_SetOutfit(gSpecialVar_0x8004);
}

// ---- Water reflections ------------------------------------------------------

// A reflection is the same sprite in washed-out colours: the look's colours
// are blended toward the water instead of used as they are.
static u16 Reflect(u16 c, u16 water, u8 waterParts)
{
    u16 r = c & 0x1F, g = (c >> 5) & 0x1F, b = (c >> 10) & 0x1F;
    u16 wr = water & 0x1F, wg = (water >> 5) & 0x1F, wb = (water >> 10) & 0x1F;

    r = (r * (4 - waterParts) + wr * waterParts) / 4;
    g = (g * (4 - waterParts) + wg * waterParts) / 4;
    b = (b * (4 - waterParts) + wb * waterParts) / 4;
    return r | (g << 5) | (b << 10);
}

extern u8 RemotePlayer_LookOfObject(u8 objectId);

void PlayerLook_ApplyToReflection(struct ObjectEvent *object, struct Sprite *reflection)
{
    const struct ObjectEventGraphicsInfo *info = GetObjectEventGraphicsInfo(object->graphicsId);
    u8 kind = BaseKind(info->paletteTag);
    u8 packed, idx, i;
    u16 tag;

    if (kind > 1)
        return;
    if (object->isPlayer)
        packed = PlayerLook_Pack();
    else
        packed = RemotePlayer_LookOfObject(object - gObjectEvents);
    if (packed == 0)
        return;

    tag = LOOK_PAL_TAG_BASE + 0x100 + kind * 32 + packed;
    idx = IndexOfSpritePaletteTag(tag);
    if (idx == 0xFF)
    {
        u16 buf[16];
        struct SpritePalette sp;
        const u16 *base = kind == 0 ? gObjectEventPal_BrendanReflection : gObjectEventPal_MayReflection;
        u16 look[16];

        for (i = 0; i < 16; i++)
        {
            buf[i] = base[i];
            look[i] = 0;
        }
        PlayerLook_ApplyBuffer(look, kind == 1, packed);
        if ((packed & 3) != 0)
        {
            for (i = 1; i < 4; i++)
                buf[i] = Reflect(look[i], C8(255, 235, 235), 1);
        }
        if (((packed >> 2) & 7) != 0)
        {
            buf[12] = Reflect(look[12], C8(170, 175, 205), 2);
            buf[13] = Reflect(look[13], C8(170, 175, 205), 2);
        }
        sp.data = buf;
        sp.tag = tag;
        idx = LoadLookPalette(&sp);
        if (idx == 0xFF)
            return;
        UpdateSpritePaletteWithWeather(idx);
    }
    reflection->oam.paletteNum = idx;
}
