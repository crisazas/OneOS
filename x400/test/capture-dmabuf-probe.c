// linux-x400: does KWin hand a screencast client a dma-buf (BGRA, DRM_FORMAT_MOD_LINEAR) on this stack? Stage C of removing the CPU copy.
// Asks KWin for a PipeWire stream of its first output, offering ONLY dma-buf BGRx/BGRA with the LINEAR modifier (and, with PROBE_FALLBACK=1, the
// ordinary memory formats too), logs what is negotiated, what the buffers are (type, fd, size, stride, offset, flags), and counts frames and
// the time between them. It never copies pixels. For each frame it can also check the dma-buf is readable: PROBE_READ=1 mmaps it and sums a few rows.
// Usage: capture-dmabuf-probe [SECONDS]   (env WAYLAND_DISPLAY, XDG_RUNTIME_DIR; PipeWire running; PROBE_FALLBACK, PROBE_READ)
// Build (needs the PipeWire/Wayland headers of the rootfs): see x400/test/README or the sysroot command used for x400/display/capture.c.
#define _GNU_SOURCE
#include "zkde-screencast-unstable-v1-client-protocol.h"
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
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#ifndef DRM_FORMAT_MOD_LINEAR
#define DRM_FORMAT_MOD_LINEAR 0ULL
#endif
#define DRM_FORMAT_MOD_INVALID 0x00ffffffffffffffULL

static struct wl_output *output;
static struct zkde_screencast_unstable_v1 *cast;
static uint32_t node, stream_failed;
static struct pw_main_loop *loop;
static struct pw_stream *stream;
static struct spa_video_info_raw vinfo;
static int use_fallback, do_read;
static unsigned long frames, corrupted, empty;
static double t_first, t_prev, max_gap, sum_gap;
static struct timespec t0;

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static void reg_global(void *d, struct wl_registry *r, uint32_t id, const char *iface, uint32_t ver)
{
  (void)d;
  if (!strcmp(iface, "wl_output") && !output) output = wl_registry_bind(r, id, &wl_output_interface, 1);
  else if (!strcmp(iface, "zkde_screencast_unstable_v1")) cast = wl_registry_bind(r, id, &zkde_screencast_unstable_v1_interface, ver < 3 ? ver : 3);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t id) { (void)d; (void)r; (void)id; }
static const struct wl_registry_listener reg_l = { reg_global, reg_remove };
static void s_created(void *d, struct zkde_screencast_stream_unstable_v1 *s, uint32_t n) { (void)d; (void)s; node = n; }
static void s_failed(void *d, struct zkde_screencast_stream_unstable_v1 *s, const char *e) { (void)d; (void)s; fprintf(stderr, "probe: KWin refused the stream: %s\n", e); stream_failed = 1; }
static void s_closed(void *d, struct zkde_screencast_stream_unstable_v1 *s) { (void)d; (void)s; stream_failed = 2; }
static const struct zkde_screencast_stream_unstable_v1_listener sl = { s_closed, s_created, s_failed };

static void on_param(void *d, uint32_t id, const struct spa_pod *param)
{
  (void)d;
  if (!param || id != SPA_PARAM_Format) return;
  spa_format_video_raw_parse(param, &vinfo);
  const struct spa_pod_prop *mp = spa_pod_find_prop(param, NULL, SPA_FORMAT_VIDEO_modifier);
  uint64_t mod = 0; int hasmod = 0;
  if (mp) { hasmod = 1; uint32_t n; uint64_t *v = (uint64_t *)SPA_POD_CHOICE_VALUES(&mp->value); n = SPA_POD_CHOICE_N_VALUES(&mp->value); mod = n ? v[0] : 0;
            fprintf(stderr, "probe: format negotiated: spa format %u (BGRx=%u BGRA=%u) %ux%u, modifier property present, %u value(s), first 0x%llx%s\n", vinfo.format, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA,
                    vinfo.size.width, vinfo.size.height, n, (unsigned long long)mod, (spa_pod_find_prop(param, NULL, SPA_FORMAT_VIDEO_modifier)->flags & SPA_POD_PROP_FLAG_DONT_FIXATE) ? " (DONT_FIXATE: KWin still has to pick one)" : " (fixated)"); }
  else fprintf(stderr, "probe: format negotiated: spa format %u %ux%u, NO modifier property (KWin chose shared memory)\n", vinfo.format, vinfo.size.width, vinfo.size.height);
  (void)hasmod;
  if (mp && SPA_POD_CHOICE_N_VALUES(&mp->value) > 1 && (mp->flags & SPA_POD_PROP_FLAG_DONT_FIXATE)) return;  /* wait for the fixated one */
  uint8_t b[1024]; struct spa_pod_builder pb = SPA_POD_BUILDER_INIT(b, sizeof b);
  const struct spa_pod *p = spa_pod_builder_add_object(&pb, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
    SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 8),
    SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(mp ? (1 << SPA_DATA_DmaBuf) : ((1 << SPA_DATA_MemPtr) | (1 << SPA_DATA_MemFd))));
  pw_stream_update_params(stream, &p, 1);
  fprintf(stderr, "probe: asked for buffers of type %s\n", mp ? "DmaBuf" : "MemPtr|MemFd");
}

