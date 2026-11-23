// linux-x400: asks KWin (zkde_screencast_unstable_v1) for a PipeWire stream of its first output and hands every frame to the
// display helper that shows it on the tablet. Runs inside the chroot.
// With the helper connected to FILE.sock, a frame is not copied: the PipeWire buffer (a memfd KWin allocated) is kept and its
// descriptor is sent to the helper (SCM_RIGHTS), which reads the pixels from it and answers when done. Without the helper
// the frame is copied into the frame file FILE (fb.h), as before. FILE's seq counts the frames in both modes.
// Usage: capture FILE   (env WAYLAND_DISPLAY, XDG_RUNTIME_DIR; PipeWire must be running; X400_NOFD=1 never uses the socket)
#define _GNU_SOURCE
#include "fb.h"
#include "zkde-screencast-unstable-v1-client-protocol.h"
#include "fdframe.h"
#include <errno.h>
#include <fcntl.h>
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/props.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

static struct wl_display *dpy;
static struct wl_output *output;
static struct zkde_screencast_unstable_v1 *cast;
static uint32_t node = 0, stream_failed;
static struct pw_main_loop *loop;
static struct pw_stream *stream;
static struct spa_video_info_raw vinfo;
static uint8_t *map; static size_t map_size; static const char *path;
static uint64_t seq, frames; static struct timespec t0;

struct bufinfo { uint32_t id; int sent; };
static struct bufinfo *infos[32]; static uint32_t next_id = 1;
static int lsock = -1, csock = -1;
static struct spa_source *lsrc, *csrc;
static struct pw_buffer *inflight, *pending;
static uint64_t sent_frames, copied_frames;

static void count_frame(void)
{
  if (++frames % 120 == 0) {
    struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
    double dt = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;
    fprintf(stderr, "capture: %llu frames, %.1f fps over the last %.1f s (%llu by descriptor, %llu copied)\n", (unsigned long long)frames, 120 / dt, dt,
            (unsigned long long)sent_frames, (unsigned long long)copied_frames);
    t0 = t1;
  }
}

static void drop_client(void)
{
  if (csrc) { pw_loop_destroy_source(pw_main_loop_get_loop(loop), csrc); csrc = NULL; }
  if (csock >= 0) { close(csock); csock = -1; fprintf(stderr, "capture: the display helper left the socket; copying into the frame file again\n"); }
  if (inflight) { pw_stream_queue_buffer(stream, inflight); inflight = NULL; }
  if (pending) { pw_stream_queue_buffer(stream, pending); pending = NULL; }
  for (int i = 0; i < 32; i++) if (infos[i]) infos[i]->sent = 0;
}

static int send_frame(struct pw_buffer *b)
{
  struct spa_data *sd = &b->buffer->datas[0]; struct bufinfo *bi = b->user_data;
  struct fb_header *h = (struct fb_header *)map;
  struct fd_msg m = { .type = FDM_FRAME, .id = bi->id, .has_fd = !bi->sent, .width = vinfo.size.width, .height = vinfo.size.height,
                      .stride = sd->chunk->stride ? (uint32_t)sd->chunk->stride : vinfo.size.width * 4, .offset = sd->chunk->offset,
                      .mapoffset = sd->mapoffset, .maxsize = sd->maxsize, .seq = h ? h->seq : 0 };
  struct iovec iov = { &m, sizeof m };
  char cb[CMSG_SPACE(sizeof(int))] = {0};
  struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1 };
  if (m.has_fd) {
    mh.msg_control = cb; mh.msg_controllen = sizeof cb;
    struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int));
    int fd = (int)sd->fd; memcpy(CMSG_DATA(c), &fd, sizeof fd);
  }
  if (sendmsg(csock, &mh, MSG_NOSIGNAL) != (ssize_t)sizeof m) return -1;
  bi->sent = 1; inflight = b; sent_frames++;
  return 0;
}

static void on_client(void *d, int fd, uint32_t mask)
{
  (void)d; (void)fd;
  struct fd_reply r; ssize_t n = 0;
  if (mask & SPA_IO_IN) n = recv(csock, &r, sizeof r, MSG_DONTWAIT);
  if ((mask & (SPA_IO_ERR | SPA_IO_HUP)) || n == 0 || (n < 0 && errno != EAGAIN)) { drop_client(); return; }
  if (n != (ssize_t)sizeof r || r.type != FDM_DONE) return;
  if (inflight && ((struct bufinfo *)inflight->user_data)->id == r.id) {
    pw_stream_queue_buffer(stream, inflight); inflight = NULL;
    if (pending) { struct pw_buffer *p = pending; pending = NULL; if (send_frame(p) < 0) { pw_stream_queue_buffer(stream, p); drop_client(); } }
  }
}

