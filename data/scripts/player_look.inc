@ Bedroom PC: change the player's look (skin tone, outfit colour).

PlayerLook_EventScript_Appearance::
	message PlayerLook_Text_WhatToChange
	waitmessage
	multichoice 0, 0, MULTI_PLAYER_LOOK_TOP, FALSE
	switch VAR_RESULT
	case 0, PlayerLook_EventScript_ChooseSkin
	case 1, PlayerLook_EventScript_ChooseOutfit
	goto PlayerLook_EventScript_Back
	end

PlayerLook_EventScript_ChooseSkin::
	message PlayerLook_Text_PickSkin
	waitmessage
	multichoice 0, 0, MULTI_PLAYER_LOOK_SKIN, FALSE
	goto_if_eq VAR_RESULT, MULTI_B_PRESSED, PlayerLook_EventScript_Appearance
	copyvar VAR_0x8004, VAR_RESULT
	special PlayerLook_SetSkinFromVar
	goto PlayerLook_EventScript_Appearance
	end

PlayerLook_EventScript_ChooseOutfit::
	message PlayerLook_Text_PickOutfit
	waitmessage
	multichoice 0, 0, MULTI_PLAYER_LOOK_OUTFIT, FALSE
	goto_if_eq VAR_RESULT, MULTI_B_PRESSED, PlayerLook_EventScript_Appearance
	copyvar VAR_0x8004, VAR_RESULT
	special PlayerLook_SetOutfitFromVar
	goto PlayerLook_EventScript_Appearance
	end

PlayerLook_EventScript_Back::
	closemessage
	special BedroomPC
	releaseall
	end

PlayerLook_Text_WhatToChange:
	.string "What would you like to change?$"

PlayerLook_Text_PickSkin:
	.string "Pick a skin tone.$"

PlayerLook_Text_PickOutfit:
	.string "Pick an outfit color.$"
