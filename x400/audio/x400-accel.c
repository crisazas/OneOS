/*
 * x400-accel: sample the accelerometer through Samsung's sysfs and print per-window statistics,
 * to tell whether the chassis vibrates while the speaker plays (independent of the microphone).
 *
 *   x400-accel <secs> [window_s]     one line per window: n, rate, per-axis std (LSB, 4096 = 1 g), and
 *                                    the std of the high-passed |a| (mean removed, 1st difference).
 *
 * Enabling follows x400/sensors/RESULT.md: "11" in ONE write without newline to ssp_sensor/enable.
 * The previous bit state is read first; the sensor is switched off again ("10") only if it was off.
 * Build: x400/audio/build-android.sh
 */
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define EN "/sys/class/sensors/ssp_sensor/enable"
#define RAW "/sys/class/sensors/accelerometer_sensor/raw_data"

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec * 1e-9;
}

static int write_once(const char *path, const char *s)
{
	int fd = open(path, O_WRONLY);
	if (fd < 0) return -1;
	int r = write(fd, s, strlen(s));
	close(fd);
	return r;
}

static long read_en(void)
{
	char b[64] = "";
	int fd = open(EN, O_RDONLY);
	if (fd < 0) return -1;
	int n = read(fd, b, sizeof b - 1);
	close(fd);
	return n > 0 ? strtol(b, NULL, 0) : -1;
}

struct stat_acc { long n, fresh; double last[3]; double s[3], q[3], d2, d1, prev; int have; };

static void flush(struct stat_acc *a, double t, double dt)
{
	if (a->n < 2) return;
	double sd[3];
	for (int i = 0; i < 3; i++) {
		double m = a->s[i] / a->n;
		sd[i] = sqrt(fmax(a->q[i] / a->n - m * m, 0));
	}
	printf("t=%.1f n=%ld fresh=%ld (%.0f/s) std=%.2f,%.2f,%.2f diffstd=%.2f\n", t, a->n, a->fresh, a->fresh / dt, sd[0], sd[1], sd[2],
	       sqrt(a->d2 / a->n));
	fflush(stdout);
	memset(a, 0, sizeof *a);
}

int main(int argc, char **argv)
{
	double secs = argc > 1 ? atof(argv[1]) : 5, win = argc > 2 ? atof(argv[2]) : 1;
	long before = read_en();
	int was_on = before >= 0 && (before & 2);
	fprintf(stderr, "enable before: %ld (accelerometer %s)\n", before, was_on ? "on" : "off");
	if (!was_on && write_once(EN, "11") < 0) { perror("enable"); return 2; }
	usleep(300000);
	struct stat_acc a;
	memset(&a, 0, sizeof a);
	double t0 = now(), wstart = t0;
	char buf[128];
	while (now() - t0 < secs) {
		int fd = open(RAW, O_RDONLY);
		if (fd < 0) { perror("raw"); break; }
		int n = read(fd, buf, sizeof buf - 1);
		close(fd);
		if (n <= 0) continue;
		buf[n] = 0;
		double v[3];
		if (sscanf(buf, "%lf,%lf,%lf", &v[0], &v[1], &v[2]) != 3) continue;
		if (v[0] == a.last[0] && v[1] == a.last[1] && v[2] == a.last[2]) continue;
		memcpy(a.last, v, sizeof v);
		a.fresh++;
		for (int i = 0; i < 3; i++) { a.s[i] += v[i]; a.q[i] += v[i] * v[i]; }
		double mag = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
		if (a.have) a.d2 += (mag - a.prev) * (mag - a.prev);
		a.prev = mag; a.have = 1;
		a.n++;
		double t = now();
		if (t - wstart >= win) { flush(&a, t - t0, t - wstart); wstart = t; }
	}
	flush(&a, now() - t0, now() - wstart);
	if (!was_on) write_once(EN, "10");
	fprintf(stderr, "enable after: %ld\n", read_en());
	return 0;
}
