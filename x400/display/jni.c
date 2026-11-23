// Credit: the window copy follows OneiOS's runtime/display/src/jni.c (MIT), by Akshay CM (akshaynexus), Musicalista and contributors,
// https://github.com/Musicalista/OneiOS-A56. Written here, not copied. See x400/CREDITS.md.
// linux-x400: liboneios-style native half of X400Display: maps the frame file and copies each new frame into the layer.
#define _GNU_SOURCE
#include "fb.h"
#include "fdframe.h"
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static volatile int stopping;
static void on_signal(int n) { (void)n; stopping = 1; }

// Frames by descriptor (fdframe.h): capture keeps a PipeWire buffer and sends its memfd; the pixels are read from there.
struct fdbuf { uint32_t id; uint8_t *base; size_t size; };
static struct fdbuf fdbufs[32];

static int sock_open(const char *path)
{
  struct sockaddr_un sa = { .sun_family = AF_UNIX };
  if (snprintf(sa.sun_path, sizeof sa.sun_path, "%s.sock", path) >= (int)sizeof sa.sun_path) return -1;
  int s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (s < 0) return -1;
  if (connect(s, (struct sockaddr *)&sa, sizeof sa)) { close(s); return -1; }
  return s;
}

static void fdbuf_drop(uint32_t id)
{
  for (int i = 0; i < 32; i++) if (fdbufs[i].base && fdbufs[i].id == id) { munmap(fdbufs[i].base, fdbufs[i].size); memset(&fdbufs[i], 0, sizeof fdbufs[i]); }
}

static void fdbuf_clear(void) { for (int i = 0; i < 32; i++) if (fdbufs[i].base) fdbuf_drop(fdbufs[i].id); }

// 1: a frame was shown (or skipped), 0: nothing yet, -1: the socket is gone.
static int sock_frame(int s, ANativeWindow *win, int width, int height, uint64_t *frames)
{
  struct fd_msg m; struct iovec iov = { &m, sizeof m };
  char cb[CMSG_SPACE(sizeof(int))];
  struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = cb, .msg_controllen = sizeof cb };
  ssize_t n = recvmsg(s, &mh, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
  if (n == 0) return -1;
  if (n < 0) return errno == EAGAIN || errno == EINTR ? 0 : -1;
  int fd = -1;
  for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c))
    if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) memcpy(&fd, CMSG_DATA(c), sizeof fd);
  if (n != (ssize_t)sizeof m) { if (fd >= 0) close(fd); return 0; }
  if (m.type == FDM_DROP) { fdbuf_drop(m.id); if (fd >= 0) close(fd); return 0; }
  if (m.type != FDM_FRAME) { if (fd >= 0) close(fd); return 0; }
  struct fdbuf *fb = NULL;
  for (int i = 0; i < 32; i++) if (fdbufs[i].base && fdbufs[i].id == m.id) fb = &fdbufs[i];
  if (!fb && fd >= 0) {
    size_t size = (size_t)m.mapoffset + m.maxsize;
    uint8_t *base = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    if (base != MAP_FAILED) for (int i = 0; i < 32; i++) if (!fdbufs[i].base) { fdbufs[i] = (struct fdbuf){ m.id, base, size }; fb = &fdbufs[i]; break; }
    if (!fb && base != MAP_FAILED) munmap(base, size);
  }
  if (fd >= 0) close(fd);
  struct fd_reply r = { FDM_DONE, m.id };
  if (fb && m.width == (uint32_t)width && m.height == (uint32_t)height) {
    ANativeWindow_Buffer b;
    if (ANativeWindow_lock(win, &b, NULL) >= 0) {
      const uint8_t *src = fb->base + m.mapoffset + m.offset;
      uint32_t rows = (uint32_t)height < (uint32_t)b.height ? (uint32_t)height : (uint32_t)b.height, row = (uint32_t)width * 4;
      for (uint32_t y = 0; y < rows; y++) memcpy((uint8_t *)b.bits + (size_t)y * b.stride * 4, src + (size_t)y * m.stride, row);
      ANativeWindow_unlockAndPost(win);
      (*frames)++;
    }
  }
  if (send(s, &r, sizeof r, MSG_NOSIGNAL) != (ssize_t)sizeof r) return -1;
  return 1;
}

