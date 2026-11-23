/*
 * x400-audiod: Android-side audio bridge (bionic, NDK, AAudio through Android's audioserver).
 *
 *   x400-audiod [--dir DIR] [--out-test] [--log FILE]
 *
 * Two unix stream sockets in DIR (default /data/local/tmp/oneos/rootfs/tmp, which the guest sees as /tmp):
 *   x400-audio-out.sock   guest -> speaker : raw PCM s16le 48000 Hz stereo, no header
 *   x400-audio-in.sock    microphone -> guest: raw PCM s16le 48000 Hz stereo (the mono mic is duplicated)
 * One client per socket. The AAudio stream is opened when a client connects and closed when it goes away,
 * so the speaker amplifier / microphone are not kept powered while nobody uses them.
 * Every second per connection it logs bytes, frames and RMS/peak (the Android-side evidence).
 * X400_MAXAMP (percent of full scale, default 100) is a hard ceiling: samples are clipped to it, never scaled.
 */
#include <aaudio/AAudio.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define RATE 48000
#define FRAMES 480

static FILE *dlogf;
static double max_amp = 1.0;
static pthread_mutex_t log_mu = PTHREAD_MUTEX_INITIALIZER;

static void dlog(const char *fmt, ...)
{
	char ts[32];
	time_t t = time(NULL);
	strftime(ts, sizeof ts, "%H:%M:%S", localtime(&t));
	pthread_mutex_lock(&log_mu);
	fprintf(dlogf, "%s ", ts);
	va_list ap;
	va_start(ap, fmt);
	vfprintf(dlogf, fmt, ap);
	va_end(ap);
	fputc('\n', dlogf);
	fflush(dlogf);
	pthread_mutex_unlock(&log_mu);
}

static int listen_unix(const char *path)
{
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	strncpy(a.sun_path, path, sizeof a.sun_path - 1);
	unlink(path);
	int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof a) < 0 || listen(s, 1) < 0) {
		dlog("listen %s: %s", path, strerror(errno));
		exit(1);
	}
	chmod(path, 0777);
	return s;
}

static AAudioStream *open_stream(int dir, int ch)
{
	AAudioStreamBuilder *b;
	AAudioStream *s = NULL;
	AAudio_createStreamBuilder(&b);
	AAudioStreamBuilder_setDirection(b, dir);
	AAudioStreamBuilder_setSampleRate(b, RATE);
	AAudioStreamBuilder_setChannelCount(b, ch);
	AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
	AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
	AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_NONE);
	if (dir == AAUDIO_DIRECTION_INPUT)
		AAudioStreamBuilder_setInputPreset(b, AAUDIO_INPUT_PRESET_CAMCORDER);
	else
		AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_MEDIA);
	aaudio_result_t r = AAudioStreamBuilder_openStream(b, &s);
	AAudioStreamBuilder_delete(b);
	if (r != AAUDIO_OK) {
		dlog("AAudio open %s: %s", dir == AAUDIO_DIRECTION_INPUT ? "input" : "output", AAudio_convertResultToText(r));
		return NULL;
	}
	AAudioStream_requestStart(s);
	return s;
}

struct meter { const char *name; long bytes, frames; double acc, pk; double t0; };

static void meter_add(struct meter *m, const int16_t *s, int n)
{
	for (int i = 0; i < n; i++) {
		double v = s[i] / 32768.0;
		m->acc += v * v;
		if (fabs(v) > m->pk) m->pk = fabs(v);
	}
	m->frames += n / 2;
	m->bytes += n * 2;
}

static double mono()
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec * 1e-9;
}

static void meter_log(struct meter *m, int force)
{
	double t = mono();
	if (!force && t - m->t0 < 1.0) return;
	long n = m->frames * 2;
	dlog("%s: %ld bytes, %ld frames, rms=%.5f peak=%.4f", m->name, m->bytes, m->frames, n ? sqrt(m->acc / n) : 0.0, m->pk);
	m->acc = 0;
	m->pk = 0;
	m->t0 = t;
}

