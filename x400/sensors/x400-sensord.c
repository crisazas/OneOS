// x400-sensord: accelerometer + ambient light of the SM-X400 for the Linux guest, without Android's sensor stack.
//
// The sensor hub (shub) keeps a sensor idle until somebody enables it and Android's sensorservice is not running, so the
// IIO buffers stay silent. The Samsung sysfs does work without the HAL:
//   printf 11 > /sys/class/sensors/ssp_sensor/enable      type 1 (accelerometer) on   ("10" off; "51"/"50" is the light sensor)
//   cat /sys/class/sensors/accelerometer_sensor/raw_data  "x,y,z" in LSB, 4096 LSB = 1 g (LSM6DSVTR)
//   cat /sys/class/sensors/light_sensor/lux               "lux,..." (STK61A10)
// Two jobs, both optional:
//   - D-Bus: serves net.hadess.SensorProxy (what KWin 6 uses for auto-rotate and light) on the "system" bus, which libdbus
//     resolves through DBUS_SYSTEM_BUS_ADDRESS, so the guest points it at its session bus (see RESULT.md).
//   - --rotate: turns the orientation into `kscreen-doctor output.N.rotation.R` itself and runs --hook with the touch
//     rotation, so it works even when KWin's own auto-rotate is not wired.
// Build: aarch64-linux-gnu-gcc -O2 -o x400-sensord x400-sensord.c $(dbus include dirs) -l:libdbus-1.so.3 -lm

#include <dbus/dbus.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PATH "/net/hadess/SensorProxy"
#define IFACE "net.hadess.SensorProxy"
#define NAME "net.hadess.SensorProxy"

static const char *sysfs = "/sys/class/sensors";
static const char *accel_file;            // test override: read "x,y,z" from this file instead of the sysfs
static const char *light_file;
static const char *axes_spec = "x,y,z";   // --axes remaps the sensor axes (e.g. "y,-x,z") if a unit is mounted differently
static const char *output = "1";
static const char *hook;
static int do_rotate, do_dbus = 1, print_only, period_ms = 200, verbose;
static volatile sig_atomic_t stopping;

enum { O_UNDEF, O_NORMAL, O_BOTTOM_UP, O_LEFT_UP, O_RIGHT_UP };
static const char *const orient_name[] = { "undefined", "normal", "bottom-up", "left-up", "right-up" };
// KScreen rotation, inputd --rot and logical size per orientation. "normal" is the landscape the session starts in
// (inputd rot 1: the desktop top edge is the touch panel's +X edge).
static const char *const rot_name[] = { "none", "none", "inverted", "left", "right" };
static const int touch_rot[] = { 1, 1, 3, 0, 2 };
static const int logical_w[] = { 2112, 2112, 2112, 1320, 1320 };
static const int logical_h[] = { 1320, 1320, 1320, 2112, 2112 };

static void on_sig(int n) { (void)n; stopping = 1; }
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static int read_text(const char *path, char *buf, size_t n)
{
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return -1;
  ssize_t r = read(fd, buf, n - 1);
  close(fd);
  if (r < 0) return -1;
  buf[r] = 0;
  return 0;
}

static int sensor_enable(int type, int on)
{
  char p[256], v[16];
  snprintf(p, sizeof p, "%s/ssp_sensor/enable", sysfs);
  int fd = open(p, O_WRONLY | O_CLOEXEC);
  if (fd < 0) return -1;
  int len = snprintf(v, sizeof v, "%d%d", type, on);
  ssize_t w = write(fd, v, len);   // one write, no newline: the driver parses "<type><0|1>"
  close(fd);
  return w == len ? 0 : -1;
}

struct axis { int src, sign; };
static struct axis axes[3] = { {0, 1}, {1, 1}, {2, 1} };

static void parse_axes(const char *s)
{
  char t[64]; strncpy(t, s, sizeof t - 1); t[sizeof t - 1] = 0;
  char *save, *tok = strtok_r(t, ",", &save);
  for (int i = 0; i < 3 && tok; i++, tok = strtok_r(NULL, ",", &save)) {
    int sign = 1; if (*tok == '-') { sign = -1; tok++; }
    axes[i].src = (*tok == 'y') ? 1 : (*tok == 'z') ? 2 : 0;
    axes[i].sign = sign;
  }
}

