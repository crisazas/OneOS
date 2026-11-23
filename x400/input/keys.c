// linux-x400: side keys. Opens event0 (gpio_keys, Volume Up) and event1 (s2mpu15-power-keys, Volume Down + Power) O_RDONLY and
// NEVER takes EVIOCGRAB: One UI's return shortcut (Volume Up + Volume Down for 3 s, watched by OneOS/x400_keywatch) keeps seeing every event.
// Volume Up/Down -> wpctl set-volume, on release, or on each auto-repeat while held; if both were held at once nothing is changed,
// so the return shortcut does not also move the volume. Power, a short press -> backlight off, the next short press -> backlight back to what it was.
// A press longer than --short ms (default 800) is ignored. Usage: keys [--step 5] [--short 800] [--up DEV] [--down DEV] [--bl DIR] [--quiet]
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile int stopping;
static void on_sig(int n) { (void)n; stopping = 1; }
static void on_chld(int n) { (void)n; while (waitpid(-1, NULL, WNOHANG) > 0) {} }

static long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000 + t.tv_nsec / 1000000; }

static void run(char *const argv[])
{
  pid_t p = fork();
  if (p == 0) { setsid(); int z = open("/dev/null", O_RDWR); dup2(z, 0); dup2(z, 1); dup2(z, 2); execvp(argv[0], argv); _exit(127); }
}

static int bl_read(const char *dir, const char *name)
{
  char path[256], buf[32]; snprintf(path, sizeof path, "%s/%s", dir, name);
  int fd = open(path, O_RDONLY); if (fd < 0) return -1;
  ssize_t n = read(fd, buf, sizeof buf - 1); close(fd); if (n <= 0) return -1; buf[n] = 0; return atoi(buf);
}

static int bl_write(const char *dir, int v)
{
  char path[256], buf[32]; snprintf(path, sizeof path, "%s/brightness", dir);
  int fd = open(path, O_WRONLY); if (fd < 0) return -1;
  int n = snprintf(buf, sizeof buf, "%d\n", v); int r = write(fd, buf, n) == n ? 0 : -1; close(fd); return r;
}

int main(int argc, char **argv)
{
  const char *dev[2] = { "/dev/input/event0", "/dev/input/event1" }, *bl = "/sys/class/backlight/panel";
  int step = 5, shortms = 800, quiet = 0;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--step") && i + 1 < argc) step = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--short") && i + 1 < argc) shortms = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--up") && i + 1 < argc) dev[0] = argv[++i];
    else if (!strcmp(argv[i], "--down") && i + 1 < argc) dev[1] = argv[++i];
    else if (!strcmp(argv[i], "--bl") && i + 1 < argc) bl = argv[++i];
    else if (!strcmp(argv[i], "--quiet")) quiet = 1;
    else { fprintf(stderr, "usage: keys [--step N] [--short MS] [--up DEV] [--down DEV] [--bl DIR] [--quiet]\n"); return 2; }
  }
  struct sigaction sa = { .sa_handler = on_sig }; sigemptyset(&sa.sa_mask);
  sigaction(SIGTERM, &sa, NULL); sigaction(SIGINT, &sa, NULL); sigaction(SIGHUP, &sa, NULL);
  struct sigaction sc = { .sa_handler = on_chld, .sa_flags = SA_RESTART }; sigemptyset(&sc.sa_mask); sigaction(SIGCHLD, &sc, NULL);
  struct pollfd pf[2];
  for (int i = 0; i < 2; i++) {
    if (i && !strcmp(dev[0], dev[1])) { pf[i].fd = -1; continue; }
    pf[i].fd = open(dev[i], O_RDONLY | O_CLOEXEC | O_NONBLOCK); pf[i].events = POLLIN;
    if (pf[i].fd < 0) { fprintf(stderr, "keys: %s: %s\n", dev[i], strerror(errno)); return 1; }
  }
  int held[2] = { 0, 0 }, repeated[2] = { 0, 0 }, combo = 0, power = 0, off = 0, saved = -1; long pdown = 0;
  char pct_up[16], pct_down[16]; snprintf(pct_up, sizeof pct_up, "%d%%+", step); snprintf(pct_down, sizeof pct_down, "%d%%-", step);
  if (!quiet) fprintf(stderr, "keys: reading %s and %s without grab, backlight %s\n", dev[0], dev[1], bl);
  while (!stopping) {
    if (poll(pf, 2, 500) < 0 && errno != EINTR) break;
    for (int i = 0; i < 2; i++) {
      if (!(pf[i].revents & POLLIN)) continue;
      struct input_event e;
      while (read(pf[i].fd, &e, sizeof e) == sizeof e) {
        if (e.type != EV_KEY) continue;
        if (e.code == KEY_VOLUMEUP || e.code == KEY_VOLUMEDOWN) {
          int k = e.code == KEY_VOLUMEDOWN;
          char *const a[] = { "wpctl", "set-volume", "-l", "1.0", "@DEFAULT_AUDIO_SINK@", k ? pct_down : pct_up, NULL };
          if (e.value == 1) { held[k] = 1; repeated[k] = 0; if (held[0] && held[1]) combo = 1; }
          else if (e.value == 2) { if (!combo) { repeated[k] = 1; run(a); } }
          else { held[k] = 0; if (!combo && !repeated[k]) run(a); if (!held[0] && !held[1]) combo = 0; }
        } else if (e.code == KEY_POWER) {
          if (e.value == 1) { power = 1; pdown = now_ms(); }
          else if (e.value == 0 && power) {
            power = 0;
            if (now_ms() - pdown > shortms) continue;
            if (!off) { saved = bl_read(bl, "brightness"); if (saved <= 0) saved = bl_read(bl, "max_brightness") / 4; if (bl_write(bl, 0) == 0) off = 1; }
            else { bl_write(bl, saved > 0 ? saved : 1); off = 0; }
            if (!quiet) fprintf(stderr, "keys: power -> screen %s\n", off ? "off" : "on");
          }
        }
      }
    }
  }
  if (off && saved > 0) bl_write(bl, saved);
  for (int i = 0; i < 2; i++) if (pf[i].fd >= 0) close(pf[i].fd);
  return 0;
}
