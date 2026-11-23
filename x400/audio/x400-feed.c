/* x400-feed: send a gated test tone (3 s tone / 3 s silence) to x400-audiod's playback socket, in real time.
 *   x400-feed <socket> <secs> <hz> <amp%>      s16le 48000 stereo
 * Static arm64 (build.sh); runs on the Android host. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	if (argc < 5) { fprintf(stderr, "usage: x400-feed <socket> <secs> <hz> <amp%%>\n"); return 1; }
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	strncpy(a.sun_path, argv[1], sizeof a.sun_path - 1);
	int s = socket(AF_UNIX, SOCK_STREAM, 0);
	if (connect(s, (struct sockaddr *)&a, sizeof a) < 0) { perror("connect"); return 2; }
	double secs = atof(argv[2]), hz = atof(argv[3]), amp = atof(argv[4]) / 100.0;
	const int R = 48000;
	int16_t buf[480 * 2];
	struct timespec t0;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	long blocks = (long)(secs * R / 480);
	for (long b = 0; b < blocks; b++) {
		for (int i = 0; i < 480; i++) {
			long n = b * 480 + i;
			int16_t v = ((n / (3 * R)) % 2 == 0) ? (int16_t)(sin(2 * M_PI * hz * n / R) * amp * 32767) : 0;
			buf[2 * i] = buf[2 * i + 1] = v;
		}
		if (write(s, buf, sizeof buf) < 0) { perror("write"); return 3; }
		struct timespec t;
		do { usleep(2000); clock_gettime(CLOCK_MONOTONIC, &t); }
		while ((t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) * 1e-9 < (b + 1) * 480.0 / R - 0.05);
	}
	close(s);
	printf("sent %ld bytes\n", blocks * (long)sizeof buf);
	return 0;
}