static int read_accel(double g[3])
{
  char b[128], p[256]; long v[3];
  if (accel_file) snprintf(p, sizeof p, "%s", accel_file); else snprintf(p, sizeof p, "%s/accelerometer_sensor/raw_data", sysfs);
  if (read_text(p, b, sizeof b) < 0 || sscanf(b, "%ld,%ld,%ld", &v[0], &v[1], &v[2]) != 3) return -1;
  for (int i = 0; i < 3; i++) g[i] = axes[i].sign * (double)v[axes[i].src] / 4096.0;
  return 0;
}

static int read_lux(double *lux)
{
  char b[128], p[256]; double v;
  if (light_file) snprintf(p, sizeof p, "%s", light_file); else snprintf(p, sizeof p, "%s/light_sensor/lux", sysfs);
  if (read_text(p, b, sizeof b) < 0 || sscanf(b, "%lf", &v) != 1) return -1;
  *lux = v;
  return 0;
}

// g[] is the "up" vector in the touch panel frame (X, Y in the portrait plane, Z out of the screen). Angle 0 = +X up
// (the desktop's normal landscape). A new orientation needs the angle to leave the current 120-degree sector, and a tablet
// lying flat (|z| > 0.85 g) changes nothing.
static int classify(const double g[3], int cur)
{
  if (fabs(g[2]) > 0.85) return cur;
  double a = atan2(g[1], g[0]) * 180.0 / M_PI;
  static const double center[] = { 0, 0, 180, 90, -90 };
  if (cur != O_UNDEF) {
    double d = fabs(a - center[cur]); if (d > 180) d = 360 - d;
    if (d < 60) return cur;
  }
  if (fabs(a) < 45) return O_NORMAL;
  if (fabs(a) > 135) return O_BOTTOM_UP;
  return a > 0 ? O_LEFT_UP : O_RIGHT_UP;
}

static void run(char *const argv[])
{
  pid_t p = fork();
  if (p == 0) { execvp(argv[0], argv); _exit(127); }
}

static void apply_rotation(int o)
{
  char arg[128], rot[8], w[16], h[16];
  snprintf(arg, sizeof arg, "output.%s.rotation.%s", output, rot_name[o]);
  fprintf(stderr, "x400-sensord: orientation %s -> kscreen-doctor %s\n", orient_name[o], arg);
  char *kd[] = { "kscreen-doctor", arg, NULL };
  run(kd);
  if (hook) {
    snprintf(rot, sizeof rot, "%d", touch_rot[o]); snprintf(w, sizeof w, "%d", logical_w[o]); snprintf(h, sizeof h, "%d", logical_h[o]);
    char *hk[] = { (char *)hook, (char *)orient_name[o], (char *)rot_name[o], rot, w, h, NULL };
    run(hk);
  }
}

static DBusConnection *conn;
static char accel_claimers[8][64], light_claimers[8][64];
static int cur_orient = O_UNDEF;
static double cur_lux = 0;
static int have_light = 1;

static int claim_count(char c[8][64]) { int n = 0; for (int i = 0; i < 8; i++) if (c[i][0]) n++; return n; }
static void claim_add(char c[8][64], const char *s) { for (int i = 0; i < 8; i++) if (!strcmp(c[i], s)) return; for (int i = 0; i < 8; i++) if (!c[i][0]) { snprintf(c[i], 64, "%s", s); return; } }
static void claim_del(char c[8][64], const char *s) { for (int i = 0; i < 8; i++) if (!strcmp(c[i], s)) c[i][0] = 0; }

