// linux-x400: asks KWin (zkde_screencast_unstable_v1) for a PipeWire stream of its first output and copies every frame
// into the frame file (fb.h) that the display helper shows on the tablet. Runs inside the chroot.
// Usage: capture FILE   (env WAYLAND_DISPLAY, XDG_RUNTIME_DIR; PipeWire must be running)
#define _GNU_SOURCE
#include "fb.h"
#include "zkde-screencast-unstable-v1-client-protocol.h"
#include <fcntl.h>
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/props.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
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
  struct pw_buffer *b = pw_stream_dequeue_buffer(stream);
  if (!b) return;
  struct spa_data *sd = &b->buffer->datas[0];
  if (map && sd->data && sd->chunk->size) {
    struct fb_header *h = (struct fb_header *)map;
    uint32_t stride = sd->chunk->stride ? (uint32_t)sd->chunk->stride : h->width * 4;
    const uint8_t *src = (const uint8_t *)sd->data + sd->chunk->offset;
    for (uint32_t y = 0; y < h->height; y++) memcpy(map + FB_PIXELS + (size_t)y * h->stride, src + (size_t)y * stride, h->width * 4);
    __atomic_store_n(&h->seq, ++seq, __ATOMIC_RELEASE);
    if (++frames % 120 == 0) {
      struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
      double dt = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;
      fprintf(stderr, "capture: %llu frames, %.1f fps over the last %.1f s\n", (unsigned long long)frames, 120 / dt, dt);
      t0 = t1;
    }
  }
  pw_stream_queue_buffer(stream, b);
}
static void on_state(void *d, enum pw_stream_state o, enum pw_stream_state n, const char *e)
{ (void)d; (void)o; fprintf(stderr, "capture: stream %s%s%s\n", pw_stream_state_as_string(n), e ? ": " : "", e ? e : ""); if (n == PW_STREAM_STATE_ERROR) pw_main_loop_quit(loop); }
static const struct pw_stream_events ev = { PW_VERSION_STREAM_EVENTS, .state_changed = on_state, .param_changed = on_param, .process = on_process };

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
