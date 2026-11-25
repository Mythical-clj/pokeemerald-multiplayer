# Emulator bridge (mGBA desktop and Android)

The ROM exposes an EWRAM mailbox when it detects mGBA. `owl_bridge.c` finds it
(by its `OMB2` magic), sends the game's events to the relay and delivers
incoming ones. It speaks the relay's UDP OWL1 datagrams, so an emulator joins
the same room as real GBAs (Celio adapter + `client.py`) and browsers.

Status: written, not compiled, not run.

## Use
1. Put `owl.txt` next to the ROM / in the app's data folder (RetroArch: its saves folder):
       relay=wss://gbcatrade.wired-ariel.it/ws
       room=4827
       peer=2          (a number no one else in the room uses)
   For a raw UDP relay use `host=` and `port=` instead of `relay=`.
   The bridge writes `owl_status.txt` beside it: network state, whether the
   game's mailbox was found, and event counts. Look there first.
2. Android (Thor): the `Android mGBA core` workflow builds a patched libretro core
   for RetroArch (`mgba_libretro_android_arm64`). Put `owl.txt` in RetroArch's
   saves folder. Other builds: in mGBA's source: call `owl_bridge_attach(core, dir)` after loading the ROM
   and `owl_bridge_frame(core)` once per frame (a `videoFrameEnded` core
   callback is enough). Add `owl_bridge.c` to the build.
3. Everyone in a room must run the same ROM build (compare `build-sha1.txt`).

## Limits
- Certificates are checked but a bad one is not fatal (game traffic only).
- No ping, spectator, club or chat datagrams; only game events.
- No loss repair: the real client's gap stitching (client.py) is not ported.
