/*
 * x400-snd: minimal static tinymix/tinyplay/tinycap replacement (raw ALSA ioctls).
 * Android ships none of them and the guest has no alsa-utils, so this one binary
 * runs both on the Android host and inside the chroot.
 *
 *   x400-snd ctl  <card> list [filter]
 *   x400-snd ctl  <card> get  <numid|name>
 *   x400-snd ctl  <card> set  <numid|name> <v> [v...]     (enum: name or index)
 *   x400-snd ctl  <card> batch                            (stdin: "name<TAB>value" per line; failures are reported, not fatal)
 *   x400-snd info <card> <dev> <p|c>                      (hw params ranges)
 *   x400-snd play <card> <dev> <hz> <secs> <amp%> [rate] [ch] [bits]
 *   x400-snd cap  <card> <dev> <secs> <out.raw> [rate] [ch] [bits]   (prints RMS/peak)
 *
 * Build: aarch64-linux-gnu-gcc -O2 -static x400-snd.c -o x400-snd -lm
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <sound/asound.h>

static int xopen(const char *fmt, int a, int b, char c, int flags)
{
	char p[64];
	if (c) snprintf(p, sizeof p, fmt, a, b, c);
	else snprintf(p, sizeof p, fmt, a);
	int fd = open(p, flags);
	if (fd < 0) { fprintf(stderr, "open %s: %s\n", p, strerror(errno)); exit(2); }
	return fd;
}

/* ---------- controls ---------- */
static int find_numid(int fd, const char *s)
{
	if (s[0] >= '0' && s[0] <= '9') return atoi(s);
	struct snd_ctl_elem_list l = {0};
	ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &l);
	unsigned n = l.count;
	struct snd_ctl_elem_id *ids = calloc(n, sizeof *ids);
	l.space = n; l.pids = ids;
	ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &l);
	int r = -1;
	for (unsigned i = 0; i < l.used; i++)
		if (!strcmp((char *)ids[i].name, s)) { r = ids[i].numid; break; }
	free(ids);
	return r;
}

static void print_val(int fd, struct snd_ctl_elem_info *inf)
{
	struct snd_ctl_elem_value v;
	memset(&v, 0, sizeof v);
	v.id.numid = inf->id.numid;
	if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_READ, &v) < 0) { printf("<unreadable>"); return; }
	for (unsigned i = 0; i < inf->count && i < 16; i++) {
		switch (inf->type) {
		case SNDRV_CTL_ELEM_TYPE_BOOLEAN: printf("%s%s", i ? " " : "", v.value.integer.value[i] ? "On" : "Off"); break;
		case SNDRV_CTL_ELEM_TYPE_INTEGER: printf("%s%ld", i ? " " : "", v.value.integer.value[i]); break;
		case SNDRV_CTL_ELEM_TYPE_INTEGER64: printf("%s%lld", i ? " " : "", (long long)v.value.integer64.value[i]); break;
		case SNDRV_CTL_ELEM_TYPE_ENUMERATED: {
			struct snd_ctl_elem_info e = *inf;
			e.value.enumerated.item = v.value.enumerated.item[i];
			ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &e);
			printf("%s%s", i ? " " : "", e.value.enumerated.name);
			break; }
		default: printf("?"); }
	}
}

static int cmd_ctl(int card, int argc, char **argv);

static int cmd_batch(int card)
{
	char line[512];
	int bad = 0;
	while (fgets(line, sizeof line, stdin)) {
		line[strcspn(line, "\r\n")] = 0;
		char *tab = strchr(line, '\t');
		if (!tab || line[0] == '#') continue;
		*tab = 0;
		char *av[3] = { "set", line, tab + 1 };
		if (cmd_ctl(card, 3, av)) { fprintf(stderr, "FAILED: %s = %s\n", line, tab + 1); bad++; }
	}
	return bad ? 6 : 0;
}

