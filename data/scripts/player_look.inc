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

@ Pokemon Center PC: outfit colour only (skin tone is a bedroom setting).
PlayerLook_EventScript_OutfitOnly::
	message PlayerLook_Text_PickOutfit
	waitmessage
	multichoice 0, 0, MULTI_PLAYER_LOOK_OUTFIT, FALSE
	goto_if_eq VAR_RESULT, MULTI_B_PRESSED, PlayerLook_EventScript_OutfitOnlyBack
	copyvar VAR_0x8004, VAR_RESULT
	special PlayerLook_SetOutfitFromVar
	goto PlayerLook_EventScript_OutfitOnly
	end

PlayerLook_EventScript_OutfitOnlyBack::
	closemessage
	special PlayerPC
	goto EventScript_PCMainMenu
	end

@ Phrases: the three battle phrases (start, won, lost), at any PC.
@ The "won" and "lost" phrases are what the other player sees when you
@ beat them or lose to them in a versus battle.
PlayerLook_EventScript_PhrasesBedroom::
	message PlayerLook_Text_WhichPhrase
	waitmessage
	multichoice 0, 0, MULTI_PLAYER_PHRASES, FALSE
	switch VAR_RESULT
	case 0, PlayerLook_EventScript_PhraseStartBedroom
	case 1, PlayerLook_EventScript_PhraseWonBedroom
	case 2, PlayerLook_EventScript_PhraseLostBedroom
	goto PlayerLook_EventScript_Back
	end

PlayerLook_EventScript_PhraseStartBedroom::
	setvar VAR_0x8004, EASY_CHAT_TYPE_BATTLE_START
	call Common_ShowEasyChatScreen
	goto PlayerLook_EventScript_PhrasesBedroom
	end

PlayerLook_EventScript_PhraseWonBedroom::
	setvar VAR_0x8004, EASY_CHAT_TYPE_BATTLE_WON
	call Common_ShowEasyChatScreen
	goto PlayerLook_EventScript_PhrasesBedroom
	end

PlayerLook_EventScript_PhraseLostBedroom::
	setvar VAR_0x8004, EASY_CHAT_TYPE_BATTLE_LOST
	call Common_ShowEasyChatScreen
	goto PlayerLook_EventScript_PhrasesBedroom
	end

PlayerLook_EventScript_PhrasesPC::
	message PlayerLook_Text_WhichPhrase
	waitmessage
	multichoice 0, 0, MULTI_PLAYER_PHRASES, FALSE
	switch VAR_RESULT
	case 0, PlayerLook_EventScript_PhraseStartPC
	case 1, PlayerLook_EventScript_PhraseWonPC
	case 2, PlayerLook_EventScript_PhraseLostPC
	goto PlayerLook_EventScript_OutfitOnlyBack
	end

PlayerLook_EventScript_PhraseStartPC::
	setvar VAR_0x8004, EASY_CHAT_TYPE_BATTLE_START
	call Common_ShowEasyChatScreen
	goto PlayerLook_EventScript_PhrasesPC
	end

PlayerLook_EventScript_PhraseWonPC::
	setvar VAR_0x8004, EASY_CHAT_TYPE_BATTLE_WON
	call Common_ShowEasyChatScreen
	goto PlayerLook_EventScript_PhrasesPC
	end

PlayerLook_EventScript_PhraseLostPC::
	setvar VAR_0x8004, EASY_CHAT_TYPE_BATTLE_LOST
	call Common_ShowEasyChatScreen
	goto PlayerLook_EventScript_PhrasesPC
	end

PlayerLook_Text_WhichPhrase:
	.string "Which phrase would you like to set?$"

PlayerLook_Text_WhatToChange:
	.string "What would you like to change?$"

PlayerLook_Text_PickSkin:
	.string "Pick a skin tone.$"

PlayerLook_Text_PickOutfit:
	.string "Pick an outfit color.$"
