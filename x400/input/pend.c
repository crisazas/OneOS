// linux-x400: the S Pen as a real tablet for KWin. KWin's --virtual backend has no libinput of its own (x400_kwin_input.so gives it one, see x400/input/kwin-input-shim.c), and
// libinput only takes a device that has a udev database entry, so this daemon (started before KWin) does three things:
//   1. reads the pen (sec_e-pen: ABS_X 0..14680, ABS_Y 0..23488, ABS_PRESSURE 0..4095, tilt, BTN_TOUCH/STYLUS/TOOL_PEN/TOOL_RUBBER),
//   2. creates a uinput tablet "x400-pen" whose X/Y are already turned into the desktop's orientation (0..65535 across the screen, so KWin stretches them over the output
//      whatever its size) and whose pressure, tilt, distance and buttons are the pen's own,
//   3. writes /run/udev/data/c13:<minor> (ID_INPUT=1, ID_INPUT_TABLET=1): there is no udevd in this guest and libinput skips a device whose database entry is missing.
// The orientation comes from the same file inputd reads (X400_KSCREEN_ROT, see x400/display/ROTATION.md); it is looked at again when the file's mtime changes.
// Usage: pend [--pen /dev/input/event10] [--orient /tmp/x400-orientation.env] [--trace]
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

static volatile int stopping;
static void on_sig(int n) { (void)n; stopping = 1; }
static int ufd = -1, trace;
static int rot = 1;                       // inputd's numbering: 1 landscape (none), 2 left, 3 inverted, 0 right: see inputd.c map()
static const char *orient_path = "/tmp/x400-orientation.env";
static time_t orient_mtime;

static void emit(int t, int c, int v) { struct input_event e = { .type = t, .code = c, .value = v }; if (write(ufd, &e, sizeof e) < 0 && errno != EAGAIN) perror("pend: uinput write"); }

static void read_orient(void)
{
  struct stat st; if (stat(orient_path, &st) < 0 || st.st_mtime == orient_mtime) return;
  FILE *f = fopen(orient_path, "r"); if (!f) return;
  char line[128], ks[32] = ""; int tr = -1;
  while (fgets(line, sizeof line, f)) { if (sscanf(line, "X400_KSCREEN_ROT=%31s", ks) == 1) continue; sscanf(line, "X400_TOUCH_ROT=%d", &tr); }
  fclose(f); orient_mtime = st.st_mtime;
  int nr = tr;
  if (!strcmp(ks, "none")) nr = 1; else if (!strcmp(ks, "left")) nr = 2; else if (!strcmp(ks, "inverted")) nr = 3; else if (!strcmp(ks, "right")) nr = 0;
  if (nr >= 0 && nr <= 3 && nr != rot) { fprintf(stderr, "pend: orientation %s: rot %d -> %d\n", ks, rot, nr); rot = nr; }
}

// the same table as inputd.c map(), as fractions of the screen (0..1)
static void turn(double fx, double fy, double *ox, double *oy)
{
  switch (rot) {
  case 1:  *ox = fy; *oy = 1.0 - fx; break;
  case 3:  *ox = 1.0 - fy; *oy = fx; break;
  case 2:  *ox = 1.0 - fx; *oy = 1.0 - fy; break;
  default: *ox = fx; *oy = fy; break;
  }
}

static void abs_set(int c, int mn, int mx, int res)
{
  struct uinput_abs_setup a = { .code = c, .absinfo = { .minimum = mn, .maximum = mx, .resolution = res } };
  ioctl(ufd, UI_SET_ABSBIT, c); ioctl(ufd, UI_ABS_SETUP, &a);
}