static void on_accept(void *d, int fd, uint32_t mask)
{
  (void)d; (void)mask;
  int c = accept4(fd, NULL, NULL, SOCK_CLOEXEC);
  if (c < 0) return;
  if (csock >= 0) drop_client();
  csock = c;
  csrc = pw_loop_add_io(pw_main_loop_get_loop(loop), csock, SPA_IO_IN | SPA_IO_ERR | SPA_IO_HUP, false, on_client, NULL);
  fprintf(stderr, "capture: the display helper connected: frames go by descriptor\n");
}

static void on_add_buffer(void *d, struct pw_buffer *b)
{
  (void)d;
  struct bufinfo *bi = calloc(1, sizeof *bi); if (!bi) return;
  bi->id = next_id++; b->user_data = bi;
  for (int i = 0; i < 32; i++) if (!infos[i]) { infos[i] = bi; break; }
}

static void on_remove_buffer(void *d, struct pw_buffer *b)
{
  (void)d;
  struct bufinfo *bi = b->user_data; if (!bi) return;
  if (b == inflight) inflight = NULL;
  if (b == pending) pending = NULL;
  if (csock >= 0 && bi->sent) {
    struct fd_msg m = { .type = FDM_DROP, .id = bi->id };
    if (send(csock, &m, sizeof m, MSG_NOSIGNAL) != (ssize_t)sizeof m) drop_client();
  }
  for (int i = 0; i < 32; i++) if (infos[i] == bi) infos[i] = NULL;
  free(bi); b->user_data = NULL;
}

static void reg_global(void *d, struct wl_registry *r, uint32_t id, const char *iface, uint32_t ver)
{
  (void)d;
  if (!strcmp(iface, "wl_output") && !output) output = wl_registry_bind(r, id, &wl_output_interface, 1);
  else if (!strcmp(iface, "zkde_screencast_unstable_v1")) cast = wl_registry_bind(r, id, &zkde_screencast_unstable_v1_interface, ver < 3 ? ver : 3);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t id) { (void)d; (void)r; (void)id; }
static const struct wl_registry_listener reg_l = { reg_global, reg_remove };

static void s_created(void *d, struct zkde_screencast_stream_unstable_v1 *s, uint32_t n) { (void)d; (void)s; node = n; }
static void s_failed(void *d, struct zkde_screencast_stream_unstable_v1 *s, const char *e) { (void)d; (void)s; fprintf(stderr, "capture: KWin refused the stream: %s\n", e); stream_failed = 1; }
static void s_closed(void *d, struct zkde_screencast_stream_unstable_v1 *s) { (void)d; (void)s; stream_failed = 2; }
static const struct zkde_screencast_stream_unstable_v1_listener sl = { s_closed, s_created, s_failed };

static void on_param(void *d, uint32_t id, const struct spa_pod *param)
{
  (void)d;
  if (!param || id != SPA_PARAM_Format) return;
  spa_format_video_raw_parse(param, &vinfo);
  fprintf(stderr, "capture: format %u (BGRx=%u BGRA=%u) %ux%u\n", vinfo.format, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA, vinfo.size.width, vinfo.size.height);
  size_t need = FB_PIXELS + (size_t)vinfo.size.width * vinfo.size.height * 4;
  int fd = open(path, O_RDWR | O_CREAT, 0666);
  if (fd < 0 || ftruncate(fd, need)) { perror("capture: frame file"); pw_main_loop_quit(loop); return; }
  if (map) munmap(map, map_size);
  map = mmap(NULL, need, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); map_size = need; close(fd);
  struct fb_header *h = (struct fb_header *)map;
  h->width = vinfo.size.width; h->height = vinfo.size.height; h->stride = vinfo.size.width * 4; h->format = 5; h->magic = FB_MAGIC;
  uint8_t b[512]; struct spa_pod_builder pb = SPA_POD_BUILDER_INIT(b, sizeof b);
  const struct spa_pod *p = spa_pod_builder_add_object(&pb, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
    SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 8),
    SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemPtr) | (1 << SPA_DATA_MemFd)));
  pw_stream_update_params(stream, &p, 1);
}