/* guest -> speaker */
static void serve_out(int c)
{
	AAudioStream *s = open_stream(AAUDIO_DIRECTION_OUTPUT, 2);
	if (!s) return;
	dlog("out: client connected, stream open (rate %d ch %d)", AAudioStream_getSampleRate(s), AAudioStream_getChannelCount(s));
	struct meter m = { "out", 0, 0, 0, 0, mono() };
	int16_t buf[FRAMES * 2];
	size_t have = 0;
	for (;;) {
		ssize_t n = read(c, (char *)buf + have, sizeof buf - have);
		if (n <= 0) break;
		have += n;
		if (have < sizeof buf) continue;
		int lim = (int)(max_amp * 32767);
		for (int i = 0; i < FRAMES * 2; i++) buf[i] = buf[i] > lim ? lim : buf[i] < -lim ? -lim : buf[i];
		meter_add(&m, buf, FRAMES * 2);
		aaudio_result_t r = AAudioStream_write(s, buf, FRAMES, 1000000000LL);
		if (r < 0) { dlog("out: write: %s", AAudio_convertResultToText(r)); break; }
		have = 0;
		meter_log(&m, 0);
	}
	meter_log(&m, 1);
	AAudioStream_requestStop(s);
	AAudioStream_close(s);
	dlog("out: client gone, stream closed");
}

/* microphone -> guest */
static void serve_in(int c)
{
	AAudioStream *s = open_stream(AAUDIO_DIRECTION_INPUT, 1);
	if (!s) return;
	dlog("in: client connected, stream open (rate %d ch %d)", AAudioStream_getSampleRate(s), AAudioStream_getChannelCount(s));
	struct meter m = { "in", 0, 0, 0, 0, mono() };
	int16_t mn[FRAMES], st[FRAMES * 2];
	for (;;) {
		aaudio_result_t r = AAudioStream_read(s, mn, FRAMES, 1000000000LL);
		if (r < 0) { dlog("in: read: %s", AAudio_convertResultToText(r)); break; }
		if (r == 0) continue;
		for (int i = 0; i < r; i++) st[2 * i] = st[2 * i + 1] = mn[i];
		meter_add(&m, st, r * 2);
		if (send(c, st, r * 4, MSG_NOSIGNAL) < 0) break;
		meter_log(&m, 0);
	}
	meter_log(&m, 1);
	AAudioStream_requestStop(s);
	AAudioStream_close(s);
	dlog("in: client gone, stream closed");
}

static void *loop(void *arg)
{
	int ls = ((int *)arg)[0], is_out = ((int *)arg)[1];
	for (;;) {
		int c = accept(ls, NULL, NULL);
		if (c < 0) { if (errno == EINTR) continue; dlog("accept: %s", strerror(errno)); sleep(1); continue; }
		if (is_out) serve_out(c); else serve_in(c);
		close(c);
	}
	return NULL;
}

int main(int argc, char **argv)
{
	const char *dir = "/data/local/tmp/oneos/rootfs/tmp", *log = NULL;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--dir") && i + 1 < argc) dir = argv[++i];
		else if (!strcmp(argv[i], "--log") && i + 1 < argc) log = argv[++i];
	}
	dlogf = log ? fopen(log, "a") : stderr;
	if (!dlogf) dlogf = stderr;
	if (getenv("X400_MAXAMP")) max_amp = atof(getenv("X400_MAXAMP")) / 100.0;
	signal(SIGPIPE, SIG_IGN);
	char po[256], pi[256];
	snprintf(po, sizeof po, "%s/x400-audio-out.sock", dir);
	snprintf(pi, sizeof pi, "%s/x400-audio-in.sock", dir);
	static int a_out[2], a_in[2];
	a_out[0] = listen_unix(po); a_out[1] = 1;
	a_in[0] = listen_unix(pi); a_in[1] = 0;
	dlog("x400-audiod up: %s, %s (output ceiling %.0f %%)", po, pi, max_amp * 100);
	pthread_t t;
	pthread_create(&t, NULL, loop, a_in);
	loop(a_out);
	return 0;
}