static int cmd_ctl(int card, int argc, char **argv)
{
	int fd = xopen("/dev/snd/controlC%d", card, 0, 0, O_RDWR);
	if (argc < 1) return 1;
	if (!strcmp(argv[0], "batch")) return cmd_batch(card);
	if (!strcmp(argv[0], "list")) {
		const char *flt = argc > 1 ? argv[1] : NULL;
		struct snd_ctl_elem_list l = {0};
		ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &l);
		unsigned n = l.count;
		struct snd_ctl_elem_id *ids = calloc(n, sizeof *ids);
		l.space = n; l.pids = ids;
		ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &l);
		for (unsigned i = 0; i < l.used; i++) {
			if (flt && !strstr((char *)ids[i].name, flt)) continue;
			struct snd_ctl_elem_info inf; memset(&inf, 0, sizeof inf);
			inf.id.numid = ids[i].numid;
			if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &inf) < 0) continue;
			printf("%u\t%s\t%s", ids[i].numid, (char *)ids[i].name,
			       inf.type == 1 ? "bool" : inf.type == 2 ? "int" : inf.type == 3 ? "enum" : "other");
			if (inf.type == SNDRV_CTL_ELEM_TYPE_INTEGER)
				printf("[%ld..%ld]", inf.value.integer.min, inf.value.integer.max);
			printf("\t= ");
			print_val(fd, &inf);
			printf("\n");
		}
		return 0;
	}
	if (argc < 2) return 1;
	int id = find_numid(fd, argv[1]);
	if (id < 0) { fprintf(stderr, "no such control: %s\n", argv[1]); return 3; }
	struct snd_ctl_elem_info inf; memset(&inf, 0, sizeof inf);
	inf.id.numid = id;
	if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &inf) < 0) { perror("info"); return 3; }
	if (!strcmp(argv[0], "get")) {
		printf("%s = ", (char *)inf.id.name); print_val(fd, &inf); printf("\n");
		if (inf.type == SNDRV_CTL_ELEM_TYPE_ENUMERATED) {
			printf("options:");
			for (unsigned i = 0; i < inf.value.enumerated.items; i++) {
				struct snd_ctl_elem_info e = inf; e.value.enumerated.item = i;
				ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &e);
				printf(" [%u]%s", i, e.value.enumerated.name);
			}
			printf("\n");
		}
		return 0;
	}
	if (!strcmp(argv[0], "set")) {
		struct snd_ctl_elem_value v; memset(&v, 0, sizeof v);
		v.id.numid = id;
		ioctl(fd, SNDRV_CTL_IOCTL_ELEM_READ, &v);
		for (unsigned i = 0; i < inf.count; i++) {
			const char *a = argv[2 + (argc - 2 > 1 ? (i < (unsigned)argc - 2 ? i : argc - 3) : 0)];
			if (inf.type == SNDRV_CTL_ELEM_TYPE_ENUMERATED) {
				int idx = -1;
				if (a[0] >= '0' && a[0] <= '9') idx = atoi(a);
				else for (unsigned k = 0; k < inf.value.enumerated.items; k++) {
					struct snd_ctl_elem_info e = inf; e.value.enumerated.item = k;
					ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &e);
					if (!strcmp(e.value.enumerated.name, a)) { idx = k; break; }
				}
				if (idx < 0) { fprintf(stderr, "bad enum value %s\n", a); return 3; }
				v.value.enumerated.item[i] = idx;
			} else if (inf.type == SNDRV_CTL_ELEM_TYPE_BOOLEAN) {
				v.value.integer.value[i] = (!strcmp(a, "On") || !strcmp(a, "on") || !strcmp(a, "1"));
			} else v.value.integer.value[i] = atol(a);
		}
		if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &v) < 0) { perror("write"); return 3; }
		printf("%s = ", (char *)inf.id.name); print_val(fd, &inf); printf("\n");
		return 0;
	}
	return 1;
}

