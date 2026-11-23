// linux-x400: reads the tablet's touchscreen (and pen) from /dev/input and injects them into KWin through
// org_kde_kwin_fake_input (KWIN_WAYLAND_NO_PERMISSION_CHECKS=1). Runs inside the chroot.
// The panel is landscape (2112x1320, Android rotation 1) while the touch reports the natural portrait 1320x2112:
// rot 1 (default): x' = y * W / (Ymax+1), y' = (Xmax - x) * H / (Xmax+1)    rot 0: no rotation    rot 3: the opposite turn
// Rotation follows KWin's output: the orientation file (--orient, default /tmp/x400-orientation.env, written by x400/sensors/x400-rotate-hook) is read at
// start, on SIGHUP and when it is replaced (inotify), and sets the mapping and the logical size without letting go of the grab. The mapping comes from
// X400_KSCREEN_ROT (none 1, left 2, inverted 3, right 0: measured on the panel, see x400/display/ROTATION.md); X400_TOUCH_ROT is only used when that is missing.
// Usage: inputd [--touch /dev/input/event8|none] [--pen /dev/input/event10] [--rot N] [--size WxH] [--orient FILE|none] [--grab] [--quiet] [--trace]
// The pen: position and hover, tip = left button, barrel button + tip = right, eraser = middle (no pressure/tilt: see x400/input/RESULT.md).
// --grab takes EVIOCGRAB on the touchscreen so One UI does not also see the touches; closing the fd (a crash included)
// gives it back. The grab is also let go as soon as the Wayland server goes away or a stop signal comes, and only held while
// KWin is there. Never opens the side keys (event0..event4).
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
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <wayland-client.h>

static struct wl_display *dpy;
static struct org_kde_kwin_fake_input *fake;
static volatile int stopping;
static volatile int reload;
static void on_sig(int n) { (void)n; stopping = 1; }
static void on_hup(int n) { (void)n; reload = 1; }
static void reg_global(void *d, struct wl_registry *r, uint32_t id, const char *iface, uint32_t ver)
{ (void)d; if (!strcmp(iface, "org_kde_kwin_fake_input")) fake = wl_registry_bind(r, id, &org_kde_kwin_fake_input_interface, ver < 4 ? ver : 4); }
static void reg_remove(void *d, struct wl_registry *r, uint32_t id) { (void)d; (void)r; (void)id; }
static const struct wl_registry_listener reg_l = { reg_global, reg_remove };

struct dev { int fd; int xmax, ymax; int pxmax, pymax, pmax; };
static int W = 2112, H = 1320, rot = 1, quiet, trace;

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

// the orientation file is KEY=VALUE lines (see x400-rotate-hook); returns 1 if it changed the mapping
static const char *orient_path = "/tmp/x400-orientation.env";
static int read_orient(void)
{
  FILE *f = orient_path ? fopen(orient_path, "r") : NULL;
  if (!f) return 0;
  char line[128], ks[32] = ""; int tr = -1, w = 0, h = 0;
  while (fgets(line, sizeof line, f)) {
    if (sscanf(line, "X400_KSCREEN_ROT=%31s", ks) == 1) continue;
    if (sscanf(line, "X400_TOUCH_ROT=%d", &tr) == 1) continue;
    if (sscanf(line, "X400_LOGICAL_W=%d", &w) == 1) continue;
    sscanf(line, "X400_LOGICAL_H=%d", &h);
  }
  fclose(f);
  int nr = tr;
  if (!strcmp(ks, "none")) nr = 1; else if (!strcmp(ks, "left")) nr = 2; else if (!strcmp(ks, "inverted")) nr = 3; else if (!strcmp(ks, "right")) nr = 0;
  if (nr < 0 || nr > 3 || w <= 0 || h <= 0) { fprintf(stderr, "inputd: %s: no usable orientation (kscreen '%s', rot %d, %dx%d)\n", orient_path, ks, tr, w, h); return 0; }
  if (nr == rot && w == W && h == H) return 0;
  fprintf(stderr, "inputd: orientation %s: rot %d -> %d, size %dx%d -> %dx%d\n", ks, rot, nr, W, H, w, h);
  rot = nr; W = w; H = h;
  return 1;
}

