#ifndef GUARD_REMOTE_SIO_H
#define GUARD_REMOTE_SIO_H

// Link transport for remote players.
//
// The adapter (Celio firmware in passthrough mode) is the Multi-Player SIO
// master and the GBA is a child. Every serial interrupt delivers one 16-bit
// word and loads the next one to send. Words are framed as
//   [0xA55A][type<<8 | wordCount][words...][checksum]
// where the checksum makes the 16-bit sum of header, data and checksum zero.
//
// The port belongs to the game's own link code whenever that is open
// (Cable Club, trading, link battles, wireless). This driver only holds it
// while the overworld is running and gives it back otherwise.

#ifndef REMOTE_PLAYER_SIO
#define REMOTE_PLAYER_SIO 1
#endif

// Detects an emulator and, if there is one, sets up the mailbox in place of
// the serial port. Call once at start-up.
void RemoteSio_Init(void);

// Called every frame from the overworld; takes the port if the game is not
// using it.
void RemoteSio_Tick(void);

// Called from VBlank: sends queued events and gives the port up when the
// overworld has stopped ticking.
void RemoteSio_VBlank(void);

// Called from the serial interrupt when the game has no serial callback.
void RemoteSio_OnSerialIrq(void);

// The game's own link opened or closed.
void RemoteSio_GameLinkOpened(void);
void RemoteSio_GameLinkClosed(void);

#endif // GUARD_REMOTE_SIO_H
