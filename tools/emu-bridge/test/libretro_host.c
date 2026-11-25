// Smallest possible libretro frontend: load a core and a ROM, run N frames.
// Used in CI to reproduce a crash of the patched core off-device.
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libretro.h"

static const char* gDir = ".";

static bool env(unsigned cmd, void* data) {
	switch (cmd) {
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
		return true;
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
		*(const char**) data = gDir;
		return true;
	case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
		*(unsigned*) data = 0;
		return true;
	default:
		return false;
	}
}
static void video(const void* d, unsigned w, unsigned h, size_t p) {}
static size_t audio(const int16_t* d, size_t n) { return n; }
static void audio1(int16_t l, int16_t r) {}
static void poll(void) {}
static int16_t input(unsigned a, unsigned b, unsigned c, unsigned d) { return 0; }

#define SYM(name) name##_t name = (name##_t) dlsym(h, #name); if (!name) { fprintf(stderr, "missing " #name "\n"); return 2; }
typedef void (*retro_set_environment_t)(retro_environment_t);
typedef void (*retro_set_video_refresh_t)(retro_video_refresh_t);
typedef void (*retro_set_audio_sample_t)(retro_audio_sample_t);
typedef void (*retro_set_audio_sample_batch_t)(retro_audio_sample_batch_t);
typedef void (*retro_set_input_poll_t)(retro_input_poll_t);
typedef void (*retro_set_input_state_t)(retro_input_state_t);
typedef void (*retro_init_t)(void);
typedef bool (*retro_load_game_t)(const struct retro_game_info*);
typedef void (*retro_run_t)(void);
typedef void (*retro_unload_game_t)(void);
typedef void (*retro_deinit_t)(void);

int main(int argc, char** argv) {
	void* h;
	FILE* f;
	struct retro_game_info g;
	void* rom;
	long n;
	int frames = argc > 4 ? atoi(argv[4]) : 300, i;

	if (argc < 4) {
		fprintf(stderr, "usage: host core.so rom.gba dir [frames]\n");
		return 2;
	}
	gDir = argv[3];
	h = dlopen(argv[1], RTLD_NOW);
	if (!h) {
		fprintf(stderr, "dlopen: %s\n", dlerror());
		return 2;
	}
	{
		SYM(retro_set_environment) SYM(retro_set_video_refresh) SYM(retro_set_audio_sample)
		SYM(retro_set_audio_sample_batch) SYM(retro_set_input_poll) SYM(retro_set_input_state)
		SYM(retro_init) SYM(retro_load_game) SYM(retro_run) SYM(retro_unload_game) SYM(retro_deinit)
		f = fopen(argv[2], "rb");
		fseek(f, 0, SEEK_END);
		n = ftell(f);
		fseek(f, 0, SEEK_SET);
		rom = malloc(n);
		fread(rom, 1, n, f);
		fclose(f);
		retro_set_environment(env);
		retro_set_video_refresh(video);
		retro_set_audio_sample(audio1);
		retro_set_audio_sample_batch(audio);
		retro_set_input_poll(poll);
		retro_set_input_state(input);
		retro_init();
		memset(&g, 0, sizeof(g));
		g.path = argv[2];
		g.data = rom;
		g.size = n;
		if (!retro_load_game(&g)) {
			fprintf(stderr, "retro_load_game failed\n");
			return 1;
		}
		fprintf(stderr, "loaded; running %d frames\n", frames);
		for (i = 0; i < frames; i++) {
			retro_run();
		}
		fprintf(stderr, "ran %d frames OK\n", i);
		retro_unload_game();
		retro_deinit();
	}
	return 0;
}
