// linux-x400: a fullscreen Wayland client that prints what KWin hands it: the surface size, each touch point and pointer motion in surface coordinates, and, when KWin offers
// zwp_tablet_manager_v2, each pen tool's proximity, motion, pressure, tilt, down/up. Used to check touch, trackpad and pen after a rotation without a hand on the real glass:
// feed inputd (uinput devices) and read what lands here. Output: one line per event on stdout, a summary every X400_IP_SECS seconds (default 20).
// Build like wl-client.c (xdg-shell code from wayland-scanner) plus tablet-unstable-v2: wayland-scanner of /usr/share/wayland-protocols/unstable/tablet/tablet-unstable-v2.xml.
#define _GNU_SOURCE
#include "tablet-unstable-v2-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

static struct wl_compositor *comp; static struct wl_shm *shm; static struct xdg_wm_base *wm; static struct wl_seat *seat;
static struct zwp_tablet_manager_v2 *tm; static struct zwp_tablet_seat_v2 *ts;
static struct wl_surface *surf; static struct xdg_toplevel *top;
static int W, H, configured, running = 1, pressure_max_seen, ntablet, ntool;
static unsigned long n_touch, n_ptr, n_tool_motion;

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); fflush(stdout); }

static void t_down(void *d, struct wl_touch *t, uint32_t s, uint32_t tm_, struct wl_surface *sf, int32_t id, wl_fixed_t x, wl_fixed_t y) { (void)d; (void)t; (void)s; (void)tm_; (void)sf; n_touch++; say("touch down %d %.0f %.0f\n", id, wl_fixed_to_double(x), wl_fixed_to_double(y)); }
static void t_up(void *d, struct wl_touch *t, uint32_t s, uint32_t tm_, int32_t id) { (void)d; (void)t; (void)s; (void)tm_; say("touch up %d\n", id); }
static void t_motion(void *d, struct wl_touch *t, uint32_t tm_, int32_t id, wl_fixed_t x, wl_fixed_t y) { (void)d; (void)t; (void)tm_; n_touch++; say("touch move %d %.0f %.0f\n", id, wl_fixed_to_double(x), wl_fixed_to_double(y)); }
static void t_frame(void *d, struct wl_touch *t) { (void)d; (void)t; }
static void t_cancel(void *d, struct wl_touch *t) { (void)d; (void)t; }
static void t_shape(void *d, struct wl_touch *t, int32_t i, wl_fixed_t a, wl_fixed_t b) { (void)d; (void)t; (void)i; (void)a; (void)b; }
static void t_orient(void *d, struct wl_touch *t, int32_t i, wl_fixed_t a) { (void)d; (void)t; (void)i; (void)a; }
static const struct wl_touch_listener tl = { t_down, t_up, t_motion, t_frame, t_cancel, t_shape, t_orient };

static void p_enter(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *sf, wl_fixed_t x, wl_fixed_t y) { (void)d; (void)p; (void)s; (void)sf; say("pointer enter %.0f %.0f\n", wl_fixed_to_double(x), wl_fixed_to_double(y)); }
static void p_leave(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *sf) { (void)d; (void)p; (void)s; (void)sf; }
static void p_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y) { (void)d; (void)p; (void)t; n_ptr++; say("pointer %.0f %.0f\n", wl_fixed_to_double(x), wl_fixed_to_double(y)); }
static void p_button(void *d, struct wl_pointer *p, uint32_t s, uint32_t t, uint32_t b, uint32_t st) { (void)d; (void)p; (void)s; (void)t; say("pointer button %u %u\n", b, st); }
static void p_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v) { (void)d; (void)p; (void)t; say("pointer axis %u %.1f\n", a, wl_fixed_to_double(v)); }
static void p_frame(void *d, struct wl_pointer *p) { (void)d; (void)p; }
static void p_as(void *d, struct wl_pointer *p, uint32_t s) { (void)d; (void)p; (void)s; }
static void p_ast(void *d, struct wl_pointer *p, uint32_t t, uint32_t s) { (void)d; (void)p; (void)t; (void)s; }
static void p_ad(void *d, struct wl_pointer *p, uint32_t s, int32_t v) { (void)d; (void)p; (void)s; (void)v; }
static void p_a120(void *d, struct wl_pointer *p, uint32_t s, int32_t v) { (void)d; (void)p; (void)s; (void)v; }
static const struct wl_pointer_listener pl = { p_enter, p_leave, p_motion, p_button, p_axis, p_frame, p_as, p_ast, p_ad, p_a120 };

static void s_caps(void *d, struct wl_seat *s, uint32_t c)
{
  (void)d;
  if (c & WL_SEAT_CAPABILITY_TOUCH) wl_touch_add_listener(wl_seat_get_touch(s), &tl, NULL);
  if (c & WL_SEAT_CAPABILITY_POINTER) wl_pointer_add_listener(wl_seat_get_pointer(s), &pl, NULL);
}
static void s_name(void *d, struct wl_seat *s, const char *n) { (void)d; (void)s; (void)n; }
static const struct wl_seat_listener sl = { s_caps, s_name };

