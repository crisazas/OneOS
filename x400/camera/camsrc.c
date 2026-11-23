// linux-x400: x400-camsrc, runs in the guest. Publishes the tablet's cameras as PipeWire Video/Source nodes (media.role Camera, which is
// what xdg-desktop-portal's Camera portal lists) and takes the frames from x400-camd (Android side) over /tmp/x400-cam.sock (camproto.h).
// The camera is only open while somebody uses the node: the stream state STREAMING opens the socket (x400-camd then opens the sensor),
// leaving it closes the socket (the sensor is closed). Frames are NV12; the sizes offered are SIZES below.
// Usage: x400-camsrc [front|back|both]     (default both; env X400_CAM_SOCK overrides the socket path)
#define _GNU_SOURCE
#include "camproto.h"
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/param/props.h>

static const struct { uint32_t w, h; } SIZES[] = { { 1280, 720 }, { 640, 480 } };
#define NSIZES 2
#define FPS 30

struct cam {
	const char *name, *desc;
	uint32_t id;			// Android camera id
	struct pw_stream *stream;
	struct spa_hook listener;
	struct spa_source *ev;
	pthread_t th;
	int th_on;
	pthread_mutex_t m;
	int fd;				// socket owned by the reader thread, shutdown() by the loop thread
	volatile int stop;
	uint8_t *last;			// newest frame
	size_t lastcap, lastsize;
	uint64_t seq, ts;
	int fresh;
	uint32_t w, h;			// negotiated size
	unsigned trig, trig_err, processed;
};

static struct pw_main_loop *loop;
static const char *sockpath = CAM_SOCK_GUEST;

static int read_all(int fd, void *b, size_t n)
{
	char *p = b;

	while (n) {
		ssize_t r = recv(fd, p, n, 0);
		if (r < 0 && errno == EINTR) continue;
		if (r <= 0) return -1;
		p += r; n -= r;
	}
	return 0;
}

static int connect_camd(struct cam *c)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

	snprintf(sa.sun_path, sizeof sa.sun_path, "%s", sockpath);
	if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof sa)) {
		if (fd >= 0) close(fd);
		return -1;
	}
	(void)c;
	return fd;
}

static void *reader(void *arg)
{
	struct cam *c = arg;
	uint8_t *buf = NULL;
	size_t cap = 0;

	while (!c->stop) {
		int fd = connect_camd(c);
		struct cam_req rq = { CAM_MAGIC, CAM_REQ_OPEN, c->id, c->w, c->h, FPS };
		struct cam_hdr hd;

		if (fd < 0) { fprintf(stderr, "camsrc[%s]: cannot connect %s: %s\n", c->name, sockpath, strerror(errno)); sleep(1); continue; }
		pthread_mutex_lock(&c->m); c->fd = fd; pthread_mutex_unlock(&c->m);
		if (c->stop || send(fd, &rq, sizeof rq, MSG_NOSIGNAL) != (ssize_t)sizeof rq || read_all(fd, &hd, sizeof hd) || hd.magic != CAM_MAGIC) goto out;
		if (hd.type == CAM_ERROR) { fprintf(stderr, "camsrc[%s]: camd could not open camera %u: %d\n", c->name, c->id, hd.error); goto retry; }
		fprintf(stderr, "camsrc[%s]: camera %u open, %ux%u\n", c->name, c->id, hd.width, hd.height);
		while (!c->stop && !read_all(fd, &hd, sizeof hd) && hd.magic == CAM_MAGIC && hd.type == CAM_FRAME) {
			if (hd.size > cap) { cap = hd.size; buf = realloc(buf, cap); }
			if (read_all(fd, buf, hd.size)) break;
			pthread_mutex_lock(&c->m);
			if (c->lastcap < hd.size) { c->lastcap = hd.size; c->last = realloc(c->last, hd.size); }
			memcpy(c->last, buf, hd.size);
			c->lastsize = hd.size; c->seq = hd.seq; c->ts = hd.ts_ns; c->fresh = 1;
			pthread_mutex_unlock(&c->m);
			pw_loop_signal_event(pw_main_loop_get_loop(loop), c->ev);
		}
		goto out;
retry:
		sleep(2);
out:
		pthread_mutex_lock(&c->m); c->fd = -1; pthread_mutex_unlock(&c->m);
		close(fd);
	}
	free(buf);
	return NULL;
}

static void start_reader(struct cam *c)
{
	if (c->th_on) return;
	c->stop = 0; c->fd = -1;
	if (!pthread_create(&c->th, NULL, reader, c)) c->th_on = 1;
}

static void stop_reader(struct cam *c)
{
	if (!c->th_on) return;
	c->stop = 1;
	pthread_mutex_lock(&c->m);
	if (c->fd >= 0) shutdown(c->fd, SHUT_RDWR);
	pthread_mutex_unlock(&c->m);
	pthread_join(c->th, NULL);
	c->th_on = 0;
	fprintf(stderr, "camsrc[%s]: camera closed\n", c->name);
}

// a frame arrived on the reader thread: let the graph run once
static void on_event(void *data, uint64_t count)
{
	struct cam *c = data;
	int r = pw_stream_trigger_process(c->stream);

	(void)count;
	if (r < 0 && !c->trig_err++)
		fprintf(stderr, "camsrc[%s]: pw_stream_trigger_process: %s\n", c->name, strerror(-r));
	c->trig++;
}