static void on_add_buffer(void *d, struct pw_buffer *b)
{
  (void)d;
  struct spa_buffer *sb = b->buffer;
  fprintf(stderr, "probe: buffer added: %u data block(s), %u meta\n", sb->n_datas, sb->n_metas);
  for (uint32_t i = 0; i < sb->n_datas; i++) {
    struct spa_data *sd = &sb->datas[i];
    fprintf(stderr, "probe:   block %u: type %u (MemPtr=%u MemFd=%u DmaBuf=%u SyncObj=%u), fd %lld, maxsize %u, mapoffset %u, flags 0x%x, chunk offset %u size %u stride %d\n", i, sd->type,
            SPA_DATA_MemPtr, SPA_DATA_MemFd, SPA_DATA_DmaBuf, SPA_DATA_SyncObj, (long long)sd->fd, sd->maxsize, sd->mapoffset, sd->flags, sd->chunk->offset, sd->chunk->size, sd->chunk->stride);
  }
}

static void on_process(void *d)
{
  (void)d;
  struct pw_buffer *b = pw_stream_dequeue_buffer(stream);
  if (!b) return;
  struct spa_data *sd = &b->buffer->datas[0];
  double t = now();
  if (frames == 0) t_first = t; else { double g = t - t_prev; sum_gap += g; if (g > max_gap) max_gap = g; }
  t_prev = t; frames++;
  if (sd->chunk->flags & SPA_CHUNK_FLAG_CORRUPTED) corrupted++;
  if (!sd->chunk->size) empty++;
  if (do_read && sd->type == SPA_DATA_DmaBuf && sd->chunk->size) {
    struct dma_buf_sync s = { DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ };
    ioctl((int)sd->fd, DMA_BUF_IOCTL_SYNC, &s);
    size_t len = (size_t)sd->maxsize; uint8_t *m = mmap(NULL, len, PROT_READ, MAP_SHARED, (int)sd->fd, 0);
    if (m == MAP_FAILED) { if (frames < 4) fprintf(stderr, "probe: mmap of the dma-buf failed: %s\n", strerror(errno)); }
    else {
      unsigned long sum = 0; const uint8_t *src = m + sd->chunk->offset;
      for (uint32_t y = 0; y < vinfo.size.height; y += 97) for (uint32_t x = 0; x < (uint32_t)sd->chunk->stride; x += 61) sum += src[(size_t)y * sd->chunk->stride + x];
      if (frames < 4 || frames % 200 == 0) fprintf(stderr, "probe: frame %lu read through the dma-buf mapping, sample sum %lu\n", frames, sum);
      munmap(m, len);
    }
    s.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ; ioctl((int)sd->fd, DMA_BUF_IOCTL_SYNC, &s);
  }
  if (frames < 4 || frames % 200 == 0) fprintf(stderr, "probe: frame %lu type %u fd %lld chunk size %u stride %d flags 0x%x\n", frames, sd->type, (long long)sd->fd, sd->chunk->size, sd->chunk->stride, sd->chunk->flags);
  pw_stream_queue_buffer(stream, b);
}

static void on_state(void *d, enum pw_stream_state o, enum pw_stream_state n, const char *e)
{ (void)d; (void)o; fprintf(stderr, "probe: stream %s%s%s\n", pw_stream_state_as_string(n), e ? ": " : "", e ? e : ""); if (n == PW_STREAM_STATE_ERROR) pw_main_loop_quit(loop); }
static const struct pw_stream_events ev = { PW_VERSION_STREAM_EVENTS, .state_changed = on_state, .param_changed = on_param, .process = on_process, .add_buffer = on_add_buffer };
static void on_timeout(void *d, uint64_t e) { (void)d; (void)e; pw_main_loop_quit(loop); }

