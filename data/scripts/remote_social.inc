@ Talking to a remote player. Request kinds: 1 = battle, 2 = trade.

RemoteSocial_EventScript_Interact::
	lockall
	special RemoteSocial_Prepare
	goto_if_eq VAR_RESULT, 2, RemoteSocial_EventScript_Busy
	goto_if_eq VAR_RESULT, 3, RemoteSocial_EventScript_NotReady
	message RemoteSocial_Text_WhatToDo
	waitmessage
	multichoice 0, 0, MULTI_REMOTE_SOCIAL, FALSE
	switch VAR_RESULT
	case 0, RemoteSocial_EventScript_AskBattle
	case 1, RemoteSocial_EventScript_Mail
	goto RemoteSocial_EventScript_Done
	end

RemoteSocial_EventScript_Done::
	closemessage
	releaseall
	end

RemoteSocial_EventScript_Busy::
	msgbox RemoteSocial_Text_Busy, MSGBOX_DEFAULT
	goto RemoteSocial_EventScript_Done
	end

RemoteSocial_EventScript_NotReady::
	msgbox RemoteSocial_Text_NotReady, MSGBOX_DEFAULT
	goto RemoteSocial_EventScript_Done
	end

RemoteSocial_EventScript_AskBattle::
	setvar VAR_0x8004, 1
	goto RemoteSocial_EventScript_Ask
	end

RemoteSocial_EventScript_AskMix::
	setvar VAR_0x8004, 3
	goto RemoteSocial_EventScript_Ask
	end

RemoteSocial_EventScript_AskTrade::
	setvar VAR_0x8004, 2
	goto RemoteSocial_EventScript_Ask
	end

@ VAR_0x8004 = what to ask for
RemoteSocial_EventScript_Ask::
	special RemoteSocial_SendRequest
	message RemoteSocial_Text_Waiting
	waitmessage
	special RemoteSocial_WaitReply
	closemessage
	goto_if_eq VAR_RESULT, 4, RemoteSocial_EventScript_Done
	goto_if_eq VAR_RESULT, 2, RemoteSocial_EventScript_Declined
	goto_if_eq VAR_RESULT, 3, RemoteSocial_EventScript_NoAnswer
	goto RemoteSocial_EventScript_Agreed
	end

RemoteSocial_EventScript_Declined::
	msgbox RemoteSocial_Text_Declined, MSGBOX_DEFAULT
	goto RemoteSocial_EventScript_Done
	end

RemoteSocial_EventScript_NoAnswer::
	msgbox RemoteSocial_Text_NoAnswer, MSGBOX_DEFAULT
	goto RemoteSocial_EventScript_Done
	end

@ Both players said yes. VAR_0x8004 may have been changed, so ask the module.
RemoteSocial_EventScript_Agreed::
	special RemoteSocial_GetAgreedKind
	goto_if_eq VAR_RESULT, 1, RemoteSocial_EventScript_BattleAgreed
	goto_if_eq VAR_RESULT, 3, RemoteSocial_EventScript_MixAgreed
	goto RemoteSocial_EventScript_TradeAgreed
	end

RemoteSocial_EventScript_MixAgreed::
	message RemoteSocial_Text_Mixing
	waitmessage
	special RemoteSocial_StartMix
	special RemoteSocial_WaitMix
	closemessage
	goto_if_eq VAR_RESULT, 0, RemoteSocial_EventScript_MixFailed
	special RemoteSocial_FinishMix
	goto_if_eq VAR_RESULT, FALSE, RemoteSocial_EventScript_MixFailed
	playfanfare MUS_OBTAIN_ITEM
	msgbox RemoteSocial_Text_MixDone, MSGBOX_DEFAULT
	waitfanfare
	goto RemoteSocial_EventScript_Done
	end

RemoteSocial_EventScript_MixFailed::
	msgbox RemoteSocial_Text_MixFailed, MSGBOX_DEFAULT
	goto RemoteSocial_EventScript_Done
	end

RemoteSocial_EventScript_BattleAgreed::
	msgbox RemoteSocial_Text_BattleStarting, MSGBOX_DEFAULT
	closemessage
	special RemoteSocial_StartBattle
	waitstate
	releaseall
	end

RemoteSocial_EventScript_TradeAgreed::
	msgbox RemoteSocial_Text_ChooseMon, MSGBOX_DEFAULT
	closemessage
	special ChoosePartyMon
	waitstate
	special RemoteSocial_OfferMon
	goto_if_eq VAR_RESULT, FALSE, RemoteSocial_EventScript_TradeCancelled
	message RemoteSocial_Text_WaitingForMon
	waitmessage
	special RemoteSocial_WaitOffer
	closemessage
	goto_if_eq VAR_RESULT, 0, RemoteSocial_EventScript_TradeCancelled
	msgbox RemoteSocial_Text_Offer, MSGBOX_YESNO
	copyvar VAR_0x8005, VAR_RESULT
	special RemoteSocial_Confirm
	goto_if_eq VAR_0x8005, 0, RemoteSocial_EventScript_TradeCancelled
	message RemoteSocial_Text_WaitingToAccept
	waitmessage
	special RemoteSocial_WaitConfirm
	closemessage
	goto_if_eq VAR_RESULT, 0, RemoteSocial_EventScript_TradeCancelled
	special RemoteSocial_FinishTrade
	goto_if_eq VAR_RESULT, FALSE, RemoteSocial_EventScript_TradeCancelled
	playfanfare MUS_OBTAIN_ITEM
	msgbox RemoteSocial_Text_TradeDone, MSGBOX_DEFAULT
	waitfanfare
	goto RemoteSocial_EventScript_Done
	end