JNIEXPORT jint JNICALL Java_X400Display_nRun(JNIEnv *env, jclass type, jobject surface, jstring file, jint width, jint height)
{
  (void)type;
  struct sigaction sa = { .sa_handler = on_signal };
  sigemptyset(&sa.sa_mask);
  sigaction(SIGTERM, &sa, NULL); sigaction(SIGINT, &sa, NULL); sigaction(SIGHUP, &sa, NULL);
  const char *path = (*env)->GetStringUTFChars(env, file, NULL);
  char path_copy[256]; snprintf(path_copy, sizeof path_copy, "%s", path);
  // The guest creates the file when its capture starts, which can be after the layer is up: wait for it (up to 5 minutes).
  int fd = -1;
  for (int waited = 0; fd < 0 && waited < 3000 && !stopping; waited++) {
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) usleep(100000);
  }
  if (fd < 0) { fprintf(stderr, "x400-display: %s: %s\n", path, stopping ? "stopped while waiting" : strerror(errno)); (*env)->ReleaseStringUTFChars(env, file, path); return -1; }
  (*env)->ReleaseStringUTFChars(env, file, path);
  struct stat st; fstat(fd, &st);
  size_t size = (size_t)st.st_size;
  uint8_t *map = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
  if (map == MAP_FAILED || size < FB_PIXELS) { fprintf(stderr, "x400-display: cannot map the frame file (%zu bytes)\n", size); return -1; }
  const struct fb_header *h = (const struct fb_header *)map;
  ANativeWindow *win = ANativeWindow_fromSurface(env, surface);
  if (!win) return -1;
  if (ANativeWindow_setBuffersGeometry(win, width, height, 5 /* BGRA_8888 */) < 0) fprintf(stderr, "x400-display: setBuffersGeometry failed\n");
  uint64_t shown = 0, frames = 0, last_frames = 0;
  struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
  int sock = -1, tries = 0; int by_fd = getenv("X400_NOFD") == NULL;
  while (!stopping) {
    if (by_fd && sock < 0 && tries++ % 500 == 0 && (sock = sock_open(path_copy)) >= 0) fprintf(stderr, "x400-display: frames by descriptor from capture\n");
    if (sock >= 0) {
      struct pollfd pf = { sock, POLLIN, 0 };
      if (poll(&pf, 1, 100) > 0) {
        int r = sock_frame(sock, win, width, height, &frames);
        if (r < 0) { close(sock); sock = -1; fdbuf_clear(); fprintf(stderr, "x400-display: the capture socket is gone: frame file again\n"); }
      }
      if (sock >= 0) goto stats;
    }
    uint64_t seq = __atomic_load_n(&h->seq, __ATOMIC_ACQUIRE);
    if (seq == shown || h->magic != FB_MAGIC || h->width != (uint32_t)width || h->height != (uint32_t)height) { usleep(2000); continue; }
    ANativeWindow_Buffer b;
    if (ANativeWindow_lock(win, &b, NULL) < 0) { usleep(5000); continue; }
    const uint8_t *src = map + FB_PIXELS;
    uint32_t rows = (uint32_t)height < (uint32_t)b.height ? (uint32_t)height : (uint32_t)b.height;
    uint32_t row = (uint32_t)width * 4;
    for (uint32_t y = 0; y < rows; y++) memcpy((uint8_t *)b.bits + (size_t)y * b.stride * 4, src + (size_t)y * h->stride, row);
    ANativeWindow_unlockAndPost(win);
    shown = seq; frames++;
  stats:
    if (frames - last_frames >= 300) {
      last_frames = frames;
      struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
      double dt = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;
      fprintf(stderr, "x400-display: %llu frames shown, %.1f fps over the last %.1f s\n", (unsigned long long)frames, 300 / dt, dt);
      t0 = t1;
    }
  }
  ANativeWindow_release(win);
  munmap(map, size); close(fd);
  return 0;
}
