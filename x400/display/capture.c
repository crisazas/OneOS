// linux-x400: asks KWin (zkde_screencast_unstable_v1) for a PipeWire stream of its first output and hands every frame to the
// display helper that shows it on the tablet. Runs inside the chroot.
// With the helper connected to FILE.sock, a frame is not copied: the PipeWire buffer KWin allocated is kept and its descriptor is sent to the
// helper (SCM_RIGHTS), which shows it and answers when done. Without the helper the frame is copied into the frame file FILE (fb.h), as before.
// FILE's seq counts the frames in both modes.
// The buffers are linear BGRA dma-bufs (KWin renders into them with the GPU: no read-back to the CPU) unless X400_FRAMES=copy, which asks for
// ordinary shared memory; if KWin cannot give dma-bufs it falls back to shared memory by itself.
// Usage: capture FILE   (env WAYLAND_DISPLAY, XDG_RUNTIME_DIR; PipeWire must be running; X400_NOFD=1 never uses the socket;
//                        X400_RACECHECK=1 compares each dma-buf frame when it arrives with the same buffer 15 ms later, to see whether KWin's GPU
//                        work was still running: a test mode that slows the stream down)
#define _GNU_SOURCE
#include "fb.h"
#include "zkde-screencast-unstable-v1-client-protocol.h"
#include "fdframe.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/props.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
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

struct bufinfo { uint32_t id; int sent; uint8_t *cpumap; size_t cpusize; };
static struct bufinfo *infos[32]; static uint32_t next_id = 1;
static int lsock = -1, csock = -1;
static struct spa_source *lsrc, *csrc;
static struct pw_buffer *inflight, *pending;
static uint64_t sent_frames, copied_frames, raced, race_checked, replaced, arrived;
static int negotiated_dmabuf, racecheck;
static uint32_t out_transform;   // wl_output transform of KWin's output (0 normal, 1 = 90, 2 = 180, 3 = 270): sent with every frame, the helper turns the layer by it