static void on_process(void *d)
{
  (void)d;
  struct pw_buffer *b = pw_stream_dequeue_buffer(stream), *n;
  if (!b) return;
  while ((n = pw_stream_dequeue_buffer(stream))) { pw_stream_queue_buffer(stream, b); b = n; }
  struct spa_data *sd = &b->buffer->datas[0];
  if (map && sd->data && sd->chunk->size && !(sd->chunk->flags & SPA_CHUNK_FLAG_CORRUPTED)) {
    struct fb_header *h = (struct fb_header *)map;
    if (csock >= 0 && sd->type == SPA_DATA_MemFd && b->user_data) {
      if (!inflight) { if (send_frame(b) < 0) { pw_stream_queue_buffer(stream, b); drop_client(); return; } }
      else { if (pending) pw_stream_queue_buffer(stream, pending); pending = b; }
      __atomic_store_n(&h->seq, ++seq, __ATOMIC_RELEASE);
      count_frame();
      return;
    }
    uint32_t stride = sd->chunk->stride ? (uint32_t)sd->chunk->stride : h->width * 4;
    const uint8_t *src = (const uint8_t *)sd->data + sd->chunk->offset;
    for (uint32_t y = 0; y < h->height; y++) memcpy(map + FB_PIXELS + (size_t)y * h->stride, src + (size_t)y * stride, h->width * 4);
    __atomic_store_n(&h->seq, ++seq, __ATOMIC_RELEASE);
    copied_frames++;
    count_frame();
  }
  pw_stream_queue_buffer(stream, b);
}

static void on_state(void *d, enum pw_stream_state o, enum pw_stream_state n, const char *e)
{ (void)d; (void)o; fprintf(stderr, "capture: stream %s%s%s\n", pw_stream_state_as_string(n), e ? ": " : "", e ? e : ""); if (n == PW_STREAM_STATE_ERROR) pw_main_loop_quit(loop); }
static const struct pw_stream_events ev = { PW_VERSION_STREAM_EVENTS, .state_changed = on_state, .param_changed = on_param, .process = on_process, .add_buffer = on_add_buffer, .remove_buffer = on_remove_buffer };

int main(int argc, char **argv)
{
  if (argc < 2) { fprintf(stderr, "usage: capture FILE\n"); return 2; }
  path = argv[1];
  clock_gettime(CLOCK_MONOTONIC, &t0);
  dpy = wl_display_connect(NULL);
  if (!dpy) { fprintf(stderr, "capture: no Wayland display (%s)\n", getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "unset"); return 1; }
  struct wl_registry *r = wl_display_get_registry(dpy);
  wl_registry_add_listener(r, &reg_l, NULL);
  wl_display_roundtrip(dpy);
  if (!output || !cast) { fprintf(stderr, "capture: KWin offers no %s\n", !output ? "wl_output" : "zkde_screencast"); return 1; }
  struct zkde_screencast_stream_unstable_v1 *s = zkde_screencast_unstable_v1_stream_output(cast, output, ZKDE_SCREENCAST_UNSTABLE_V1_POINTER_EMBEDDED);
  zkde_screencast_stream_unstable_v1_add_listener(s, &sl, NULL);
  for (int i = 0; i < 50 && !node && !stream_failed; i++) wl_display_roundtrip(dpy);
  if (!node) { fprintf(stderr, "capture: no PipeWire node from KWin (%u)\n", stream_failed); return 1; }
  fprintf(stderr, "capture: KWin's stream is PipeWire node %u\n", node);
  pw_init(&argc, &argv);
  loop = pw_main_loop_new(NULL);
  if (!getenv("X400_NOFD")) {
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    if (snprintf(sa.sun_path, sizeof sa.sun_path, "%s.sock", path) < (int)sizeof sa.sun_path) {
      unlink(sa.sun_path);
      lsock = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
      if (lsock >= 0 && (bind(lsock, (struct sockaddr *)&sa, sizeof sa) || listen(lsock, 1))) { perror("capture: socket"); close(lsock); lsock = -1; }
      if (lsock >= 0) { lsrc = pw_loop_add_io(pw_main_loop_get_loop(loop), lsock, SPA_IO_IN, false, on_accept, NULL); fprintf(stderr, "capture: listening on %s\n", sa.sun_path); }
    }
  }
  stream = pw_stream_new_simple(pw_main_loop_get_loop(loop), "x400-capture", pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Screen", NULL), &ev, NULL);
  uint8_t b[1024]; struct spa_pod_builder pb = SPA_POD_BUILDER_INIT(b, sizeof b);
  const struct spa_pod *p = spa_pod_builder_add_object(&pb, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
    SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
    SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(3, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA),
    SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&SPA_RECTANGLE(2112, 1320), &SPA_RECTANGLE(1, 1), &SPA_RECTANGLE(8192, 8192)),
    SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&SPA_FRACTION(60, 1), &SPA_FRACTION(0, 1), &SPA_FRACTION(1000, 1)));
  if (pw_stream_connect(stream, PW_DIRECTION_INPUT, node, PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS, &p, 1) < 0) { fprintf(stderr, "capture: pw_stream_connect failed\n"); return 1; }
  pw_main_loop_run(loop);
  pw_stream_destroy(stream); pw_main_loop_destroy(loop);
  return 0;
}
