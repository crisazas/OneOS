/*
 * x400-pwbridge: PipeWire side of the audio bridge, runs in the guest.
 *
 *   x400-pwbridge [--dir DIR]
 *
 * Creates two PipeWire nodes and connects each to the unix socket of x400-audiod (Android host):
 *   Sink   "Altavoces" (Audio/Sink)   -> DIR/x400-audio-out.sock   (s16le 48000 stereo, to the speaker)
 *   Source "Microfono" (Audio/Source) <- DIR/x400-audio-in.sock    (s16le 48000 stereo, from the microphone)
 * A socket is connected only while the node is in use (stream state "streaming"), so the host opens the
 * speaker / microphone only while there is a client. DIR defaults to /tmp.
 * Build: see x400/audio/build.sh (sysroot of out/rootfs-plasma).
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <spa/param/audio/format-utils.h>
#include <pipewire/pipewire.h>

struct node {
	const char *name, *sock;
	int is_sink, fd;
	struct pw_stream *stream;
	long bytes;
	struct spa_hook hook;
};

static struct pw_main_loop *loop;
static struct node out_n = { .is_sink = 1, .fd = -1 }, in_n = { .fd = -1 };

static int sock_connect(struct node *n)
{
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	strncpy(a.sun_path, n->sock, sizeof a.sun_path - 1);
	int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (s < 0) return -1;
	if (connect(s, (struct sockaddr *)&a, sizeof a) < 0) {
		fprintf(stderr, "%s: connect %s: %s\n", n->name, n->sock, strerror(errno));
		close(s);
		return -1;
	}
	if (!n->is_sink) {
		int fl = fcntl(s, F_GETFL);
		fcntl(s, F_SETFL, fl | O_NONBLOCK);
	}
	return s;
}

static void on_state(void *data, enum pw_stream_state old, enum pw_stream_state state, const char *err)
{
	struct node *n = data;
	fprintf(stderr, "%s: %s -> %s%s%s\n", n->name, pw_stream_state_as_string(old), pw_stream_state_as_string(state),
		err ? ": " : "", err ? err : "");
	if (state == PW_STREAM_STATE_STREAMING && n->fd < 0) {
		n->fd = sock_connect(n);
		fprintf(stderr, "%s: socket %s\n", n->name, n->fd >= 0 ? "connected" : "NOT connected");
	} else if (state != PW_STREAM_STATE_STREAMING && n->fd >= 0) {
		close(n->fd);
		n->fd = -1;
		fprintf(stderr, "%s: socket closed (%ld bytes this session)\n", n->name, n->bytes);
		n->bytes = 0;
	}
}

static void on_process_sink(void *data)
{
	struct node *n = data;
	struct pw_buffer *b = pw_stream_dequeue_buffer(n->stream);
	if (!b) return;
	struct spa_data *d = &b->buffer->datas[0];
	if (d->data && n->fd >= 0) {
		const char *p = (const char *)d->data + d->chunk->offset;
		uint32_t left = d->chunk->size;
		while (left) {
			ssize_t w = send(n->fd, p, left, MSG_NOSIGNAL);
			if (w < 0) {
				if (errno == EINTR) continue;
				if (errno == EAGAIN) { usleep(1000); continue; }
				close(n->fd);
				n->fd = -1;
				break;
			}
			n->bytes += w;
			p += w;
			left -= w;
		}
	}
	pw_stream_queue_buffer(n->stream, b);
}

static void on_process_source(void *data)
{
	struct node *n = data;
	struct pw_buffer *b = pw_stream_dequeue_buffer(n->stream);
	if (!b) return;
	struct spa_data *d = &b->buffer->datas[0];
	if (d->data) {
		uint32_t want = d->maxsize;
		if (b->requested) want = SPA_MIN(want, (uint32_t)(b->requested * 4));
		ssize_t r = n->fd >= 0 ? recv(n->fd, d->data, want, 0) : -1;
		if (r > 0) {
			n->bytes += r;
			if ((size_t)r < want) memset((char *)d->data + r, 0, want - r);
		} else {
			if (r == 0) { close(n->fd); n->fd = -1; }
			memset(d->data, 0, want);
		}
		d->chunk->offset = 0;
		d->chunk->stride = 4;
		d->chunk->size = want;
	}
	pw_stream_queue_buffer(n->stream, b);
}

static const struct pw_stream_events ev_sink = { PW_VERSION_STREAM_EVENTS, .state_changed = on_state, .process = on_process_sink };
static const struct pw_stream_events ev_source = { PW_VERSION_STREAM_EVENTS, .state_changed = on_state, .process = on_process_source };

static void make(struct node *n, struct pw_core *core, const char *desc, const char *nodename)
{
	struct pw_properties *p = pw_properties_new(
		PW_KEY_NODE_NAME, nodename,
		PW_KEY_NODE_DESCRIPTION, desc,
		PW_KEY_MEDIA_CLASS, n->is_sink ? "Audio/Sink" : "Audio/Source",
		PW_KEY_MEDIA_TYPE, "Audio",
		PW_KEY_MEDIA_CATEGORY, n->is_sink ? "Playback" : "Capture",
		PW_KEY_NODE_LATENCY, "480/48000",
		PW_KEY_AUDIO_CHANNELS, "2",
		"audio.position", "FL,FR",
		"node.pause-on-idle", "true",
		"node.suspend-on-idle", "true",
		NULL);
	n->stream = pw_stream_new(core, desc, p);
	uint8_t buf[512];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
	struct spa_audio_info_raw info = SPA_AUDIO_INFO_RAW_INIT(.format = SPA_AUDIO_FORMAT_S16_LE, .rate = 48000, .channels = 2,
								 .position = { SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR });
	const struct spa_pod *params[1] = { spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info) };
	pw_stream_add_listener(n->stream, &n->hook, n->is_sink ? &ev_sink : &ev_source, n);
	int rc = pw_stream_connect(n->stream, n->is_sink ? PW_DIRECTION_INPUT : PW_DIRECTION_OUTPUT, PW_ID_ANY,
				   PW_STREAM_FLAG_MAP_BUFFERS,
				   params, 1);
	fprintf(stderr, "%s: node '%s' created (%d)\n", desc, nodename, rc);
}

static void quit(void *d, int sig)
{
	(void)d; (void)sig;
	pw_main_loop_quit(loop);
}

int main(int argc, char **argv)
{
	const char *dir = "/tmp";
	for (int i = 1; i < argc; i++)
		if (!strcmp(argv[i], "--dir") && i + 1 < argc) dir = argv[++i];
	static char so[256], si[256];
	snprintf(so, sizeof so, "%s/x400-audio-out.sock", dir);
	snprintf(si, sizeof si, "%s/x400-audio-in.sock", dir);
	out_n.sock = so;
	in_n.sock = si;
	out_n.name = "Altavoces";
	in_n.name = "Microfono";
	pw_init(&argc, &argv);
	loop = pw_main_loop_new(NULL);
	pw_loop_add_signal(pw_main_loop_get_loop(loop), SIGINT, quit, NULL);
	pw_loop_add_signal(pw_main_loop_get_loop(loop), SIGTERM, quit, NULL);
	struct pw_context *ctx = pw_context_new(pw_main_loop_get_loop(loop), NULL, 0);
	struct pw_core *core = pw_context_connect(ctx, NULL, 0);
	if (!core) { fprintf(stderr, "cannot connect to PipeWire: %s\n", strerror(errno)); return 1; }
	make(&out_n, core, "Altavoces", "x400.altavoces");
	make(&in_n, core, "Microfono", "x400.microfono");
	pw_main_loop_run(loop);
	pw_context_destroy(ctx);
	pw_main_loop_destroy(loop);
	return 0;
}
