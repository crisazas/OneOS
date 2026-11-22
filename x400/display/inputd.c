// linux-x400: reads the tablet's touchscreen (and pen) from /dev/input and injects them into KWin through
// org_kde_kwin_fake_input (KWIN_WAYLAND_NO_PERMISSION_CHECKS=1). Runs inside the chroot.
// The panel is landscape (2112x1320, Android rotation 1) while the touch reports the natural portrait 1320x2112:
// rot 1 (default): x' = y * W / (Ymax+1), y' = (Xmax - x) * H / (Xmax+1)    rot 0: no rotation    rot 3: the opposite turn
// Usage: inputd [--touch /dev/input/event8] [--pen /dev/input/event10] [--rot N] [--size WxH] [--grab] [--quiet]
// --grab takes EVIOCGRAB on the touchscreen so One UI does not also see the touches; closing the fd (a crash included)
// gives it back. Never opens the side keys (event0..event4).
#define _GNU_SOURCE
#include "fake-input-client-protocol.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <wayland-client.h>

static struct wl_display *dpy;
static struct org_kde_kwin_fake_input *fake;
static volatile int stopping;
static void on_sig(int n) { (void)n; stopping = 1; }
static void reg_global(void *d, struct wl_registry *r, uint32_t id, const char *iface, uint32_t ver)
{ (void)d; if (!strcmp(iface, "org_kde_kwin_fake_input")) fake = wl_registry_bind(r, id, &org_kde_kwin_fake_input_interface, ver < 4 ? ver : 4); }
static void reg_remove(void *d, struct wl_registry *r, uint32_t id) { (void)d; (void)r; (void)id; }
static const struct wl_registry_listener reg_l = { reg_global, reg_remove };

struct dev { int fd; int xmax, ymax; int pxmax, pymax, pmax; };
static int W = 2112, H = 1320, rot = 1, quiet;

static void map(int x, int y, int xmax, int ymax, double *ox, double *oy)
{
  double fx = (double)x / (xmax ? xmax : 1), fy = (double)y / (ymax ? ymax : 1);   // 0..1 in the natural portrait frame
  switch (rot) {
  case 1:  *ox = fy * (W - 1); *oy = (1.0 - fx) * (H - 1); break;
  case 3:  *ox = (1.0 - fy) * (W - 1); *oy = fx * (H - 1); break;
  case 2:  *ox = (1.0 - fx) * (W - 1); *oy = (1.0 - fy) * (H - 1); break;
  default: *ox = fx * (W - 1); *oy = fy * (H - 1); break;
  }
}

struct slot { int x, y, tracking, active, dirty, ended; };
static struct slot slots[16];

static int open_dev(const char *path, int grab, struct dev *d)
{
  d->fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  if (d->fd < 0) { fprintf(stderr, "inputd: %s: %s\n", path, strerror(errno)); return -1; }
  struct input_absinfo a;
  if (ioctl(d->fd, EVIOCGABS(ABS_MT_POSITION_X), &a) == 0) d->xmax = a.maximum; else if (ioctl(d->fd, EVIOCGABS(ABS_X), &a) == 0) d->xmax = a.maximum;
  if (ioctl(d->fd, EVIOCGABS(ABS_MT_POSITION_Y), &a) == 0) d->ymax = a.maximum; else if (ioctl(d->fd, EVIOCGABS(ABS_Y), &a) == 0) d->ymax = a.maximum;
  if (grab && ioctl(d->fd, EVIOCGRAB, 1) < 0) fprintf(stderr, "inputd: EVIOCGRAB %s: %s\n", path, strerror(errno));
  fprintf(stderr, "inputd: %s range %dx%d%s\n", path, d->xmax, d->ymax, grab ? " (grabbed)" : "");
  return 0;
}

static void touch_event(struct dev *d, struct input_event *e, int *cur)
{
  if (e->type == EV_ABS) {
    struct slot *s = &slots[*cur];
    if (e->code == ABS_MT_SLOT) { if (e->value >= 0 && e->value < 16) *cur = e->value; }
    else if (e->code == ABS_MT_TRACKING_ID) { if (e->value < 0) s->ended = 1; else { s->tracking = 1; s->dirty = 1; } }
    else if (e->code == ABS_MT_POSITION_X) { s->x = e->value; s->dirty = 1; }
    else if (e->code == ABS_MT_POSITION_Y) { s->y = e->value; s->dirty = 1; }
  } else if (e->type == EV_SYN && e->code == SYN_REPORT) {
    int any = 0;
    for (int i = 0; i < 16; i++) {
      struct slot *s = &slots[i]; double x, y;
      if (s->ended) {
        if (s->active) org_kde_kwin_fake_input_touch_up(fake, i);
        s->ended = s->active = s->tracking = s->dirty = 0; any = 1;
      } else if (s->tracking && s->dirty) {
        map(s->x, s->y, d->xmax, d->ymax, &x, &y);
        if (!s->active) { org_kde_kwin_fake_input_touch_down(fake, i, wl_fixed_from_double(x), wl_fixed_from_double(y)); s->active = 1; }
        else org_kde_kwin_fake_input_touch_motion(fake, i, wl_fixed_from_double(x), wl_fixed_from_double(y));
        s->dirty = 0; any = 1;
      }
    }
    if (any) { org_kde_kwin_fake_input_touch_frame(fake); wl_display_flush(dpy); }
  }
}

