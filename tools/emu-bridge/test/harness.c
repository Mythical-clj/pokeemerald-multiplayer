#include <mgba/core/core.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "owl_bridge.h"

static uint8_t mem[0x40000];
#define ADDR(a) ((a) - 0x02000000u)
static uint32_t r32(struct mCore* c, uint32_t a) { uint32_t v; memcpy(&v, mem + ADDR(a), 4); return v; }
static uint32_t r8(struct mCore* c, uint32_t a) { return mem[ADDR(a)]; }
static void w32(struct mCore* c, uint32_t a, uint32_t v) { memcpy(mem + ADDR(a), &v, 4); }
static void w8(struct mCore* c, uint32_t a, uint8_t v) { mem[ADDR(a)] = v; }

int main(int argc, char** argv) {
	static struct mCore core;
	uint32_t box = 0x02001000;
	int marker = atoi(argv[2]), i, got = 0, ok = 0;
	core.busRead32 = r32; core.busRead8 = r8; core.busWrite32 = w32; core.busWrite8 = w8;
	w32(&core, box + 0, 0x324D424F); w32(&core, box + 0x0C, 16);
	owl_bridge_attach(&core, argv[1]);
	for (i = 0; i < 400; i++) { // ~8 s at 20 ms
		w32(&core, box + 0x1C, i + 1);
		if (i % 20 == 5) { // game sends an event: kind 12 (BSTART), marker byte
			uint32_t head = r32(&core, box + 4);
			uint8_t ev[12] = {12, marker, 0, 0, 3, 7, 0, 0, 0, 0, 0, 0};
			memcpy(mem + ADDR(box + 0x20 + head * 12), ev, 12);
			w32(&core, box + 4, (head + 1) % 16);
		}
		owl_bridge_frame(&core);
		{
			uint32_t tail = r32(&core, box + 0x14), head = r32(&core, box + 0x10);
			while (tail != head) {
				uint8_t* e = mem + ADDR(box + 0x20 + 16 * 12 + tail * 12);
				got++;
				if ((e[0] & 0x0F) == 12 && e[1] != marker) { ok = 1; if (got < 3) printf("rx: kind %d slot %d marker %d map %d.%d\n", e[0] & 15, e[0] >> 4, e[1], e[4], e[5]); }
				tail = (tail + 1) % 16;
				w32(&core, box + 0x14, tail);
			}
		}
		usleep(20000);
	}
	owl_bridge_detach();
	printf("peer marker %d: received %d events, saw other player's: %s\n", marker, got, ok ? "YES" : "NO");
	return ok ? 0 : 1;
}
