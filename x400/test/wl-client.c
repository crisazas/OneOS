// linux-x400: a Wayland client that draws as little as possible, to separate what the compositor and the capture chain deliver from what a heavy client costs.
// It opens an xdg_toplevel with wl_shm buffers (no GPU, no Zink, no Venus), paints a bar that moves one step per frame, and asks for the next frame with a wl_surface frame
// callback, so it runs at whatever rate KWin gives it: its own count of callbacks per second is the rate the compositor really offers. Prints that every second.
//   mode "shm"  (default): the window is W x H wl_shm pixels, updated entirely each frame (a memset of a bar and a fill)
//   X400_WLC_SIZE=WxH sets the window size (default 400x300), X400_WLC_SECS the run time (default 12), X400_WLC_FULL=1 asks for fullscreen
// Build in the guest rootfs's sysroot like capture.c (needs wayland-client, xdg-shell protocol code generated with wayland-scanner):
//   wayland-scanner client-header /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml xdg-shell-client-protocol.h ; wayland-scanner private-code ... xdg-shell-protocol.c
#define _GNU_SOURCE
#include "xdg-shell-client-protocol.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

static struct wl_compositor *comp; static struct wl_shm *shm; static struct xdg_wm_base *wm;
static struct wl_surface *surf; static struct xdg_surface *xs; static struct xdg_toplevel *top;
static int W = 400, H = 300, configured, running = 1, full;
static struct { struct wl_buffer *buf; uint32_t *px; int busy; } bufs[3];
static unsigned long frames, drawn, skipped; static double t0, tlast, worst; static uint32_t step;

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

static void rel(void *d, struct wl_buffer *b) { (void)b; *(int *)d = 0; }
static const struct wl_buffer_listener bl = { rel };

static int make_buffers(void)
{
  size_t sz = (size_t)W * H * 4;
  for (int i = 0; i < 3; i++) {
    int fd = memfd_create("wlc", MFD_CLOEXEC); if (fd < 0 || ftruncate(fd, sz)) return -1;
    bufs[i].px = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); if (bufs[i].px == MAP_FAILED) return -1;
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, sz); bufs[i].buf = wl_shm_pool_create_buffer(pool, 0, W, H, W * 4, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool); close(fd); wl_buffer_add_listener(bufs[i].buf, &bl, &bufs[i].busy);
  }
  return 0;
}

static void draw(void);
static void frame_done(void *d, struct wl_callback *cb, uint32_t time) { (void)d; (void)time; wl_callback_destroy(cb); frames++; draw(); }
static const struct wl_callback_listener fl = { frame_done };

static void draw(void)
{
  int i = 0; while (i < 3 && bufs[i].busy) i++;
  if (i == 3) { skipped++; struct wl_callback *cb = wl_surface_frame(surf); wl_callback_add_listener(cb, &fl, NULL); wl_surface_commit(surf); return; }
  uint32_t *p = bufs[i].px; int bar = (int)((step++ * 7) % (uint32_t)(W - 40));
  for (int y = 0; y < H; y++) { uint32_t *r = p + (size_t)y * W; for (int x = 0; x < W; x++) r[x] = (x >= bar && x < bar + 40) ? 0xffffffffu : 0xff203040u + (uint32_t)y; }
  bufs[i].busy = 1; drawn++;
  wl_surface_attach(surf, bufs[i].buf, 0, 0); wl_surface_damage_buffer(surf, 0, 0, W, H);
  struct wl_callback *cb = wl_surface_frame(surf); wl_callback_add_listener(cb, &fl, NULL);
  wl_surface_commit(surf);
  double t = now(); if (tlast > 0 && t - tlast > worst) worst = t - tlast; tlast = t;
}

static void reg(void *d, struct wl_registry *r, uint32_t id, const char *iface, uint32_t ver)
{
  (void)d;
  if (!strcmp(iface, "wl_compositor")) comp = wl_registry_bind(r, id, &wl_compositor_interface, 4);
  else if (!strcmp(iface, "wl_shm")) shm = wl_registry_bind(r, id, &wl_shm_interface, 1);
  else if (!strcmp(iface, "xdg_wm_base")) wm = wl_registry_bind(r, id, &xdg_wm_base_interface, ver < 2 ? ver : 2);
}
static void reg_rm(void *d, struct wl_registry *r, uint32_t id) { (void)d; (void)r; (void)id; }
static const struct wl_registry_listener rl = { reg, reg_rm };
static void ping(void *d, struct xdg_wm_base *w, uint32_t s) { (void)d; xdg_wm_base_pong(w, s); }
static const struct xdg_wm_base_listener wl_ = { ping };
static void xs_conf(void *d, struct xdg_surface *x, uint32_t s) { (void)d; xdg_surface_ack_configure(x, s); configured = 1; }
static const struct xdg_surface_listener xl = { xs_conf };
static void tp_conf(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *st) { (void)d; (void)t; (void)w; (void)h; (void)st; }
static void tp_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; running = 0; }
static const struct xdg_toplevel_listener tl = { tp_conf, tp_close };

int main(void)
{
  if (getenv("X400_WLC_SIZE")) sscanf(getenv("X400_WLC_SIZE"), "%dx%d", &W, &H);
  double secs = getenv("X400_WLC_SECS") ? atof(getenv("X400_WLC_SECS")) : 12; full = getenv("X400_WLC_FULL") != NULL;
  struct wl_display *dpy = wl_display_connect(NULL); if (!dpy) { fprintf(stderr, "wlc: no display\n"); return 1; }
  struct wl_registry *r = wl_display_get_registry(dpy); wl_registry_add_listener(r, &rl, NULL); wl_display_roundtrip(dpy);
  if (!comp || !shm || !wm) { fprintf(stderr, "wlc: missing globals\n"); return 1; }
  xdg_wm_base_add_listener(wm, &wl_, NULL);
  surf = wl_compositor_create_surface(comp); xs = xdg_wm_base_get_xdg_surface(wm, surf); xdg_surface_add_listener(xs, &xl, NULL);
  top = xdg_surface_get_toplevel(xs); xdg_toplevel_add_listener(top, &tl, NULL); xdg_toplevel_set_title(top, "x400 wl-client"); if (full) xdg_toplevel_set_fullscreen(top, NULL);
  wl_surface_commit(surf);
  while (!configured && wl_display_dispatch(dpy) >= 0) {}
  if (make_buffers()) { fprintf(stderr, "wlc: buffers: %s\n", strerror(errno)); return 1; }
  fprintf(stderr, "wlc: window %dx%d%s, %.0f s\n", W, H, full ? " fullscreen" : "", secs);
  t0 = now(); double tp = t0; unsigned long fp = 0;
  draw();
  while (running && now() - t0 < secs && wl_display_dispatch(dpy) >= 0) {
    double t = now();
    if (t - tp >= 1.0) { fprintf(stderr, "wlc: %.1f fps (callbacks) in the last %.1f s; drawn %lu skipped %lu\n", (frames - fp) / (t - tp), t - tp, drawn, skipped); tp = t; fp = frames; }
  }
  double dt = now() - t0;
  fprintf(stderr, "wlc: RESULT %lu frame callbacks in %.1f s = %.1f fps; drawn %lu, skipped (no free buffer) %lu; worst gap between two draws %.1f ms\n", frames, dt, frames / dt, drawn, skipped, worst * 1000);
  return 0;
}