int main(int argc, char **argv)
{
  double secs = argc > 1 ? atof(argv[1]) : 10;
  use_fallback = getenv("PROBE_FALLBACK") != NULL; do_read = getenv("PROBE_READ") != NULL;
  struct wl_display *dpy = wl_display_connect(NULL);
  if (!dpy) { fprintf(stderr, "probe: no Wayland display\n"); return 1; }
  struct wl_registry *r = wl_display_get_registry(dpy);
  wl_registry_add_listener(r, &reg_l, NULL); wl_display_roundtrip(dpy);
  if (!output || !cast) { fprintf(stderr, "probe: no wl_output / zkde_screencast\n"); return 1; }
  struct zkde_screencast_stream_unstable_v1 *s = zkde_screencast_unstable_v1_stream_output(cast, output, ZKDE_SCREENCAST_UNSTABLE_V1_POINTER_EMBEDDED);
  zkde_screencast_stream_unstable_v1_add_listener(s, &sl, NULL);
  for (int i = 0; i < 50 && !node && !stream_failed; i++) wl_display_roundtrip(dpy);
  if (!node) { fprintf(stderr, "probe: no PipeWire node (%u)\n", stream_failed); return 1; }
  fprintf(stderr, "probe: node %u, fallback to shared memory %s, reading the dma-buf %s\n", node, use_fallback ? "offered" : "NOT offered", do_read ? "yes" : "no");
  pw_init(&argc, &argv);
  loop = pw_main_loop_new(NULL);
  stream = pw_stream_new_simple(pw_main_loop_get_loop(loop), "x400-dmabuf-probe", pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Screen", NULL), &ev, NULL);
  uint8_t b[2048]; struct spa_pod_builder pb = SPA_POD_BUILDER_INIT(b, sizeof b);
  const struct spa_pod *params[2]; int np = 0;
  uint64_t mods[1] = { DRM_FORMAT_MOD_LINEAR };
  struct spa_pod_frame f[2];
  spa_pod_builder_push_object(&pb, &f[0], SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
  spa_pod_builder_add(&pb, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
    SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(3, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA),
    SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&SPA_RECTANGLE(2112, 1320), &SPA_RECTANGLE(1, 1), &SPA_RECTANGLE(8192, 8192)),
    SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&SPA_FRACTION(60, 1), &SPA_FRACTION(0, 1), &SPA_FRACTION(1000, 1)), 0);
  spa_pod_builder_prop(&pb, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
  spa_pod_builder_push_choice(&pb, &f[1], SPA_CHOICE_Enum, 0);
  spa_pod_builder_long(&pb, (int64_t)mods[0]); spa_pod_builder_long(&pb, (int64_t)mods[0]);
  spa_pod_builder_pop(&pb, &f[1]);
  params[np++] = spa_pod_builder_pop(&pb, &f[0]);
  if (use_fallback) {
    params[np++] = spa_pod_builder_add_object(&pb, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
      SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(3, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA),
      SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&SPA_RECTANGLE(2112, 1320), &SPA_RECTANGLE(1, 1), &SPA_RECTANGLE(8192, 8192)),
      SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&SPA_FRACTION(60, 1), &SPA_FRACTION(0, 1), &SPA_FRACTION(1000, 1)));
  }
  if (pw_stream_connect(stream, PW_DIRECTION_INPUT, node, PW_STREAM_FLAG_AUTOCONNECT, params, np) < 0) { fprintf(stderr, "probe: pw_stream_connect failed\n"); return 1; }
  struct spa_source *tm = pw_loop_add_timer(pw_main_loop_get_loop(loop), on_timeout, NULL);
  struct timespec to = { (time_t)secs, (long)((secs - (time_t)secs) * 1e9) };
  pw_loop_update_timer(pw_main_loop_get_loop(loop), tm, &to, NULL, false);
  clock_gettime(CLOCK_MONOTONIC, &t0);
  pw_main_loop_run(loop);
  double dur = frames > 1 ? t_prev - t_first : 0;
  fprintf(stderr, "probe: RESULT %lu frames in %.1f s%s; corrupted %lu, empty %lu; mean gap %.1f ms, max gap %.1f ms\n", frames, dur, dur > 0 ? "" : " (none or one)", corrupted, empty,
          frames > 1 ? sum_gap / (frames - 1) * 1000 : 0, max_gap * 1000);
  pw_stream_destroy(stream); pw_main_loop_destroy(loop);
  return 0;
}
