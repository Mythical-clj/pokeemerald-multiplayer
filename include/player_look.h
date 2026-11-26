#ifndef GUARD_PLAYER_LOOK_H
#define GUARD_PLAYER_LOOK_H

// The player's chosen look: skin tone (0 default, 1 light brown, 2 dark
// brown) and outfit colour (0 default red, 1 baby blue, 2 orange, 3 light
// grey, 4 dark grey, 5 pink, 6 purple),
// kept in a saved game variable and packed into 5 bits for sending.
#define PLAYER_LOOK_SKINS    3
#define PLAYER_LOOK_OUTFITS  7

struct ObjectEvent;

u8 PlayerLook_Pack(void);          // skin | outfit << 2
void PlayerLook_SetSkin(u8 skin);
void PlayerLook_SetOutfit(u8 outfit);

// Recolours a 16-entry palette in RAM in place. female selects the layout.
void PlayerLook_ApplyBuffer(u16 *pal, u8 female, u8 packed);
// Recolours a palette that is already in the palette buffers (both copies).
void PlayerLook_ApplyToLoaded(u16 bufferOffset, u8 female, u8 packed);
// Gives an overworld player object the palette for the look (or restores the
// normal one for the default look).
void PlayerLook_ApplyToObject(struct ObjectEvent *object, u8 packed);
void PlayerLook_ApplySkinToLoaded(u16 bufferOffset);
// Per frame: Mom and Norman take the player's skin tone.
void PlayerLook_UpdateParents(void);
// Per frame: the local player.
void PlayerLook_UpdateLocal(void);
// The water reflection of a player object (called every frame for its
// reflection sprite).
struct Sprite;
void PlayerLook_ApplyToReflection(struct ObjectEvent *object, struct Sprite *reflection);

#endif // GUARD_PLAYER_LOOK_H