static void count_frame(void)
{
  if (++frames % 120 == 0) {
    struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
    double dt = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;
    fprintf(stderr, "capture: %llu frames, %.1f fps over the last %.1f s (%llu by descriptor, %llu copied; %s)", (unsigned long long)frames, 120 / dt, dt,
            (unsigned long long)sent_frames, (unsigned long long)copied_frames, negotiated_dmabuf ? "dma-buf" : "shared memory");
    if (sent_frames) fprintf(stderr, "; from KWin %llu, replaced while the helper had one %llu", (unsigned long long)arrived, (unsigned long long)replaced);
    if (racecheck) fprintf(stderr, "; race check: %llu of %llu frames still changing 15 ms after they arrived", (unsigned long long)raced, (unsigned long long)race_checked);
    fputc('\n', stderr);
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

static void dmabuf_sync(int fd, int start) { struct dma_buf_sync sy = { .flags = (start ? DMA_BUF_SYNC_START : DMA_BUF_SYNC_END) | DMA_BUF_SYNC_READ }; ioctl(fd, DMA_BUF_IOCTL_SYNC, &sy); }

// a dma-buf buffer mapped for the CPU, kept for as long as the buffer lives (the fallback copy and the race check read it)
static const uint8_t *dmabuf_map(struct pw_buffer *b)
{
  struct bufinfo *bi = b->user_data; struct spa_data *sd = &b->buffer->datas[0];
  if (!bi) return NULL;
  if (!bi->cpumap) {
    void *m = mmap(NULL, sd->maxsize, PROT_READ, MAP_SHARED, (int)sd->fd, 0);
    if (m == MAP_FAILED) { fprintf(stderr, "capture: cannot map a dma-buf: %s\n", strerror(errno)); return NULL; }
    bi->cpumap = m; bi->cpusize = sd->maxsize;
  }
  return bi->cpumap + sd->mapoffset;
}

static uint64_t sample_hash(const uint8_t *p, uint32_t stride, uint32_t rows)
{
  uint64_t h = 1469598103934665603ull;
  for (uint32_t y = 0; y < rows; y += 29) for (uint32_t x = 0; x + 4 <= stride; x += 92) { uint32_t v; memcpy(&v, p + (size_t)y * stride + x, 4); h = (h ^ v) * 1099511628211ull; }
  return h;
}

static int send_frame(struct pw_buffer *b)
{
  struct spa_data *sd = &b->buffer->datas[0]; struct bufinfo *bi = b->user_data;
  struct fb_header *h = (struct fb_header *)map;
  struct fd_msg m = { .type = FDM_FRAME, .id = bi->id, .has_fd = !bi->sent, .width = vinfo.size.width, .height = vinfo.size.height,
                      .stride = sd->chunk->stride ? (uint32_t)sd->chunk->stride : vinfo.size.width * 4, .offset = sd->chunk->offset,
                      .mapoffset = sd->mapoffset, .maxsize = sd->maxsize, .flags = (sd->type == SPA_DATA_DmaBuf ? FDF_DMABUF : 0) | (out_transform & FDF_ROT_MASK) << FDF_ROT_SHIFT, .seq = h ? h->seq : 0 };
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
  if (bi->cpumap) munmap(bi->cpumap, bi->cpusize);
  if (csock >= 0 && bi->sent) {
    struct fd_msg m = { .type = FDM_DROP, .id = bi->id };
    if (send(csock, &m, sizeof m, MSG_NOSIGNAL) != (ssize_t)sizeof m) drop_client();
  }
  for (int i = 0; i < 32; i++) if (infos[i] == bi) infos[i] = NULL;
  free(bi); b->user_data = NULL;
}

static void o_geometry(void *d, struct wl_output *o, int32_t x, int32_t y, int32_t pw, int32_t ph, int32_t sub, const char *make, const char *model, int32_t tr)
{ (void)d; (void)o; (void)x; (void)y; (void)pw; (void)ph; (void)sub; (void)make; (void)model; if ((uint32_t)tr != out_transform) fprintf(stderr, "capture: output transform %d\n", tr); out_transform = (uint32_t)tr & 7; }
static void o_mode(void *d, struct wl_output *o, uint32_t f, int32_t w, int32_t h, int32_t r) { (void)d; (void)o; (void)f; (void)w; (void)h; (void)r; }
static void o_done(void *d, struct wl_output *o) { (void)d; (void)o; }
static void o_scale(void *d, struct wl_output *o, int32_t f) { (void)d; (void)o; (void)f; }
static const struct wl_output_listener out_l = { o_geometry, o_mode, o_done, o_scale };

static void on_wayland(void *d, int fd, uint32_t mask)
{
  (void)d; (void)fd;
  if (mask & (SPA_IO_ERR | SPA_IO_HUP) || wl_display_dispatch(dpy) < 0) { fprintf(stderr, "capture: KWin went away\n"); pw_main_loop_quit(loop); }
}

static void reg_global(void *d, struct wl_registry *r, uint32_t id, const char *iface, uint32_t ver)
{
  (void)d;
  if (!strcmp(iface, "wl_output") && !output) { output = wl_registry_bind(r, id, &wl_output_interface, 2); wl_output_add_listener(output, &out_l, NULL); }
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
  const struct spa_pod_prop *mp = spa_pod_find_prop(param, NULL, SPA_FORMAT_VIDEO_modifier);
  fprintf(stderr, "capture: format %u (BGRx=%u BGRA=%u) %ux%u%s, framerate %u/%u, max framerate %u/%u\n", vinfo.format, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA, vinfo.size.width, vinfo.size.height,
          mp ? ", dma-buf" : ", shared memory", vinfo.framerate.num, vinfo.framerate.denom, vinfo.max_framerate.num, vinfo.max_framerate.denom);
  // with a dma-buf KWin first sends the modifiers it can still pick from, then the one it chose: the buffers are asked for on the second
  if (mp && (mp->flags & SPA_POD_PROP_FLAG_DONT_FIXATE) && SPA_POD_CHOICE_N_VALUES(&mp->value) > 1) return;
  negotiated_dmabuf = mp != NULL;
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
    SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(mp ? (1 << SPA_DATA_DmaBuf) : ((1 << SPA_DATA_MemPtr) | (1 << SPA_DATA_MemFd))));
  pw_stream_update_params(stream, &p, 1);
}

static void on_process(void *d)
{
  (void)d;
  struct pw_buffer *b = pw_stream_dequeue_buffer(stream), *n;
  if (!b) return;
  while ((n = pw_stream_dequeue_buffer(stream))) { pw_stream_queue_buffer(stream, b); b = n; }
  struct spa_data *sd = &b->buffer->datas[0];
  int dmabuf = sd->type == SPA_DATA_DmaBuf;
  arrived++;
  if (map && sd->chunk->size && !(sd->chunk->flags & SPA_CHUNK_FLAG_CORRUPTED) && (sd->data || dmabuf)) {
    struct fb_header *h = (struct fb_header *)map;
    if (racecheck && dmabuf) {
      const uint8_t *p = dmabuf_map(b);
      if (p) {
        uint32_t st = sd->chunk->stride ? (uint32_t)sd->chunk->stride : h->width * 4;
        dmabuf_sync((int)sd->fd, 1); uint64_t h0 = sample_hash(p + sd->chunk->offset, st, h->height); dmabuf_sync((int)sd->fd, 0);
        usleep(15000);
        dmabuf_sync((int)sd->fd, 1); uint64_t h1 = sample_hash(p + sd->chunk->offset, st, h->height); dmabuf_sync((int)sd->fd, 0);
        race_checked++; if (h0 != h1) raced++;
      }
    }
    if (csock >= 0 && (sd->type == SPA_DATA_MemFd || dmabuf) && b->user_data) {
      if (!inflight) { if (send_frame(b) < 0) { pw_stream_queue_buffer(stream, b); drop_client(); return; } }
      else { if (pending) { pw_stream_queue_buffer(stream, pending); replaced++; } pending = b; }
      __atomic_store_n(&h->seq, ++seq, __ATOMIC_RELEASE);
      count_frame();
      return;
    }
    uint32_t stride = sd->chunk->stride ? (uint32_t)sd->chunk->stride : h->width * 4;
    const uint8_t *src = dmabuf ? dmabuf_map(b) : (const uint8_t *)sd->data;
    if (src) {
      src += sd->chunk->offset;
      if (dmabuf) dmabuf_sync((int)sd->fd, 1);
      for (uint32_t y = 0; y < h->height; y++) memcpy(map + FB_PIXELS + (size_t)y * h->stride, src + (size_t)y * stride, h->width * 4);
      if (dmabuf) dmabuf_sync((int)sd->fd, 0);
      __atomic_store_n(&h->seq, ++seq, __ATOMIC_RELEASE);
      copied_frames++;
      count_frame();
    }
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
  racecheck = getenv("X400_RACECHECK") != NULL;
  pw_loop_add_io(pw_main_loop_get_loop(loop), wl_display_get_fd(dpy), SPA_IO_IN | SPA_IO_ERR | SPA_IO_HUP, false, on_wayland, NULL);
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
  uint8_t b[2048]; struct spa_pod_builder pb = SPA_POD_BUILDER_INIT(b, sizeof b);
  const struct spa_pod *params[2]; int np = 0;
  const char *fr = getenv("X400_FRAMES");
  if (!fr || strcmp(fr, "copy")) {
    // first choice: linear BGRA dma-bufs (the only modifier this driver has for BGRA; KWin renders into them with the GPU)
    struct spa_pod_frame f[2];
    spa_pod_builder_push_object(&pb, &f[0], SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
    spa_pod_builder_add(&pb, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
      SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(3, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA),
      SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&SPA_RECTANGLE(2112, 1320), &SPA_RECTANGLE(1, 1), &SPA_RECTANGLE(8192, 8192)),
      SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&SPA_FRACTION(60, 1), &SPA_FRACTION(0, 1), &SPA_FRACTION(1000, 1)),
      SPA_FORMAT_VIDEO_maxFramerate, SPA_POD_Fraction(&SPA_FRACTION(0, 1)), 0);
    spa_pod_builder_prop(&pb, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
    spa_pod_builder_push_choice(&pb, &f[1], SPA_CHOICE_Enum, 0);
    spa_pod_builder_long(&pb, 0 /* DRM_FORMAT_MOD_LINEAR */); spa_pod_builder_long(&pb, 0);
    spa_pod_builder_pop(&pb, &f[1]);
    params[np++] = spa_pod_builder_pop(&pb, &f[0]);
  }
  params[np++] = spa_pod_builder_add_object(&pb, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
    SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
    SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(3, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA),
    SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&SPA_RECTANGLE(2112, 1320), &SPA_RECTANGLE(1, 1), &SPA_RECTANGLE(8192, 8192)),
    SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&SPA_FRACTION(60, 1), &SPA_FRACTION(0, 1), &SPA_FRACTION(1000, 1)),
    SPA_FORMAT_VIDEO_maxFramerate, SPA_POD_Fraction(&SPA_FRACTION(0, 1)));
  if (pw_stream_connect(stream, PW_DIRECTION_INPUT, node, PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS, params, np) < 0) { fprintf(stderr, "capture: pw_stream_connect failed\n"); return 1; }
  pw_main_loop_run(loop);
  pw_stream_destroy(stream); pw_main_loop_destroy(loop);
  return 0;
}