int main(int argc, char **argv)
{
  const char *pen = "/dev/input/event10";
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--pen") && i + 1 < argc) pen = argv[++i];
    else if (!strcmp(argv[i], "--orient") && i + 1 < argc) orient_path = argv[++i];
    else if (!strcmp(argv[i], "--trace")) trace = 1;
    else { fprintf(stderr, "usage: pend [--pen DEV] [--orient FILE] [--trace]\n"); return 2; }
  }
  signal(SIGTERM, on_sig); signal(SIGINT, on_sig);
  int pf = open(pen, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  if (pf < 0) { fprintf(stderr, "pend: %s: %s\n", pen, strerror(errno)); return 1; }
  struct input_absinfo ax, ay, ap, ad;
  if (ioctl(pf, EVIOCGABS(ABS_X), &ax) || ioctl(pf, EVIOCGABS(ABS_Y), &ay) || ax.maximum <= 0 || ay.maximum <= 0) { fprintf(stderr, "pend: %s has no usable X/Y range\n", pen); return 1; }
  if (ioctl(pf, EVIOCGABS(ABS_PRESSURE), &ap) || ap.maximum <= 0) ap.maximum = 4095;
  if (ioctl(pf, EVIOCGABS(ABS_DISTANCE), &ad) || ad.maximum <= 0) ad.maximum = 255;

  ufd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
  if (ufd < 0) ufd = open("/dev/input/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
  if (ufd < 0) { fprintf(stderr, "pend: no /dev/uinput: %s\n", strerror(errno)); return 1; }
  ioctl(ufd, UI_SET_EVBIT, EV_KEY); ioctl(ufd, UI_SET_EVBIT, EV_ABS);
  int keys[] = { BTN_TOOL_PEN, BTN_TOOL_RUBBER, BTN_TOUCH, BTN_STYLUS, BTN_STYLUS2 };
  for (unsigned i = 0; i < sizeof keys / sizeof *keys; i++) ioctl(ufd, UI_SET_KEYBIT, keys[i]);
  ioctl(ufd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);
  abs_set(ABS_X, 0, 65535, 100); abs_set(ABS_Y, 0, 65535, 100);   // libinput drops a tablet whose axes have no resolution
  abs_set(ABS_PRESSURE, 0, ap.maximum, 0); abs_set(ABS_DISTANCE, 0, ad.maximum, 0);
  abs_set(ABS_TILT_X, -63, 63, 0); abs_set(ABS_TILT_Y, -63, 63, 0);
  struct uinput_setup us = { .id = { .bustype = BUS_USB, .vendor = 0x04e8, .product = 0x4e01, .version = 1 } };
  strcpy(us.name, "x400-pen");
  if (ioctl(ufd, UI_DEV_SETUP, &us) || ioctl(ufd, UI_DEV_CREATE)) { perror("pend: uinput create"); return 1; }
  char sys[64] = ""; ioctl(ufd, UI_GET_SYSNAME(sizeof sys), sys);
  char dp[128], devs[32] = ""; snprintf(dp, sizeof dp, "/sys/class/input/%s", sys);
  // /sys/class/input/inputN/eventM/dev holds "13:<minor>"
  for (int tries = 0; tries < 50 && !devs[0]; tries++) {
    char g[160]; for (int m = 64; m < 256 && !devs[0]; m++) { snprintf(g, sizeof g, "%s/event%d/dev", dp, m); FILE *f = fopen(g, "r"); if (f) { if (fgets(devs, sizeof devs, f)) devs[strcspn(devs, "\n")] = 0; fclose(f); } }
    if (!devs[0]) usleep(100000);
  }
  if (!devs[0]) { fprintf(stderr, "pend: no event node for %s\n", sys); return 1; }
  mkdir("/run/udev", 0755); mkdir("/run/udev/data", 0755);
  char db[96]; snprintf(db, sizeof db, "/run/udev/data/c%s", devs);
  FILE *f = fopen(db, "w");
  if (!f) { perror("pend: udev database"); return 1; }
  fprintf(f, "I:%lld\nE:ID_INPUT=1\nE:ID_INPUT_TABLET=1\nE:ID_INPUT_WIDTH_MM=250\nE:ID_INPUT_HEIGHT_MM=156\nG:seat\nQ:seat\n", (long long)time(NULL) * 1000000);
  fclose(f);
  fprintf(stderr, "pend: %s -> uinput %s (%s), udev entry %s, pressure 0..%d\n", pen, sys, devs, db, ap.maximum);
  read_orient();

  int x = 0, y = 0, pr = 0, dist = 0, tx = 0, ty = 0, touch = 0, stylus = 0, tool_pen = 0, tool_rub = 0, have_tool = 0, dirty = 0;
  int sent_pen = 0, sent_rub = 0;
  struct pollfd p = { .fd = pf, .events = POLLIN };
  time_t chk = 0;
  while (!stopping) {
    if (poll(&p, 1, 500) < 0 && errno != EINTR) break;
    if (time(NULL) != chk) { chk = time(NULL); read_orient(); }
    if (p.revents & (POLLERR | POLLHUP)) { fprintf(stderr, "pend: pen went away\n"); break; }
    struct input_event e;
    while (read(pf, &e, sizeof e) == sizeof e) {
      if (e.type == EV_ABS) {
        switch (e.code) {
        case ABS_X: x = e.value; dirty = 1; break; case ABS_Y: y = e.value; dirty = 1; break;
        case ABS_PRESSURE: pr = e.value; dirty = 1; break; case ABS_DISTANCE: dist = e.value; dirty = 1; break;
        case ABS_TILT_X: tx = e.value; dirty = 1; break; case ABS_TILT_Y: ty = e.value; dirty = 1; break;
        }
      } else if (e.type == EV_KEY) {
        switch (e.code) {
        case BTN_TOOL_PEN: tool_pen = e.value; have_tool = 1; dirty = 1; break;
        case BTN_TOOL_RUBBER: tool_rub = e.value; have_tool = 1; dirty = 1; break;
        case BTN_TOUCH: touch = e.value; dirty = 1; break;
        case BTN_STYLUS: stylus = e.value; dirty = 1; break;
        }
      } else if (e.type == EV_SYN && e.code == SYN_REPORT && dirty) {
        dirty = 0;
        // a pen that never reports a tool key is taken as in range while it touches or sends positions
        int in_pen = have_tool ? tool_pen : 1, in_rub = have_tool ? tool_rub : 0;
        if (in_rub) in_pen = 0;
        double fx = (double)x / ax.maximum, fy = (double)y / ay.maximum, ox, oy; turn(fx, fy, &ox, &oy);
        if (!in_pen && !in_rub) {   // leaving: tell libinput the tool went, once
          if (sent_pen) emit(EV_KEY, BTN_TOOL_PEN, 0);
          if (sent_rub) emit(EV_KEY, BTN_TOOL_RUBBER, 0);
          if (sent_pen || sent_rub) emit(EV_KEY, BTN_TOUCH, 0);
          if (sent_pen || sent_rub) emit(EV_SYN, SYN_REPORT, 0);
          sent_pen = sent_rub = 0; continue;
        }
        if (in_pen != sent_pen) { emit(EV_KEY, BTN_TOOL_PEN, in_pen); sent_pen = in_pen; }
        if (in_rub != sent_rub) { emit(EV_KEY, BTN_TOOL_RUBBER, in_rub); sent_rub = in_rub; }
        emit(EV_ABS, ABS_X, (int)(ox * 65535 + 0.5)); emit(EV_ABS, ABS_Y, (int)(oy * 65535 + 0.5));
        emit(EV_ABS, ABS_PRESSURE, touch ? pr : 0); emit(EV_ABS, ABS_DISTANCE, dist);
        emit(EV_ABS, ABS_TILT_X, tx); emit(EV_ABS, ABS_TILT_Y, ty);
        emit(EV_KEY, BTN_TOUCH, touch); emit(EV_KEY, BTN_STYLUS, stylus);
        emit(EV_SYN, SYN_REPORT, 0);
        if (trace) fprintf(stderr, "pend: %d,%d -> %.0f,%.0f pressure %d tilt %d,%d touch %d stylus %d tool %d/%d\n", x, y, ox * 65535, oy * 65535, pr, tx, ty, touch, stylus, in_pen, in_rub);
      }
    }
  }
  ioctl(ufd, UI_DEV_DESTROY); close(ufd); unlink(db);
  return 0;
}