static int open_dev(const char *path, int grab, struct dev *d)
{
  d->fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  if (d->fd < 0) { fprintf(stderr, "inputd: %s: %s\n", path, strerror(errno)); return -1; }
  struct input_absinfo a;
  // the kernel answers EVIOCGABS with zeros for an axis the device lacks, so an answer only counts when its maximum is not 0
  d->xmax = d->ymax = 0;
  if (ioctl(d->fd, EVIOCGABS(ABS_MT_POSITION_X), &a) == 0 && a.maximum > 0) d->xmax = a.maximum; else if (ioctl(d->fd, EVIOCGABS(ABS_X), &a) == 0) d->xmax = a.maximum;
  if (ioctl(d->fd, EVIOCGABS(ABS_MT_POSITION_Y), &a) == 0 && a.maximum > 0) d->ymax = a.maximum; else if (ioctl(d->fd, EVIOCGABS(ABS_Y), &a) == 0) d->ymax = a.maximum;
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
        if (trace) fprintf(stderr, "touch %d %d,%d -> %.0f %.0f\n", i, s->x, s->y, x, y);
        if (!s->active) { org_kde_kwin_fake_input_touch_down(fake, i, wl_fixed_from_double(x), wl_fixed_from_double(y)); s->active = 1; }
        else org_kde_kwin_fake_input_touch_motion(fake, i, wl_fixed_from_double(x), wl_fixed_from_double(y));
        s->dirty = 0; any = 1;
      }
    }
    if (any) { org_kde_kwin_fake_input_touch_frame(fake); wl_display_flush(dpy); }
  }
}

// The pen goes in as a mouse: KWin's fake_input has no tablet requests, and in a --virtual session KWin opens no /dev/input at all,
// so a uinput tablet would never be seen. Pressure and tilt therefore have no way in. Tip = left button, tip with the barrel button
// held = right button, eraser tip = middle button; the button is chosen when the tip comes down and released with it.
struct pen { int x, y, lx, ly, down, btn, stylus, rubber, prox, seen_tool, dirty; };

static void pen_button(struct pen *s, int code, int value)
{ org_kde_kwin_fake_input_button(fake, code, value); if (trace) fprintf(stderr, "pen button %d %d\n", code, value); (void)s; }

static void pen_event(struct pen *s, struct input_event *e, int xmax, int ymax)
{
  if (e->type == EV_ABS) {
    if (e->code == ABS_X) { s->x = e->value; s->dirty = 1; } else if (e->code == ABS_Y) { s->y = e->value; s->dirty = 1; }
  } else if (e->type == EV_KEY) {
    switch (e->code) {
    case BTN_TOOL_PEN: s->seen_tool = 1; s->prox = e->value ? 1 : (s->rubber ? 1 : 0); if (!e->value) s->dirty = 0; break;
    case BTN_TOOL_RUBBER: s->seen_tool = 1; s->rubber = e->value; s->prox = e->value ? 1 : 0; break;
    case BTN_STYLUS: s->stylus = e->value; break;
    case BTN_TOUCH:
      if (e->value && !s->down) {
        s->btn = s->rubber ? BTN_MIDDLE : s->stylus ? BTN_RIGHT : BTN_LEFT; s->down = 1;
        if (s->dirty) { double x, y; map(s->x, s->y, xmax, ymax, &x, &y); org_kde_kwin_fake_input_pointer_motion_absolute(fake, wl_fixed_from_double(x), wl_fixed_from_double(y)); s->lx = s->x; s->ly = s->y; s->dirty = 0; }
        pen_button(s, s->btn, 1);
      } else if (!e->value && s->down) { pen_button(s, s->btn, 0); s->down = 0; }
      wl_display_flush(dpy); break;
    }
  } else if (e->type == EV_SYN && e->code == SYN_REPORT) {
    if (s->dirty && (s->prox || s->down || !s->seen_tool) && (s->x != s->lx || s->y != s->ly)) {
      double x, y; map(s->x, s->y, xmax, ymax, &x, &y);
      org_kde_kwin_fake_input_pointer_motion_absolute(fake, wl_fixed_from_double(x), wl_fixed_from_double(y));
      s->lx = s->x; s->ly = s->y; wl_display_flush(dpy);
      if (trace) fprintf(stderr, "pen move %.0f %.0f\n", x, y);
    }
    s->dirty = 0;
  }
}

static struct pen *cur_pen;
// A rotation while a finger or the pen is down would move it to somewhere else: lift them (a finger has to touch again).
static void release_all(void)
{
  int any = 0;
  for (int i = 0; i < 16; i++) if (slots[i].active) { org_kde_kwin_fake_input_touch_up(fake, i); any = 1; }
  for (int i = 0; i < 16; i++) { slots[i].active = slots[i].tracking = slots[i].dirty = slots[i].ended = 0; }
  if (any) org_kde_kwin_fake_input_touch_frame(fake);
  if (cur_pen && cur_pen->down) { pen_button(cur_pen, cur_pen->btn, 0); cur_pen->down = 0; }
  if (cur_pen) { cur_pen->dirty = 0; cur_pen->lx = cur_pen->ly = -1; }
  wl_display_flush(dpy);
}

static int is_side_key(const char *path)
{
  const char *n = strstr(path, "event"); int k;
  return n && sscanf(n + 5, "%d", &k) == 1 && k >= 0 && k <= 4;
}

