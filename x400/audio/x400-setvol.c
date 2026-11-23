/*
 * x400-setvol: give Android's audio policy the volume range and index that the framework (AudioService, absent while
 * OneOS has stopped One UI) normally sets. Without them every volume curve reports "invalid volume index range in the
 * curve", the track gain is -inf and the speaker is silent although AAudio, AudioFlinger and the amplifier all run.
 *
 *   x400-setvol [index] [max]        default index 5, max 15, STREAM_MUSIC (3), device SPEAKER
 *   x400-setvol --get                print the current index
 *
 * libaudiobase.so exports android::AudioSystem::initStreamVolume / setStreamVolumeIndex (C++ symbols, not in the NDK):
 * resolved with dlsym. They take the audio_devices_t of the speaker (0x2). All of them end in a binder call to
 * media.audio_policy; nothing is written to the hardware here.
 * Build: x400/audio/build-android.sh
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int (*init_fn)(int stream, int min, int max);
typedef int (*set_fn)(int stream, int index, int muted, unsigned device);
typedef int (*get_fn)(int stream, int *index, unsigned device);

#define STREAM_MUSIC 3
#define DEVICE_OUT_SPEAKER 0x2

int main(int argc, char **argv)
{
	void *h = dlopen("libaudiobase.so", RTLD_NOW);
	if (!h) { fprintf(stderr, "dlopen libaudiobase.so: %s\n", dlerror()); return 2; }
	init_fn init = (init_fn)dlsym(h, "_ZN7android11AudioSystem16initStreamVolumeE19audio_stream_type_tii");
	set_fn set = (set_fn)dlsym(h, "_ZN7android11AudioSystem20setStreamVolumeIndexE19audio_stream_type_tib15audio_devices_t");
	get_fn get = (get_fn)dlsym(h, "_ZN7android11AudioSystem20getStreamVolumeIndexE19audio_stream_type_tPij");
	if (!init || !set) { fprintf(stderr, "symbols not found (init=%p set=%p)\n", (void *)init, (void *)set); return 2; }
	if (argc > 1 && !strcmp(argv[1], "--get")) {
		int idx = -1;
		if (!get) { fprintf(stderr, "getStreamVolumeIndex not found\n"); return 2; }
		int r = get(STREAM_MUSIC, &idx, DEVICE_OUT_SPEAKER);
		printf("STREAM_MUSIC speaker index %d (status %d)\n", idx, r);
		return 0;
	}
	int index = argc > 1 ? atoi(argv[1]) : 5, max = argc > 2 ? atoi(argv[2]) : 15;
	int r1 = init(STREAM_MUSIC, 0, max);
	int r2 = set(STREAM_MUSIC, index, 0, DEVICE_OUT_SPEAKER);
	printf("initStreamVolume(MUSIC, 0, %d) = %d; setStreamVolumeIndex(MUSIC, %d, speaker) = %d\n", max, r1, index, r2);
	return (r1 || r2) ? 1 : 0;
}
