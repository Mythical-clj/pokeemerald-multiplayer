# Emulator bridge (mGBA desktop and Android)

The ROM exposes an EWRAM mailbox when it detects mGBA. `owl_bridge.c` finds it
(by its `OMB2` magic), sends the game's events to the relay and delivers
incoming ones. It speaks the relay's UDP OWL1 datagrams, so an emulator joins
the same room as real GBAs (Celio adapter + `client.py`) and browsers.

Status: written, not compiled, not run.

## Use
1. Put `owl.txt` next to the ROM / in the app's data folder:
       host=your.relay.example
       port=9000
       room=1
       peer=2          (a number no one else in the room uses)
2. In mGBA's source: call `owl_bridge_attach(core, dir)` after loading the ROM
   and `owl_bridge_frame(core)` once per frame (a `videoFrameEnded` core
   callback is enough). Add `owl_bridge.c` to the build.
3. Everyone in a room must run the same ROM build (compare `build-sha1.txt`).

## Limits
- UDP only. A relay reachable only over wss:443 needs a WebSocket/TLS client
  added here (the sync project notes UDP 9000 may be blocked on its VPS).
- No ping, spectator, club or chat datagrams; only game events.
- No loss repair: the real client's gap stitching (client.py) is not ported.