static const char *const introspect =
  "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\" \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
  "<node><interface name=\"org.freedesktop.DBus.Introspectable\"><method name=\"Introspect\"><arg type=\"s\" direction=\"out\"/></method></interface>\n"
  "<interface name=\"org.freedesktop.DBus.Properties\">"
  "<method name=\"Get\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"v\" direction=\"out\"/></method>"
  "<method name=\"GetAll\"><arg type=\"s\" direction=\"in\"/><arg type=\"a{sv}\" direction=\"out\"/></method>"
  "<signal name=\"PropertiesChanged\"><arg type=\"s\"/><arg type=\"a{sv}\"/><arg type=\"as\"/></signal></interface>\n"
  "<interface name=\"" IFACE "\">"
  "<method name=\"ClaimAccelerometer\"/><method name=\"ReleaseAccelerometer\"/><method name=\"ClaimLight\"/><method name=\"ReleaseLight\"/>"
  "<method name=\"ClaimProximity\"/><method name=\"ReleaseProximity\"/><method name=\"ClaimCompass\"/><method name=\"ReleaseCompass\"/>"
  "<property name=\"HasAccelerometer\" type=\"b\" access=\"read\"/><property name=\"AccelerometerOrientation\" type=\"s\" access=\"read\"/>"
  "<property name=\"HasAmbientLight\" type=\"b\" access=\"read\"/><property name=\"LightLevelUnit\" type=\"s\" access=\"read\"/>"
  "<property name=\"LightLevel\" type=\"d\" access=\"read\"/><property name=\"HasProximity\" type=\"b\" access=\"read\"/>"
  "<property name=\"ProximityNear\" type=\"b\" access=\"read\"/><property name=\"HasCompass\" type=\"b\" access=\"read\"/></interface></node>";

enum { T_BOOL, T_STR, T_DBL };
static const struct { const char *name; int type; } props[] = {
  { "HasAccelerometer", T_BOOL }, { "AccelerometerOrientation", T_STR }, { "HasAmbientLight", T_BOOL },
  { "LightLevelUnit", T_STR }, { "LightLevel", T_DBL }, { "HasProximity", T_BOOL }, { "ProximityNear", T_BOOL }, { "HasCompass", T_BOOL },
};
#define NPROPS ((int)(sizeof props / sizeof props[0]))

static void append_value(DBusMessageIter *it, int idx)
{
  DBusMessageIter v;
  const char *n = props[idx].name;
  if (props[idx].type == T_BOOL) {
    dbus_bool_t b = !strcmp(n, "HasAccelerometer") ? 1 : !strcmp(n, "HasAmbientLight") ? have_light : 0;
    dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, "b", &v); dbus_message_iter_append_basic(&v, DBUS_TYPE_BOOLEAN, &b);
  } else if (props[idx].type == T_STR) {
    const char *s = !strcmp(n, "AccelerometerOrientation") ? orient_name[cur_orient] : "lux";
    dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, "s", &v); dbus_message_iter_append_basic(&v, DBUS_TYPE_STRING, &s);
  } else {
    dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, "d", &v); dbus_message_iter_append_basic(&v, DBUS_TYPE_DOUBLE, &cur_lux);
  }
  dbus_message_iter_close_container(it, &v);
}

static void emit_changed(int idx)
{
  DBusMessage *m = dbus_message_new_signal(PATH, "org.freedesktop.DBus.Properties", "PropertiesChanged");
  DBusMessageIter it, arr, ent; const char *iface = IFACE; const char *name = props[idx].name;
  dbus_message_iter_init_append(m, &it);
  dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &iface);
  dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &arr);
  dbus_message_iter_open_container(&arr, DBUS_TYPE_DICT_ENTRY, NULL, &ent);
  dbus_message_iter_append_basic(&ent, DBUS_TYPE_STRING, &name);
  append_value(&ent, idx);
  dbus_message_iter_close_container(&arr, &ent);
  dbus_message_iter_close_container(&it, &arr);
  dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "s", &arr);
  dbus_message_iter_close_container(&it, &arr);
  dbus_connection_send(conn, m, NULL);
  dbus_message_unref(m);
}

