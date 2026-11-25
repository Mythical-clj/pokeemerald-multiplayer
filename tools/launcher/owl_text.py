"""English text and plain-language diagnosis for the sync client's output.

The client (gen3-poke-multiplayer-overworld-sync, net/client.py) logs in
Italian. `translate()` turns the lines that matter into English; anything it
does not know is passed through marked "(untranslated)". `Health` tracks four
simple checks from the output so the window can say what is wrong.
"""
import re

# (pattern, English). Patterns are searched, not anchored; \1 etc. are filled in.
RULES = [
    # --- adapter / USB ---
    (r"nessun device ([0-9a-f:]+)",
     r"ADAPTER NOT FOUND (USB \1). Is the GB-Link plugged in? Does it have the patched "
     r"celio.uf2 firmware? On Windows the adapter also needs the WinUSB driver (Zadig button)."),
    (r"pyusb senza backend libusb",
     r"USB library (libusb) is missing from this program. Re-download EmeraldLink.exe."),
    (r"il comando '(.*?)' non e' partito",
     r"ADAPTER STUCK: command '\1' failed. Unplug the GB-Link, plug it back in and press Start again. "
     r"(Do not power off the GBA.)"),
    (r"sparito e ricomparso: riavvio avvenuto",
     r"Adapter restarted and came back: OK."),
    (r"il Pico NON e' mai sparito",
     r"The adapter did not restart: its firmware has no software-restart. Reflash celio.uf2 "
     r"or unplug and replug it."),
    (r"sparito ma MAI ricomparso",
     r"The adapter restarted but did not come back within 10 s: unplug and replug it."),
    (r"riavvio del Pico",
     r"Restarting the adapter..."),
    (r"transport USB \(Celio\), relay (\S+), peer (\d+), stanza (\d+)",
     r"Started: adapter mode, relay \1, player \2, room \3"),
    (r"GBA collegato via Celio",
     r"GBA connected through the adapter."),
    # --- relay ---
    (r"\[ws \] collegato al relay (\S+)",
     r"RELAY CONNECTED: \1"),
    (r"\[ws \] collegamento a (\S+) fallito: (.*)",
     r"RELAY CONNECTION FAILED (\1): \2. Check the PC's internet and the relay address."),
    (r"\[ws \] riagganciato al relay \((\d+)",
     r"Relay reconnected (attempt \1)."),
    (r"\[ws \] (?:canale chiuso dal server|il relay ha chiuso il canale)",
     r"Relay closed the connection; reconnecting..."),
    (r"\[ws \] (?:invio fallito|errore di lettura|lettura fallita)",
     r"Relay connection hiccup; reconnecting..."),
    (r"peer-id (\d+) - DEVE essere diverso",
     r"You are player \1. Every player in the room must use a DIFFERENT number "
     r"or you will kick each other out."),
    (r"il peer (\d+) e' USATO da un altro (\S+) nella stanza (\d+): passo al peer (\d+)",
     r"PLAYER NUMBER \1 IS ALREADY TAKEN in room \3. Switched to player \4. "
     r"Set different numbers on the two PCs."),
    (r"il relay dice che il peer (\d+) e' gia' in uso nella stanza (\d+)",
     r"Relay says player number \1 is already in use in room \2 (may clear in a few seconds "
     r"if you are reconnecting; otherwise pick another number)."),
    (r"invio al relay fallito",
     r"Could not send to the relay (will retry)."),
    # --- other players ---
    (r"amico (\d+) -> avatar slot (\d+)",
     r"PLAYER \1 JOINED: showing their sprite (slot \2)."),
    (r"amico (\d+) SENZA avatar",
     r"Player \1 joined but 3 other players are already on screen."),
    (r"peer (\d+) se n'e' andato \(era su mappa (\d+)\.(\d+)\)",
     r"Player \1 left (was on map \2.\3)."),
    (r"peer (\d+) se n'e' andato \(mai visto",
     r"Player \1 left (never saw them move)."),
    (r"amico (\d+) VA E VIENE",
     r"Player \1 keeps disconnecting and reconnecting (usually their adapter or internet)."),
    (r"\[stato \] io -> (\S+) \(dal GBA",
     r"Your GBA reported: \1 (your game is talking to the adapter)."),
    (r"\[stato \] amico (\d+) -> (\S+)",
     r"Player \1 status: \2"),
    # --- the GBA side ---
    (r"GBA MUTO da (\d+) s",
     r"YOUR GBA IS SILENT for \1 s. Stand in the overworld (not a menu, house or battle), "
     r"make sure the new ROM is running, and check the cable is firmly in the GBA's link port."),
    (r"nulla dal GBA verso il relay",
     r"Nothing is coming from your GBA. Stand in the overworld and walk around."),
    (r"CANALE CON IL GBA FERMO",
     r"ADAPTER STALLED: no new data from the GBA. Unplug and replug the GB-Link, press Start "
     r"again. Do not power off the GBA."),
    (r"DIREZIONE GBA->PC ROTTA",
     r"The PC is sending to the GBA but the GBA never answers. Check the cable, that you are "
     r"in the overworld, and that this is the new ROM build."),
    (r"il TUO GBA ha chiuso la porta seriale",
     r"Your GBA has closed its link port because you are not in the overworld "
     r"(menu, house, transition). Go back outside."),
    (r"il GBA non ha ancora detto su che mappa",
     r"The GBA has not reported a map yet: walk around in the overworld."),
    (r"nessun evento\. Che cosa",
     r"No events from the GBA."),
    # --- housekeeping ---
    (r"interrotto", r"Stopped."),
    (r"chiusura", r"Closing."),
    (r"la porta TCP (\d+) e' GIA' IN ASCOLTO",
     r"Another copy of the client is already running. Close it and press Start again."),
    (r"inviati (\d+) \| ricevuti (\d+)",
     r"Relay traffic: sent \1, received \2."),
]
_COMPILED = [(re.compile(p), r) for p, r in RULES]

