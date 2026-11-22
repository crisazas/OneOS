// Credit: the window copy follows OneiOS's runtime/display/src/jni.c (MIT), by Akshay CM (akshaynexus), Musicalista and contributors,
// https://github.com/Musicalista/OneiOS-A56. Written here, not copied. See x400/CREDITS.md.
// linux-x400: liboneios-style native half of X400Display: maps the frame file and copies each new frame into the layer.
#define _GNU_SOURCE
#include "fb.h"
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static volatile int stopping;
static void on_signal(int n) { (void)n; stopping = 1; }

JNIEXPORT jint JNICALL Java_X400Display_nRun(JNIEnv *env, jclass type, jobject surface, jstring file, jint width, jint height)
{
  (void)type;
  struct sigaction sa = { .sa_handler = on_signal };
  sigemptyset(&sa.sa_mask);
  sigaction(SIGTERM, &sa, NULL); sigaction(SIGINT, &sa, NULL); sigaction(SIGHUP, &sa, NULL);
  const char *path = (*env)->GetStringUTFChars(env, file, NULL);
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
  uint64_t shown = 0, frames = 0;
  struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
  while (!stopping) {
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
    if (frames % 300 == 0) {
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