static void tool_type(void *d, struct zwp_tablet_tool_v2 *t, uint32_t ty) { (void)d; (void)t; say("tablet tool type %u\n", ty); }
static void tool_ser(void *d, struct zwp_tablet_tool_v2 *t, uint32_t a, uint32_t b) { (void)d; (void)t; (void)a; (void)b; }
static void tool_hw(void *d, struct zwp_tablet_tool_v2 *t, uint32_t a, uint32_t b) { (void)d; (void)t; (void)a; (void)b; }
static void tool_cap(void *d, struct zwp_tablet_tool_v2 *t, uint32_t c) { (void)d; (void)t; say("tablet tool capability %u\n", c); }
static void tool_done(void *d, struct zwp_tablet_tool_v2 *t) { (void)d; (void)t; }
static void tool_removed(void *d, struct zwp_tablet_tool_v2 *t) { (void)d; (void)t; }
static void tool_prox_in(void *d, struct zwp_tablet_tool_v2 *t, uint32_t s, struct zwp_tablet_v2 *tb, struct wl_surface *sf) { (void)d; (void)t; (void)s; (void)tb; (void)sf; say("tablet proximity in\n"); }
static void tool_prox_out(void *d, struct zwp_tablet_tool_v2 *t) { (void)d; (void)t; say("tablet proximity out\n"); }
static void tool_down(void *d, struct zwp_tablet_tool_v2 *t, uint32_t s) { (void)d; (void)t; (void)s; say("tablet down\n"); }
static void tool_up(void *d, struct zwp_tablet_tool_v2 *t) { (void)d; (void)t; say("tablet up\n"); }
static void tool_motion(void *d, struct zwp_tablet_tool_v2 *t, wl_fixed_t x, wl_fixed_t y) { (void)d; (void)t; n_tool_motion++; say("tablet motion %.0f %.0f\n", wl_fixed_to_double(x), wl_fixed_to_double(y)); }
static void tool_pressure(void *d, struct zwp_tablet_tool_v2 *t, uint32_t p) { (void)d; (void)t; if ((int)p > pressure_max_seen) pressure_max_seen = p; say("tablet pressure %u\n", p); }
static void tool_distance(void *d, struct zwp_tablet_tool_v2 *t, uint32_t v) { (void)d; (void)t; (void)v; }
static void tool_tilt(void *d, struct zwp_tablet_tool_v2 *t, wl_fixed_t x, wl_fixed_t y) { (void)d; (void)t; say("tablet tilt %.0f %.0f\n", wl_fixed_to_double(x), wl_fixed_to_double(y)); }
static void tool_rot(void *d, struct zwp_tablet_tool_v2 *t, wl_fixed_t v) { (void)d; (void)t; (void)v; }
static void tool_slider(void *d, struct zwp_tablet_tool_v2 *t, int32_t v) { (void)d; (void)t; (void)v; }
static void tool_wheel(void *d, struct zwp_tablet_tool_v2 *t, wl_fixed_t a, int32_t b) { (void)d; (void)t; (void)a; (void)b; }
static void tool_button(void *d, struct zwp_tablet_tool_v2 *t, uint32_t s, uint32_t b, uint32_t st) { (void)d; (void)t; (void)s; say("tablet button %u %u\n", b, st); }
static void tool_frame(void *d, struct zwp_tablet_tool_v2 *t, uint32_t tm_) { (void)d; (void)t; (void)tm_; }
static const struct zwp_tablet_tool_v2_listener tol = { tool_type, tool_ser, tool_hw, tool_cap, tool_done, tool_removed, tool_prox_in, tool_prox_out, tool_down, tool_up, tool_motion,
  tool_pressure, tool_distance, tool_tilt, tool_rot, tool_slider, tool_wheel, tool_button, tool_frame };

static void ts_tablet(void *d, struct zwp_tablet_seat_v2 *s, struct zwp_tablet_v2 *t) { (void)d; (void)s; (void)t; ntablet++; say("tablet added\n"); }
static void ts_tool(void *d, struct zwp_tablet_seat_v2 *s, struct zwp_tablet_tool_v2 *t) { (void)d; (void)s; ntool++; zwp_tablet_tool_v2_add_listener(t, &tol, NULL); say("tablet tool added\n"); }
static void ts_pad(void *d, struct zwp_tablet_seat_v2 *s, struct zwp_tablet_pad_v2 *p) { (void)d; (void)s; (void)p; }
static const struct zwp_tablet_seat_v2_listener tsl = { ts_tablet, ts_tool, ts_pad };

