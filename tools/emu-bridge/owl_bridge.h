#ifndef OWL_BRIDGE_H
#define OWL_BRIDGE_H

struct mCore;

// Starts the bridge for this core. Reads "owl.txt" (see README.md) from the
// given directory. Safe to call again after loading a new ROM.
void owl_bridge_attach(struct mCore *core, const char *configDir);

// Call once per emulated frame, on the emulation thread.
void owl_bridge_frame(struct mCore *core);

void owl_bridge_detach(void);

#endif
