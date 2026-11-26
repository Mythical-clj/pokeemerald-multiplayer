#include "global.h"
#include "event_data.h"
#include "event_object_movement.h"
#include "field_player_avatar.h"
#include "field_weather.h"
#include "palette.h"
#include "sprite.h"
#include "player_look.h"
#include "constants/vars.h"

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

// Light to dark. The boy's clothes use all four (entries 5-8), the girl's
// only the first two (entries 12-13).
static const u16 sOutfitRamps[PLAYER_LOOK_OUTFITS][4] =
{
    {0, 0, 0, 0},   // default: left as it is
    {C8(255, 98, 90), C8(197, 65, 65), C8(140, 41, 49), C8(98, 24, 33)},       // red
    {C8(98, 123, 189), C8(74, 90, 156), C8(49, 65, 115), C8(24, 41, 82)},      // blue
    {C8(115, 205, 115), C8(74, 148, 82), C8(41, 106, 57), C8(24, 74, 41)},     // green
    {C8(172, 123, 213), C8(139, 90, 181), C8(106, 57, 148), C8(74, 33, 115)},  // purple
};

extern const u16 gObjectEventPal_Brendan[];
extern const u16 gObjectEventPal_May[];
extern const u16 gObjectEventPal_PlayerUnderwater[];

#define LOOK_PAL_TAG_BASE 0xB100

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
}

void PlayerLook_SetOutfit(u8 outfit)
{
    u8 p = PlayerLook_Pack();

    if (outfit >= PLAYER_LOOK_OUTFITS)
        outfit = 0;
    VarSet(LOOK_VAR, (p & 3) | (outfit << 2));
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
        if (female)
        {
            pal[12] = sOutfitRamps[outfit][0];
            pal[13] = sOutfitRamps[outfit][1];
        }
        else
        {
            for (i = 0; i < 4; i++)
                pal[5 + i] = sOutfitRamps[outfit][i];
        }
    }
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
        idx = LoadSpritePalette(&sp);
        if (idx == 0xFF)
            return;
        UpdateSpritePaletteWithWeather(idx);
    }
    if (sprite->oam.paletteNum != idx)
        sprite->oam.paletteNum = idx;
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