int main(int argc, char **argv)
{
  const char *touch = "/dev/input/event8", *pen = NULL; int grab = 0;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--touch") && i + 1 < argc) touch = argv[++i];
    else if (!strcmp(argv[i], "--pen") && i + 1 < argc) pen = argv[++i];
    else if (!strcmp(argv[i], "--rot") && i + 1 < argc) rot = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &W, &H);
    else if (!strcmp(argv[i], "--grab")) grab = 1;
    else if (!strcmp(argv[i], "--quiet")) quiet = 1;
    else { fprintf(stderr, "usage: inputd [--touch DEV] [--pen DEV] [--rot 0..3] [--size WxH] [--grab] [--quiet]\n"); return 2; }
  }
  if (strstr(touch, "event0") || strstr(touch, "event1") || strstr(touch, "event2") || strstr(touch, "event3") || strstr(touch, "event4")) { fprintf(stderr, "inputd: refusing the side keys\n"); return 2; }
  struct sigaction sa = { .sa_handler = on_sig }; sigemptyset(&sa.sa_mask);
  sigaction(SIGTERM, &sa, NULL); sigaction(SIGINT, &sa, NULL); sigaction(SIGHUP, &sa, NULL);
  dpy = wl_display_connect(NULL);
  if (!dpy) { fprintf(stderr, "inputd: no Wayland display\n"); return 1; }
  struct wl_registry *r = wl_display_get_registry(dpy);
  wl_registry_add_listener(r, &reg_l, NULL);
  wl_display_roundtrip(dpy);
  if (!fake) { fprintf(stderr, "inputd: KWin offers no org_kde_kwin_fake_input\n"); return 1; }
  org_kde_kwin_fake_input_authenticate(fake, "x400 inputd", "touch and pen of the tablet");
  wl_display_roundtrip(dpy);
  struct dev t = { .fd = -1 }, p = { .fd = -1 };
  if (open_dev(touch, grab, &t) < 0) return 1;
  if (pen) open_dev(pen, 0, &p);
  int cur = 0, pen_down = 0, px = 0, py = 0; unsigned long n = 0;
  struct pollfd pf[3] = { { .fd = t.fd, .events = POLLIN }, { .fd = p.fd, .events = POLLIN }, { .fd = wl_display_get_fd(dpy), .events = POLLIN } };
  while (!stopping) {
    if (poll(pf, 3, 500) < 0 && errno != EINTR) break;
    if (pf[2].revents & (POLLERR | POLLHUP)) { fprintf(stderr, "inputd: KWin went away\n"); break; }
    if (pf[2].revents & POLLIN) { if (wl_display_dispatch(dpy) < 0) break; }
    struct input_event e;
    if (pf[0].revents & POLLIN) while (read(t.fd, &e, sizeof e) == sizeof e) { touch_event(&t, &e, &cur); n++; }
    if (p.fd >= 0 && (pf[1].revents & POLLIN)) while (read(p.fd, &e, sizeof e) == sizeof e) {
      if (e.type == EV_ABS && e.code == ABS_X) px = e.value; else if (e.type == EV_ABS && e.code == ABS_Y) py = e.value;
      else if (e.type == EV_KEY && e.code == BTN_TOUCH) { org_kde_kwin_fake_input_button(fake, BTN_LEFT, e.value ? 1 : 0); pen_down = e.value; }
      else if (e.type == EV_SYN && e.code == SYN_REPORT) { double x, y; map(px, py, p.xmax, p.ymax, &x, &y); org_kde_kwin_fake_input_pointer_motion_absolute(fake, wl_fixed_from_double(x), wl_fixed_from_double(y)); wl_display_flush(dpy); }
    }
    (void)pen_down;
  }
  if (t.fd >= 0) close(t.fd);
  if (!quiet) fprintf(stderr, "inputd: stopped after %lu touch events\n", n);
  return 0;
}