int main(int argc, char **argv)
{
  const char *touch = "/dev/input/event8", *pen = NULL; int grab = 0;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--touch") && i + 1 < argc) touch = argv[++i];
    else if (!strcmp(argv[i], "--pen") && i + 1 < argc) pen = argv[++i];
    else if (!strcmp(argv[i], "--rot") && i + 1 < argc) rot = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &W, &H);
    else if (!strcmp(argv[i], "--orient") && i + 1 < argc) { orient_path = argv[++i]; if (!strcmp(orient_path, "none")) orient_path = NULL; }
    else if (!strcmp(argv[i], "--grab")) grab = 1;
    else if (!strcmp(argv[i], "--quiet")) quiet = 1;
    else if (!strcmp(argv[i], "--trace")) trace = 1;
    else { fprintf(stderr, "usage: inputd [--touch DEV] [--pen DEV] [--rot 0..3] [--size WxH] [--orient FILE|none] [--grab] [--quiet] [--trace]\n"); return 2; }
  }
  if (is_side_key(touch) || (pen && is_side_key(pen))) { fprintf(stderr, "inputd: refusing the side keys\n"); return 2; }
  struct sigaction sa = { .sa_handler = on_sig }; sigemptyset(&sa.sa_mask);
  sigaction(SIGTERM, &sa, NULL); sigaction(SIGINT, &sa, NULL);
  struct sigaction sh = { .sa_handler = on_hup }; sigemptyset(&sh.sa_mask); sigaction(SIGHUP, &sh, NULL);
  dpy = wl_display_connect(NULL);
  if (!dpy) { fprintf(stderr, "inputd: no Wayland display\n"); return 1; }
  struct wl_registry *r = wl_display_get_registry(dpy);
  wl_registry_add_listener(r, &reg_l, NULL);
  wl_display_roundtrip(dpy);
  if (!fake) { fprintf(stderr, "inputd: KWin offers no org_kde_kwin_fake_input\n"); return 1; }
  org_kde_kwin_fake_input_authenticate(fake, "x400 inputd", "touch and pen of the tablet");
  wl_display_roundtrip(dpy);
  struct dev t = { .fd = -1 }, p = { .fd = -1 };
  if (strcmp(touch, "none") && open_dev(touch, grab, &t) < 0) return 1;
  int cur = 0; unsigned long n = 0; struct pen ps = { .lx = -1, .ly = -1 };
  if (pen && open_dev(pen, 0, &p) < 0) pen = NULL;
  cur_pen = &ps;
  read_orient();
  int in = -1; char odir[256] = "", obase[128] = "";
  if (orient_path) {
    const char *sl = strrchr(orient_path, '/');
    snprintf(odir, sizeof odir, "%.*s", sl ? (int)(sl - orient_path) : 1, sl ? orient_path : "."); snprintf(obase, sizeof obase, "%s", sl ? sl + 1 : orient_path);
    in = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (in >= 0 && inotify_add_watch(in, odir, IN_MOVED_TO | IN_CLOSE_WRITE | IN_CREATE) < 0) { close(in); in = -1; }
  }
  struct pollfd pf[4] = { { .fd = t.fd, .events = POLLIN }, { .fd = p.fd, .events = POLLIN }, { .fd = wl_display_get_fd(dpy), .events = POLLIN }, { .fd = in, .events = POLLIN } };
  while (!stopping) {
    if (poll(pf, 4, 500) < 0 && errno != EINTR) break;
    if (in >= 0 && (pf[3].revents & POLLIN)) {
      char buf[1024] __attribute__((aligned(8))); ssize_t k = read(in, buf, sizeof buf);
      for (char *q = buf; k > 0 && q < buf + k; q += sizeof(struct inotify_event) + ((struct inotify_event *)q)->len)
        if (((struct inotify_event *)q)->len && !strcmp(((struct inotify_event *)q)->name, obase)) reload = 1;
    }
    if (reload) { reload = 0; if (read_orient()) release_all(); }
    if (pf[2].revents & (POLLERR | POLLHUP)) { fprintf(stderr, "inputd: KWin went away\n"); break; }
    if (pf[2].revents & POLLIN) { if (wl_display_dispatch(dpy) < 0) break; }
    struct input_event e;
    if (t.fd >= 0 && (pf[0].revents & POLLIN)) while (read(t.fd, &e, sizeof e) == sizeof e) { touch_event(&t, &e, &cur); n++; }
    if (p.fd >= 0 && (pf[1].revents & POLLIN)) while (read(p.fd, &e, sizeof e) == sizeof e) pen_event(&ps, &e, p.xmax, p.ymax);
  }
  if (t.fd >= 0) { if (grab) ioctl(t.fd, EVIOCGRAB, 0); close(t.fd); }
  if (p.fd >= 0) close(p.fd);
  if (!quiet) fprintf(stderr, "inputd: stopped after %lu touch events\n", n);
  return 0;
}
