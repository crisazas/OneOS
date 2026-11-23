/*
 * x400-aaudio: AAudio probe for the Android host (bionic, NDK).
 *
 *   x400-aaudio tone <hz> <secs> <amp%>            play a sine on the speaker
 *   x400-aaudio rec  <secs> <out.raw> [src]        record s16le 48000 mono/stereo, print RMS/peak per 0.25 s
 *   x400-aaudio both <hz> <secs> <amp%> <out.raw>  record while a tone plays (acoustic loop: speaker -> mic)
 *
 * Build: x400/audio/build-android.sh
 */
#include <aaudio/AAudio.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RATE 48000

static volatile int stop_flag;

static AAudioStream *open_stream(int dir, int channels, int input_preset)
{
	AAudioStreamBuilder *b;
	AAudioStream *s = NULL;
	AAudio_createStreamBuilder(&b);
	AAudioStreamBuilder_setDirection(b, dir);
	AAudioStreamBuilder_setSampleRate(b, RATE);
	AAudioStreamBuilder_setChannelCount(b, channels);
	AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
	AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
	AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_NONE);
	if (dir == AAUDIO_DIRECTION_INPUT)
		AAudioStreamBuilder_setInputPreset(b, input_preset);
	else
		AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_MEDIA);
	aaudio_result_t r = AAudioStreamBuilder_openStream(b, &s);
	AAudioStreamBuilder_delete(b);
	if (r != AAUDIO_OK) {
		fprintf(stderr, "open %s stream: %s\n", dir == AAUDIO_DIRECTION_INPUT ? "input" : "output", AAudio_convertResultToText(r));
		return NULL;
	}
	fprintf(stderr, "%s: rate %d ch %d fmt %d burst %d buf %d dev %d\n", dir == AAUDIO_DIRECTION_INPUT ? "in" : "out",
		AAudioStream_getSampleRate(s), AAudioStream_getChannelCount(s), AAudioStream_getFormat(s),
		AAudioStream_getFramesPerBurst(s), AAudioStream_getBufferSizeInFrames(s), AAudioStream_getDeviceId(s));
	return s;
}

struct play_args { double hz, secs, amp; };
static int g_preset = AAUDIO_INPUT_PRESET_VOICE_RECOGNITION;

static void *play_thread(void *p)
{
	struct play_args *a = p;
	AAudioStream *s = open_stream(AAUDIO_DIRECTION_OUTPUT, 2, 0);
	if (!s) return NULL;
	int16_t buf[480 * 2];
	double ph = 0, st = 2 * M_PI * a->hz / RATE;
	long total = (long)(a->secs * RATE), done = 0;
	AAudioStream_requestStart(s);
	while (done < total && !stop_flag) {
		for (int i = 0; i < 480; i++, ph += st) {
			double fade = done + i < RATE / 20 ? (double)(done + i) / (RATE / 20) : 1.0;
			double w = getenv("X400_NOISE") ? (rand() / (double)RAND_MAX) * 2 - 1 : sin(ph);
			int16_t v = (int16_t)(w * a->amp * fade * 32767);
			buf[i * 2] = buf[i * 2 + 1] = v;
		}
		aaudio_result_t r = AAudioStream_write(s, buf, 480, 2000000000LL);
		if (r < 0) { fprintf(stderr, "write: %s\n", AAudio_convertResultToText(r)); break; }
		done += 480;
	}
	fprintf(stderr, "played %ld frames, xruns %d, state %s\n", done, AAudioStream_getXRunCount(s),
		AAudio_convertStreamStateToText(AAudioStream_getState(s)));
	AAudioStream_requestStop(s);
	AAudioStream_close(s);
	return NULL;
}

static int record(double secs, const char *out, int preset)
{
	AAudioStream *s = open_stream(AAUDIO_DIRECTION_INPUT, 1, preset);
	if (!s) return 3;
	FILE *f = fopen(out, "wb");
	int16_t buf[480];
	long total = (long)(secs * RATE), done = 0;
	double acc = 0, pk = 0, all = 0;
	long n = 0;
	AAudioStream_requestStart(s);
	while (done < total) {
		aaudio_result_t r = AAudioStream_read(s, buf, 480, 2000000000LL);
		if (r < 0) { fprintf(stderr, "read: %s\n", AAudio_convertResultToText(r)); break; }
		if (r == 0) continue;
		fwrite(buf, 2, r, f);
		for (int i = 0; i < r; i++) {
			double v = buf[i] / 32768.0;
			acc += v * v; all += v * v;
			if (fabs(v) > pk) pk = fabs(v);
		}
		done += r; n += r;
		if (n >= RATE / 4) { printf("t=%.2f rms=%.6f peak=%.4f\n", (double)done / RATE, sqrt(acc / n), pk); acc = 0; pk = 0; n = 0; }
	}
	printf("total rms=%.6f frames=%ld\n", done ? sqrt(all / done) : 0.0, done);
	AAudioStream_requestStop(s);
	AAudioStream_close(s);
	fclose(f);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc >= 5 && !strcmp(argv[1], "tone")) {
		struct play_args a = { atof(argv[2]), atof(argv[3]), atof(argv[4]) / 100.0 };
		play_thread(&a);
		return 0;
	}
	if (argc >= 4 && !strcmp(argv[1], "rec"))
		return record(atof(argv[2]), argv[3], argc > 4 ? atoi(argv[4]) : AAUDIO_INPUT_PRESET_VOICE_RECOGNITION);
	if (argc >= 6 && !strcmp(argv[1], "both")) {
		struct play_args a = { atof(argv[2]), atof(argv[3]), atof(argv[4]) / 100.0 };
		pthread_t t;
		pthread_create(&t, NULL, play_thread, &a);
		int rc = record(a.secs, argv[5], argc > 6 ? atoi(argv[6]) : g_preset);
		stop_flag = 1;
		pthread_join(t, NULL);
		return rc;
	}
	fprintf(stderr, "usage: see header of x400-aaudio.c\n");
	return 1;
}