static void reg(void *d, struct wl_registry *r, uint32_t id, const char *iface, uint32_t ver)
{
  (void)d;
  if (!strcmp(iface, "wl_compositor")) comp = wl_registry_bind(r, id, &wl_compositor_interface, 4);
  else if (!strcmp(iface, "wl_shm")) shm = wl_registry_bind(r, id, &wl_shm_interface, 1);
  else if (!strcmp(iface, "xdg_wm_base")) wm = wl_registry_bind(r, id, &xdg_wm_base_interface, ver < 2 ? ver : 2);
  else if (!strcmp(iface, "wl_seat")) { seat = wl_registry_bind(r, id, &wl_seat_interface, ver < 5 ? ver : 5); wl_seat_add_listener(seat, &sl, NULL); }
  else if (!strcmp(iface, "zwp_tablet_manager_v2")) tm = wl_registry_bind(r, id, &zwp_tablet_manager_v2_interface, 1);
}
static void reg_rm(void *d, struct wl_registry *r, uint32_t id) { (void)d; (void)r; (void)id; }
static const struct wl_registry_listener rl = { reg, reg_rm };

static void wm_ping(void *d, struct xdg_wm_base *w, uint32_t s) { (void)d; xdg_wm_base_pong(w, s); }
static const struct xdg_wm_base_listener wml = { wm_ping };
static void xs_conf(void *d, struct xdg_surface *x, uint32_t s) { (void)d; xdg_surface_ack_configure(x, s); configured = 1; }
static const struct xdg_surface_listener xsl = { xs_conf };
static void top_conf(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *st) { (void)d; (void)t; (void)st; if (w > 0 && h > 0) { W = w; H = h; } }
static void top_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; running = 0; }
static const struct xdg_toplevel_listener tpl = { top_conf, top_close, NULL, NULL };

int main(void)
{
  struct wl_display *dpy = wl_display_connect(NULL);
  if (!dpy) { fprintf(stderr, "ip: no display\n"); return 1; }
  struct wl_registry *r = wl_display_get_registry(dpy); wl_registry_add_listener(r, &rl, NULL); wl_display_roundtrip(dpy);
  if (!comp || !shm || !wm) { fprintf(stderr, "ip: missing globals\n"); return 1; }
  say("globals: seat %d tablet_manager %d\n", seat != NULL, tm != NULL);
  xdg_wm_base_add_listener(wm, &wml, NULL);
  if (tm && seat) { ts = zwp_tablet_manager_v2_get_tablet_seat(tm, seat); zwp_tablet_seat_v2_add_listener(ts, &tsl, NULL); }
  surf = wl_compositor_create_surface(comp);
  struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm, surf); xdg_surface_add_listener(xs, &xsl, NULL);
  top = xdg_surface_get_toplevel(xs); xdg_toplevel_add_listener(top, &tpl, NULL);
  xdg_toplevel_set_fullscreen(top, NULL); xdg_toplevel_set_title(top, "input-probe"); xdg_toplevel_set_app_id(top, "input-probe");
  wl_surface_commit(surf);
  while (!configured) if (wl_display_dispatch(dpy) < 0) return 1;
  if (W <= 0) { W = 1000; H = 600; }
  int stride = W * 4; size_t sz = (size_t)stride * H;
  int fd = memfd_create("ip", 0); if (fd < 0 || ftruncate(fd, sz)) return 1;
  uint32_t *px = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  for (size_t i = 0; i < sz / 4; i++) px[i] = 0xff202830;
  // markers at known desktop corners: white 300x300 top-left, red 300x300 top-right (a screencap of the panel then shows which way the desktop is turned)
  for (int y = 0; y < 300 && y < H; y++) for (int x = 0; x < 300 && x < W; x++) { px[(size_t)y * W + x] = 0xffffffff; px[(size_t)y * W + (W - 1 - x)] = 0xffff0000; }
  struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, sz);
  struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, W, H, stride, WL_SHM_FORMAT_XRGB8888);
  wl_surface_attach(surf, b, 0, 0); wl_surface_commit(surf);
  say("surface %dx%d\n", W, H);
  const char *e = getenv("X400_IP_SECS"); int secs = e ? atoi(e) : 20;
  time_t t0 = time(NULL);
  while (running && time(NULL) - t0 < secs) {
    wl_display_flush(dpy);
    if (wl_display_prepare_read(dpy) == 0) { struct pollfd pf = { .fd = wl_display_get_fd(dpy), .events = POLLIN }; if (poll(&pf, 1, 200) > 0) wl_display_read_events(dpy); else wl_display_cancel_read(dpy); }
    wl_display_dispatch_pending(dpy);
  }
  say("summary: surface %dx%d touch %lu pointer %lu tablets %d tools %d tool_motion %lu max_pressure %d\n", W, H, n_touch, n_ptr, ntablet, ntool, n_tool_motion, pressure_max_seen);
  return 0;
}