/* ---------- pcm ---------- */
static void m_set(struct snd_pcm_hw_params *p, int var, unsigned bit)
{
	struct snd_mask *m = &p->masks[var - SNDRV_PCM_HW_PARAM_FIRST_MASK];
	memset(m->bits, 0, sizeof m->bits);
	m->bits[bit >> 5] |= 1u << (bit & 31);
}
static void i_set(struct snd_pcm_hw_params *p, int var, unsigned v)
{
	struct snd_interval *i = &p->intervals[var - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
	i->min = i->max = v; i->integer = 1;
}
static void hw_any(struct snd_pcm_hw_params *p)
{
	memset(p, 0, sizeof *p);
	for (int i = 0; i < SNDRV_PCM_HW_PARAM_LAST_MASK - SNDRV_PCM_HW_PARAM_FIRST_MASK + 1; i++)
		memset(p->masks[i].bits, 0xff, sizeof p->masks[i].bits);
	for (int i = 0; i < SNDRV_PCM_HW_PARAM_LAST_INTERVAL - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL + 1; i++) {
		p->intervals[i].min = 0; p->intervals[i].max = ~0u;
	}
	p->rmask = ~0u; p->cmask = 0; p->info = ~0u;
}

static int cmd_info(int card, int dev, char dir)
{
	int fd = xopen("/dev/snd/pcmC%dD%d%c", card, dev, dir, O_RDWR);
	struct snd_pcm_hw_params p; hw_any(&p);
	if (ioctl(fd, SNDRV_PCM_IOCTL_HW_REFINE, &p) < 0) { perror("refine"); return 3; }
	static const struct { int v; const char *n; } iv[] = {
		{ SNDRV_PCM_HW_PARAM_RATE, "rate" }, { SNDRV_PCM_HW_PARAM_CHANNELS, "channels" },
		{ SNDRV_PCM_HW_PARAM_PERIOD_SIZE, "period_size" }, { SNDRV_PCM_HW_PARAM_PERIODS, "periods" },
		{ SNDRV_PCM_HW_PARAM_BUFFER_SIZE, "buffer_size" } };
	for (unsigned k = 0; k < sizeof iv / sizeof *iv; k++) {
		struct snd_interval *i = &p.intervals[iv[k].v - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
		printf("%s: %u..%u\n", iv[k].n, i->min, i->max);
	}
	struct snd_mask *m = &p.masks[SNDRV_PCM_HW_PARAM_FORMAT - SNDRV_PCM_HW_PARAM_FIRST_MASK];
	printf("format mask: %08x %08x\n", m->bits[1], m->bits[0]);
	m = &p.masks[SNDRV_PCM_HW_PARAM_ACCESS - SNDRV_PCM_HW_PARAM_FIRST_MASK];
	printf("access mask: %08x (0=mmap_int 1=mmap_noninterleaved 2=mmap_complex 3=rw_int 4=rw_noninterleaved: bit n)\n", m->bits[0]);
	return 0;
}

static int open_pcm(int card, int dev, char dir, unsigned rate, unsigned ch, unsigned bits,
		    snd_pcm_uframes_t *period)
{
	/* playback is started explicitly after the first fill so errors name the ioctl */
	int fd = xopen("/dev/snd/pcmC%dD%d%c", card, dev, dir, O_RDWR);
	struct snd_pcm_hw_params p; hw_any(&p);
	m_set(&p, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
	m_set(&p, SNDRV_PCM_HW_PARAM_FORMAT, bits == 32 ? SNDRV_PCM_FORMAT_S32_LE :
	      bits == 24 ? SNDRV_PCM_FORMAT_S24_LE : SNDRV_PCM_FORMAT_S16_LE);
	m_set(&p, SNDRV_PCM_HW_PARAM_SUBFORMAT, 0);
	i_set(&p, SNDRV_PCM_HW_PARAM_CHANNELS, ch);
	i_set(&p, SNDRV_PCM_HW_PARAM_RATE, rate);
	i_set(&p, SNDRV_PCM_HW_PARAM_FRAME_BITS, (bits == 16 ? 16 : 32) * ch);
	i_set(&p, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, *period);
	i_set(&p, SNDRV_PCM_HW_PARAM_PERIODS, 4);
	p.rmask = ~0u;
	if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &p) < 0) {
		fprintf(stderr, "hw_params(%u Hz, %u ch, %u bit, period %lu): %s\n", rate, ch, bits, *period, strerror(errno));
		exit(4);
	}
	struct snd_pcm_sw_params s; memset(&s, 0, sizeof s);
	s.tstamp_mode = SNDRV_PCM_TSTAMP_ENABLE;
	s.period_step = 1; s.avail_min = *period;
	s.start_threshold = dir == 'p' ? 0x7fffffff / 8 : 1; s.stop_threshold = 4 * *period;
	s.boundary = 4 * *period; while (s.boundary * 2 <= 0x7fffffff / 4) s.boundary *= 2;
	s.xfer_align = 1;
	if (ioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &s) < 0) { perror("sw_params"); exit(4); }
	if (ioctl(fd, SNDRV_PCM_IOCTL_PREPARE) < 0) { perror("prepare"); exit(4); }
	return fd;
}

