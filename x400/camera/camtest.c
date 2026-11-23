// linux-x400: x400-camtest, a PipeWire video consumer for checking the camera nodes (no GStreamer in the image).
// Connects to the node named NODE, receives N frames, prints the mean luma of each and writes the last one as a PPM.
// Usage: x400-camtest NODE [N] [OUT.ppm]     e.g. x400-camtest x400-camera-front 10 /tmp/front.ppm
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>

static struct pw_main_loop *loop;
static struct pw_stream *stream;
static struct spa_video_info_raw vi;
static int want = 10, got;
static const char *out;
static double t0, t1;

static double now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static uint8_t clamp(double v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }

static void write_ppm(const uint8_t *y, uint32_t stride, uint32_t w, uint32_t h)
{
	FILE *f = fopen(out, "wb");
	const uint8_t *uv = y + (size_t)stride * h;

	if (!f) { perror(out); return; }
	fprintf(f, "P6\n%u %u\n255\n", w, h);
	for (uint32_t r = 0; r < h; r++)
		for (uint32_t x = 0; x < w; x++) {
			int Y = y[(size_t)r * stride + x], U = uv[(size_t)(r / 2) * stride + (x & ~1u)] - 128, V = uv[(size_t)(r / 2) * stride + (x & ~1u) + 1] - 128;
			uint8_t p[3] = { clamp(Y + 1.402 * V), clamp(Y - 0.344 * U - 0.714 * V), clamp(Y + 1.772 * U) };
			fwrite(p, 1, 3, f);
		}
	fclose(f);
	printf("wrote %s\n", out);
}

static void on_process(void *data)
{
	struct pw_buffer *b = pw_stream_dequeue_buffer(stream);
	struct spa_data *d;
	uint32_t w = vi.size.width, h = vi.size.height, stride;
	const uint8_t *p;
	double sum = 0;

	(void)data;
	if (!b) return;
	d = &b->buffer->datas[0];
	if (!d->data || !d->chunk->size) { pw_stream_queue_buffer(stream, b); return; }
	stride = d->chunk->stride ? d->chunk->stride : w;
	p = (const uint8_t *)d->data + d->chunk->offset;
	for (uint32_t r = 0; r < h; r++)
		for (uint32_t x = 0; x < w; x++)
			sum += p[(size_t)r * stride + x];
	if (!got) t0 = now();
	t1 = now();
	got++;
	printf("frame %d: %ux%u luma mean %.1f (chunk %u bytes, stride %u)\n", got, w, h, sum / ((double)w * h), d->chunk->size, stride);
	if (got == want) {
		if (got > 1) printf("%.1f fps\n", (got - 1) / (t1 - t0));
		if (out) write_ppm(p, stride, w, h);
		pw_main_loop_quit(loop);
	}
	pw_stream_queue_buffer(stream, b);
}

static void on_param(void *data, uint32_t id, const struct spa_pod *param)
{
	uint8_t buf[512];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
	const struct spa_pod *params[1];

	(void)data;
	if (id != SPA_PARAM_Format || !param) return;
	spa_format_video_raw_parse(param, &vi);
	printf("negotiated %ux%u format %u\n", vi.size.width, vi.size.height, vi.format);
	params[0] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
		SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int((1u << SPA_DATA_MemFd) | (1u << SPA_DATA_MemPtr)));
	pw_stream_update_params(stream, params, 1);
}

static void on_state(void *data, enum pw_stream_state old, enum pw_stream_state s, const char *err)
{
	(void)data; (void)old;
	printf("state %s%s%s\n", pw_stream_state_as_string(s), err ? ": " : "", err ? err : "");
	fflush(stdout);
	if (s == PW_STREAM_STATE_ERROR) pw_main_loop_quit(loop);
}

static const struct pw_stream_events ev = { PW_VERSION_STREAM_EVENTS, .state_changed = on_state, .param_changed = on_param, .process = on_process };

static void on_quit(void *data, int sig) { (void)data; (void)sig; pw_main_loop_quit(loop); }

int main(int argc, char **argv)
{
	uint8_t buf[512];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
	const struct spa_pod *params[1];
	const char *node;
	struct pw_properties *p;

	setvbuf(stdout, NULL, _IOLBF, 0);
	pw_init(&argc, &argv);
	if (argc < 2) { fprintf(stderr, "usage: x400-camtest NODE [N] [OUT.ppm]\n"); return 2; }
	node = argv[1];
	if (argc > 2) want = atoi(argv[2]);
	if (argc > 3) out = argv[3];
	loop = pw_main_loop_new(NULL);
	pw_loop_add_signal(pw_main_loop_get_loop(loop), SIGINT, on_quit, NULL);
	pw_loop_add_signal(pw_main_loop_get_loop(loop), SIGTERM, on_quit, NULL);
	p = pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Camera", PW_KEY_TARGET_OBJECT, node, NULL);
	stream = pw_stream_new_simple(pw_main_loop_get_loop(loop), "x400-camtest", p, &ev, NULL);
	params[0] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
		SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
		SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
		SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_NV12),
		SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&SPA_RECTANGLE(1280, 720), &SPA_RECTANGLE(1, 1), &SPA_RECTANGLE(4096, 4096)),
		SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&SPA_FRACTION(30, 1), &SPA_FRACTION(0, 1), &SPA_FRACTION(120, 1)));
	if (pw_stream_connect(stream, PW_DIRECTION_INPUT, PW_ID_ANY, PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS, params, 1) < 0) {
		fprintf(stderr, "camtest: pw_stream_connect failed\n");
		return 1;
	}
	pw_main_loop_run(loop);
	pw_stream_destroy(stream);
	pw_main_loop_destroy(loop);
	pw_deinit();
	return got == want ? 0 : 1;
}