static void update_sensors(void)
{
  static int accel_on, light_on, stable_n, cand = O_UNDEF;
  int want_accel = claim_count(accel_claimers) > 0 || do_rotate || print_only;
  int want_light = claim_count(light_claimers) > 0 || print_only;
  if (want_accel != accel_on) { if (!accel_file) sensor_enable(1, want_accel); accel_on = want_accel; if (verbose) fprintf(stderr, "accelerometer %s\n", want_accel ? "on" : "off"); }
  if (want_light != light_on) { if (!light_file) sensor_enable(5, want_light); light_on = want_light; }
  if (accel_on) {
    double g[3];
    if (read_accel(g) == 0) {
      int o = classify(g, cur_orient);
      if (verbose) fprintf(stderr, "g=%+.3f,%+.3f,%+.3f -> %s\n", g[0], g[1], g[2], orient_name[o]);
      if (o != cur_orient) {
        if (o == cand) stable_n++; else { cand = o; stable_n = 1; }
        if (stable_n >= 3 || cur_orient == O_UNDEF) {   // 3 samples agree, or the very first one
          cur_orient = o; stable_n = 0;
          if (do_rotate) apply_rotation(o);
          if (conn) emit_changed(1);
        }
      } else stable_n = 0;
    }
  }
  if (light_on) {
    double l;
    if (read_lux(&l) == 0 && fabs(l - cur_lux) >= 1) { cur_lux = l; if (conn) emit_changed(4); }
  }
}

static void reply_error(DBusMessage *m, const char *err, const char *txt)
{
  DBusMessage *r = dbus_message_new_error(m, err, txt); dbus_connection_send(conn, r, NULL); dbus_message_unref(r);
}

static DBusHandlerResult filter(DBusConnection *c, DBusMessage *m, void *u)
{
  (void)c; (void)u;
  const char *iface = dbus_message_get_interface(m), *member = dbus_message_get_member(m), *sender = dbus_message_get_sender(m);
  if (dbus_message_is_signal(m, DBUS_INTERFACE_DBUS, "NameOwnerChanged")) {
    const char *n, *o, *nw;
    if (dbus_message_get_args(m, NULL, DBUS_TYPE_STRING, &n, DBUS_TYPE_STRING, &o, DBUS_TYPE_STRING, &nw, DBUS_TYPE_INVALID) && !*nw) { claim_del(accel_claimers, n); claim_del(light_claimers, n); }
    return DBUS_HANDLER_RESULT_HANDLED;
  }
  if (dbus_message_get_type(m) != DBUS_MESSAGE_TYPE_METHOD_CALL || !dbus_message_has_path(m, PATH)) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  DBusMessage *r = NULL;
  if (iface && !strcmp(iface, "org.freedesktop.DBus.Introspectable")) {
    r = dbus_message_new_method_return(m); const char *x = introspect; dbus_message_append_args(r, DBUS_TYPE_STRING, &x, DBUS_TYPE_INVALID);
  } else if (iface && !strcmp(iface, "org.freedesktop.DBus.Properties")) {
    if (!strcmp(member, "Get")) {
      const char *i, *n; int idx = -1;
      dbus_message_get_args(m, NULL, DBUS_TYPE_STRING, &i, DBUS_TYPE_STRING, &n, DBUS_TYPE_INVALID);
      for (int k = 0; k < NPROPS; k++) if (n && !strcmp(n, props[k].name)) idx = k;
      if (idx < 0) { reply_error(m, DBUS_ERROR_UNKNOWN_PROPERTY, "no such property"); return DBUS_HANDLER_RESULT_HANDLED; }
      DBusMessageIter it; r = dbus_message_new_method_return(m); dbus_message_iter_init_append(r, &it); append_value(&it, idx);
    } else if (!strcmp(member, "GetAll")) {
      DBusMessageIter it, arr, ent; r = dbus_message_new_method_return(m); dbus_message_iter_init_append(r, &it);
      dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &arr);
      for (int k = 0; k < NPROPS; k++) {
        const char *n = props[k].name;
        dbus_message_iter_open_container(&arr, DBUS_TYPE_DICT_ENTRY, NULL, &ent);
        dbus_message_iter_append_basic(&ent, DBUS_TYPE_STRING, &n); append_value(&ent, k);
        dbus_message_iter_close_container(&arr, &ent);
      }
      dbus_message_iter_close_container(&it, &arr);
    }
  } else if (iface && !strcmp(iface, IFACE)) {
    if (!strcmp(member, "ClaimAccelerometer")) claim_add(accel_claimers, sender);
    else if (!strcmp(member, "ReleaseAccelerometer")) claim_del(accel_claimers, sender);
    else if (!strcmp(member, "ClaimLight")) claim_add(light_claimers, sender);
    else if (!strcmp(member, "ReleaseLight")) claim_del(light_claimers, sender);
    r = dbus_message_new_method_return(m);   // Proximity/Compass claims are accepted and ignored: HasProximity/HasCompass are false
    update_sensors();
  }
  if (!r) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  dbus_connection_send(conn, r, NULL); dbus_message_unref(r);
  return DBUS_HANDLER_RESULT_HANDLED;
}