# Lines that are statistics or noise: shown as-is would be Italian, so drop them.
_DROP = re.compile(r"^\s*(\[squadra\]|\[chat \]|\[mappa\]|\[club \]|\[usb\])")


def translate(line):
    """English version of one output line (or "" to hide it)."""
    text = line.rstrip("\n")
    if not text.strip():
        return ""
    for rx, repl in _COMPILED:
        m = rx.search(text)
        if m:
            return m.expand(repl) + "\n"
    if _DROP.match(text):
        return ""
    if re.search(r"[A-Za-z]", text) and re.search(
            r"\b(il|la|che|non|per|gia|sei|nel|del|della|gli|dal|con)\b", text):
        return "(untranslated) " + text + "\n"
    return text + "\n"


class Health:
    """Four yes/no checks, updated from raw client lines."""

    def __init__(self):
        self.adapter = None      # True found, False missing/stuck
        self.relay = None
        self.gba = None
        self.players = set()

    def feed(self, line):
        if "nessun device" in line or "pyusb senza backend" in line or "CANALE CON IL GBA FERMO" in line:
            self.adapter = False
        elif "transport USB" in line or "GBA collegato via Celio" in line:
            self.adapter = True
        if "collegato al relay" in line and "fallito" not in line:
            self.relay = True
        elif "collegamento a" in line and "fallito" in line:
            self.relay = False
        if "GBA MUTO" in line or "nulla dal GBA" in line or "DIREZIONE GBA->PC ROTTA" in line \
                or "ha chiuso la porta seriale" in line:
            self.gba = False
        elif "[stato ] io ->" in line:
            self.gba = True
        m = re.search(r"amico (\d+) -> avatar slot", line)
        if m:
            self.players.add(m.group(1))
        m = re.search(r"peer (\d+) se n'e' andato", line)
        if m:
            self.players.discard(m.group(1))

    def summary(self):
        def mark(v, yes, no, unknown):
            return yes if v else (no if v is False else unknown)
        return [
            "Adapter: " + mark(self.adapter, "found", "NOT FOUND / STUCK", "checking..."),
            "Relay:   " + mark(self.relay, "connected", "NOT CONNECTED", "connecting..."),
            "Your GBA: " + mark(self.gba, "talking", "SILENT (go to the overworld)", "waiting for it..."),
            "Other players: " + (", ".join(sorted(self.players)) if self.players else "none yet"),
        ]