RemoteSocial_EventScript_TradeCancelled::
	msgbox RemoteSocial_Text_TradeCancelled, MSGBOX_DEFAULT
	goto RemoteSocial_EventScript_Done
	end

RemoteSocial_EventScript_Mail::
	fadescreen FADE_TO_BLACK
	special RemoteSocial_ComposeChat
	fadescreen FADE_FROM_BLACK
	special RemoteSocial_SendChat
	goto_if_eq VAR_RESULT, FALSE, RemoteSocial_EventScript_Done
	message RemoteSocial_Text_SendingMail
	waitmessage
	special RemoteSocial_WaitMail
	closemessage
	goto_if_eq VAR_RESULT, 1, RemoteSocial_EventScript_MailSent
	goto_if_eq VAR_RESULT, 2, RemoteSocial_EventScript_MailFull
	msgbox RemoteSocial_Text_MailFailed, MSGBOX_DEFAULT
	goto RemoteSocial_EventScript_Done
	end

RemoteSocial_EventScript_MailSent::
	msgbox RemoteSocial_Text_MailSent, MSGBOX_DEFAULT
	goto RemoteSocial_EventScript_Done
	end

RemoteSocial_EventScript_MailFull::
	msgbox RemoteSocial_Text_MailFull, MSGBOX_DEFAULT
	goto RemoteSocial_EventScript_Done
	end

@ The other player asked for something.
RemoteSocial_EventScript_Incoming::
	lockall
	special RemoteSocial_PrepareIncoming
	goto_if_eq VAR_0x8004, 1, RemoteSocial_EventScript_IncomingBattle
	goto_if_eq VAR_0x8004, 3, RemoteSocial_EventScript_IncomingMix
	goto RemoteSocial_EventScript_IncomingTrade
	end

RemoteSocial_EventScript_IncomingBattle::
	msgbox RemoteSocial_Text_WantsToBattle, MSGBOX_YESNO
	copyvar VAR_0x8005, VAR_RESULT
	special RemoteSocial_Answer
	goto_if_eq VAR_0x8005, 1, RemoteSocial_EventScript_BattleAgreed
	goto RemoteSocial_EventScript_Done
	end

RemoteSocial_EventScript_IncomingMix::
	msgbox RemoteSocial_Text_WantsToMix, MSGBOX_YESNO
	copyvar VAR_0x8005, VAR_RESULT
	special RemoteSocial_Answer
	goto_if_eq VAR_0x8005, 1, RemoteSocial_EventScript_MixAgreed
	goto RemoteSocial_EventScript_Done
	end

RemoteSocial_EventScript_IncomingTrade::
	msgbox RemoteSocial_Text_WantsToTrade, MSGBOX_YESNO
	copyvar VAR_0x8005, VAR_RESULT
	special RemoteSocial_Answer
	goto_if_eq VAR_0x8005, 1, RemoteSocial_EventScript_TradeAgreed
	goto RemoteSocial_EventScript_Done
	end


RemoteSocial_Text_SendingMail:
	.string "Sending the mail to {STR_VAR_1}…\n"
	.string "Press B to stop waiting.$"

RemoteSocial_Text_MailSent:
	.string "The mail is in {STR_VAR_1}'s\n"
	.string "PC mailbox!$"

RemoteSocial_Text_MailFull:
	.string "{STR_VAR_1}'s mailbox is full.$"

RemoteSocial_Text_MailFailed:
	.string "The mail could not be delivered.$"

RemoteSocial_Text_WhatToDo:
	.string "What would you like to do with\n"
	.string "{STR_VAR_1}?$"

RemoteSocial_Text_Busy:
	.string "You are still busy with\n"
	.string "something else.$"

RemoteSocial_Text_NotReady:
	.string "{STR_VAR_1} has not shown up\n"
	.string "properly yet. Try again.$"

RemoteSocial_Text_Waiting:
	.string "Waiting for {STR_VAR_1}'s answer…\n"
	.string "Press B to give up.$"

RemoteSocial_Text_Declined:
	.string "{STR_VAR_1} said no.$"

RemoteSocial_Text_NoAnswer:
	.string "{STR_VAR_1} did not answer.$"

RemoteSocial_Text_BattleStarting:
	.string "A battle with {STR_VAR_1}!\n"
	.string "Get ready!$"

RemoteSocial_Text_ChooseMon:
	.string "Choose the POKéMON to trade.$"

RemoteSocial_Text_WaitingForMon:
	.string "Waiting for {STR_VAR_1} to choose…\n"
	.string "Press B to cancel.$"

RemoteSocial_Text_Offer:
	.string "{STR_VAR_1} offers {STR_VAR_2}\n"
	.string "at Lv. {STR_VAR_3}. Trade for it?$"

RemoteSocial_Text_WaitingToAccept:
	.string "Waiting for {STR_VAR_1} to accept…$"

RemoteSocial_Text_TradeDone:
	.string "The trade is done!\n"
	.string "You received {STR_VAR_2}.$"

RemoteSocial_Text_TradeCancelled:
	.string "The trade was called off.$"

RemoteSocial_Text_WantsToBattle:
	.string "{STR_VAR_1} wants to battle!\n"
	.string "Accept?$"

RemoteSocial_Text_WantsToMix:
	.string "{STR_VAR_1} wants to mix records\n"
	.string "and share secret bases. Accept?$"

RemoteSocial_Text_Mixing:
	.string "Mixing records with {STR_VAR_1}…\n"
	.string "Press B to cancel.$"

RemoteSocial_Text_MixDone:
	.string "The records were mixed!$"

RemoteSocial_Text_MixFailed:
	.string "The records could not be mixed.$"

RemoteSocial_Text_WantsToTrade:
	.string "{STR_VAR_1} wants to trade\n"
	.string "POKéMON! Accept?$"