static void on_process(void *data)
{
	struct cam *c = data;
	struct pw_buffer *b = pw_stream_dequeue_buffer(c->stream);
	struct spa_data *d;
	struct spa_meta_header *h;

	if (!b) return;
	c->processed++;
	d = &b->buffer->datas[0];
	pthread_mutex_lock(&c->m);
	if (d->data && c->last && c->lastsize <= d->maxsize) {
		memcpy(d->data, c->last, c->lastsize);
		d->chunk->offset = 0;
		d->chunk->size = c->lastsize;
		d->chunk->stride = c->w;
		if ((h = spa_buffer_find_meta_data(b->buffer, SPA_META_Header, sizeof *h))) {
			h->pts = c->ts;
			h->seq = c->seq;
			h->flags = 0;
			h->dts_offset = 0;
		}
	} else {
		d->chunk->size = 0;
	}
	pthread_mutex_unlock(&c->m);
	pw_stream_queue_buffer(c->stream, b);
}

static void on_state(void *data, enum pw_stream_state old, enum pw_stream_state state, const char *error)
{
	struct cam *c = data;

	fprintf(stderr, "camsrc[%s]: %s -> %s%s%s\n", c->name, pw_stream_state_as_string(old), pw_stream_state_as_string(state), error ? ": " : "", error ? error : "");
	if (state == PW_STREAM_STATE_STREAMING) start_reader(c);
	else {
		fprintf(stderr, "camsrc[%s]: triggers %u processed %u errors %u\n", c->name, c->trig, c->processed, c->trig_err);
		stop_reader(c);
	}
}

static void on_param(void *data, uint32_t id, const struct spa_pod *param)
{
	struct cam *c = data;
	struct spa_video_info_raw vi;
	uint8_t buf[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
	const struct spa_pod *params[2];

	if (id != SPA_PARAM_Format || !param) return;
	if (spa_format_video_raw_parse(param, &vi) < 0) return;
	c->w = vi.size.width; c->h = vi.size.height;
	fprintf(stderr, "camsrc[%s]: format %ux%u\n", c->name, c->w, c->h);
	params[0] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
		SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 8),
		SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
		SPA_PARAM_BUFFERS_size, SPA_POD_Int(c->w * c->h * 3 / 2),
		SPA_PARAM_BUFFERS_stride, SPA_POD_Int(c->w),
		SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int((1u << SPA_DATA_MemFd) | (1u << SPA_DATA_MemPtr)));
	params[1] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
		SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));
	pw_stream_update_params(c->stream, params, 2);
}

static const struct pw_stream_events ev = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_state,
	.param_changed = on_param,
	.process = on_process,
};

static int cam_publish(struct cam *c)
{
	uint8_t buf[2048];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
	const struct spa_pod *params[NSIZES];
	struct pw_properties *p = pw_properties_new(
		PW_KEY_MEDIA_CLASS, "Video/Source", PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Camera",
		PW_KEY_NODE_NAME, c->name, PW_KEY_NODE_DESCRIPTION, c->desc, PW_KEY_NODE_NICK, c->desc, "node.pause-on-idle", "true",
		"api.x400.camera.id", c->id == 0 ? "0" : "1", NULL);

	pthread_mutex_init(&c->m, NULL);
	c->fd = -1;
	c->ev = pw_loop_add_event(pw_main_loop_get_loop(loop), on_event, c);
	c->stream = pw_stream_new_simple(pw_main_loop_get_loop(loop), c->name, p, &ev, c);
	if (!c->stream) return -1;
	for (int i = 0; i < NSIZES; i++)
		params[i] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
			SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
			SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
			SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_NV12),
			SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&SPA_RECTANGLE(SIZES[i].w, SIZES[i].h)),
			SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&SPA_FRACTION(FPS, 1)));
	return pw_stream_connect(c->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
				 PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_MAP_BUFFERS, params, NSIZES);
}

static void on_quit(void *data, int sig)
{
	(void)data; (void)sig;
	pw_main_loop_quit(loop);
}

int main(int argc, char **argv)
{
	static struct cam cams[2] = {
		{ .name = "x400-camera-front", .desc = "Front camera (x400)", .id = 1 },
		{ .name = "x400-camera-back", .desc = "Rear camera (x400)", .id = 0 },
	};
	const char *which = argc > 1 ? argv[1] : "both";

	if (getenv("X400_CAM_SOCK")) sockpath = getenv("X400_CAM_SOCK");
	signal(SIGPIPE, SIG_IGN);
	pw_init(&argc, &argv);
	loop = pw_main_loop_new(NULL);
	pw_loop_add_signal(pw_main_loop_get_loop(loop), SIGINT, on_quit, NULL);
	pw_loop_add_signal(pw_main_loop_get_loop(loop), SIGTERM, on_quit, NULL);
	for (int i = 0; i < 2; i++) {
		if (!strcmp(which, "front") && i != 0) continue;
		if (!strcmp(which, "back") && i != 1) continue;
		if (cam_publish(&cams[i]) < 0) { fprintf(stderr, "camsrc: cannot publish %s\n", cams[i].name); return 1; }
		fprintf(stderr, "camsrc: published %s (Android camera %u)\n", cams[i].name, cams[i].id);
	}
	pw_main_loop_run(loop);
	for (int i = 0; i < 2; i++)
		if (cams[i].stream) { stop_reader(&cams[i]); pw_stream_destroy(cams[i].stream); }
	pw_main_loop_destroy(loop);
	pw_deinit();
	return 0;
}