static int cmd_play(int card, int dev, double hz, double secs, double amp, unsigned rate, unsigned ch, unsigned bits)
{
	snd_pcm_uframes_t per = getenv("X400_PERIOD") ? atoi(getenv("X400_PERIOD")) : 1024;
	int fd = open_pcm(card, dev, 'p', rate, ch, bits, &per);
	size_t bps = (bits == 16 ? 2 : 4);
	void *buf = calloc(per, bps * ch);
	double ph = 0, st = 2 * M_PI * hz / rate;
	long total = (long)(secs * rate), done = 0;
	int started = 0;
	while (done < total) {
		for (unsigned i = 0; i < per; i++, ph += st) {
			double fade = 1.0;
			if (done + i < rate / 20) fade = (double)(done + i) / (rate / 20);
			double s = sin(ph) * amp * fade;
			for (unsigned c = 0; c < ch; c++) {
				if (bits == 16) ((int16_t *)buf)[i * ch + c] = (int16_t)(s * 32767);
				else ((int32_t *)buf)[i * ch + c] = (int32_t)(s * 2147483647.0) & (bits == 24 ? ~0xff : ~0);
			}
		}
		struct snd_xferi x = { .buf = buf, .frames = per };
		if (ioctl(fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &x) < 0) {
			if (errno == EPIPE) { ioctl(fd, SNDRV_PCM_IOCTL_PREPARE); started = 0; continue; }
			fprintf(stderr, "writei #%ld after %ld frames, started=%d: %s\n", done / per, done, started, strerror(errno)); return 5;
		}
		done += per;
		if (!started && done >= 2 * per) {
			if (ioctl(fd, SNDRV_PCM_IOCTL_START) < 0) { perror("start"); return 5; }
			started = 1;
		}
	}
	ioctl(fd, SNDRV_PCM_IOCTL_DRAIN);
	printf("played %.1f Hz amp %.0f%% %.1fs on card%d dev%d (%u Hz %u ch %u bit)\n", hz, amp * 100, secs, card, dev, rate, ch, bits);
	return 0;
}

static int cmd_cap(int card, int dev, double secs, const char *out, unsigned rate, unsigned ch, unsigned bits)
{
	snd_pcm_uframes_t per = getenv("X400_PERIOD") ? atoi(getenv("X400_PERIOD")) : 1024;
	int fd = open_pcm(card, dev, 'c', rate, ch, bits, &per);
	size_t fs = (bits == 16 ? 2 : 4) * ch;
	void *buf = malloc(per * fs);
	FILE *o = fopen(out, "wb");
	long total = (long)(secs * rate), done = 0;
	double acc[8] = {0}, peak[8] = {0};
	if (ioctl(fd, SNDRV_PCM_IOCTL_START) < 0 && errno != EBADFD) perror("start");
	while (done < total) {
		struct snd_xferi x = { .buf = buf, .frames = per };
		if (ioctl(fd, SNDRV_PCM_IOCTL_READI_FRAMES, &x) < 0) {
			if (errno == EPIPE) { ioctl(fd, SNDRV_PCM_IOCTL_PREPARE); ioctl(fd, SNDRV_PCM_IOCTL_START); continue; }
			perror("readi"); return 5;
		}
		fwrite(buf, fs, x.result, o);
		for (long i = 0; i < (long)x.result; i++)
			for (unsigned c = 0; c < ch && c < 8; c++) {
				double v = bits == 16 ? ((int16_t *)buf)[i * ch + c] / 32768.0
				                      : ((int32_t *)buf)[i * ch + c] / 2147483648.0;
				acc[c] += v * v; if (fabs(v) > peak[c]) peak[c] = fabs(v);
			}
		done += x.result;
	}
	fclose(o);
	for (unsigned c = 0; c < ch && c < 8; c++)
		printf("ch%u rms=%.6f (%.1f dBFS) peak=%.6f\n", c, sqrt(acc[c] / done), 20 * log10(sqrt(acc[c] / done) + 1e-12), peak[c]);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 3) { fprintf(stderr, "usage: see header of x400-snd.c\n"); return 1; }
	int card = atoi(argv[2]);
	if (!strcmp(argv[1], "ctl")) return cmd_ctl(card, argc - 3, argv + 3);
	if (argc < 4) return 1;
	int dev = atoi(argv[3]);
	if (!strcmp(argv[1], "info") && argc >= 5) return cmd_info(card, dev, argv[4][0]);
	if (!strcmp(argv[1], "play") && argc >= 7)
		return cmd_play(card, dev, atof(argv[4]), atof(argv[5]), atof(argv[6]) / 100.0,
				argc > 7 ? atoi(argv[7]) : 48000, argc > 8 ? atoi(argv[8]) : 2, argc > 9 ? atoi(argv[9]) : 16);
	if (!strcmp(argv[1], "cap") && argc >= 6)
		return cmd_cap(card, dev, atof(argv[4]), argv[5],
			       argc > 6 ? atoi(argv[6]) : 48000, argc > 7 ? atoi(argv[7]) : 2, argc > 8 ? atoi(argv[8]) : 16);
	return 1;
}