int main(int argc, char **argv)
{
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--rotate")) do_rotate = 1;
    else if (!strcmp(argv[i], "--no-dbus")) do_dbus = 0;
    else if (!strcmp(argv[i], "--print")) { print_only = 1; do_dbus = 0; }
    else if (!strcmp(argv[i], "-v")) verbose = 1;
    else if (!strcmp(argv[i], "--output") && i + 1 < argc) output = argv[++i];
    else if (!strcmp(argv[i], "--hook") && i + 1 < argc) hook = argv[++i];
    else if (!strcmp(argv[i], "--axes") && i + 1 < argc) axes_spec = argv[++i];
    else if (!strcmp(argv[i], "--period") && i + 1 < argc) period_ms = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--sysfs") && i + 1 < argc) sysfs = argv[++i];
    else if (!strcmp(argv[i], "--accel-file") && i + 1 < argc) accel_file = argv[++i];
    else if (!strcmp(argv[i], "--light-file") && i + 1 < argc) light_file = argv[++i];
    else { fprintf(stderr, "usage: x400-sensord [--rotate] [--no-dbus] [--print] [--output N] [--hook CMD] [--axes x,y,z] [--period ms] [-v]\n"
                           "  hook args: <orientation> <kscreen rotation> <inputd --rot> <logical width> <logical height>\n"); return 2; }
  }
  parse_axes(axes_spec);
  signal(SIGINT, on_sig); signal(SIGTERM, on_sig); signal(SIGCHLD, SIG_IGN);
  if (do_dbus) {
    DBusError err; dbus_error_init(&err);
    conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
    if (!conn) { fprintf(stderr, "x400-sensord: D-Bus: %s\n", err.message); return 1; }
    int rc = dbus_bus_request_name(conn, NAME, DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);
    if (rc != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) { fprintf(stderr, "x400-sensord: cannot own %s: %s\n", NAME, err.message ? err.message : "name taken"); return 1; }
    dbus_bus_add_match(conn, "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged'", NULL);
    dbus_connection_add_filter(conn, filter, NULL, NULL);
    fprintf(stderr, "x400-sensord: serving %s on %s\n", NAME, getenv("DBUS_SYSTEM_BUS_ADDRESS") ? getenv("DBUS_SYSTEM_BUS_ADDRESS") : "the system bus");
  }
  double next = 0;
  while (!stopping) {
    if (conn) { if (!dbus_connection_read_write_dispatch(conn, period_ms)) break; } else usleep(period_ms * 1000);
    double t = now_s();
    if (t >= next) {
      update_sensors();
      next = t + period_ms / 1000.0;
      if (print_only) {
        double g[3] = {0, 0, 0}, l = 0; int a = read_accel(g), b = read_lux(&l);
        printf("accel g=%+.3f,%+.3f,%+.3f%s orientation=%s lux=%.0f%s\n", g[0], g[1], g[2], a ? " (unreadable)" : "", orient_name[cur_orient], l, b ? " (unreadable)" : "");
        fflush(stdout);
        static int n; if (++n >= 5) break;
      }
    }
  }
  if (!accel_file) sensor_enable(1, 0);
  if (!light_file) sensor_enable(5, 0);
  return 0;
}
