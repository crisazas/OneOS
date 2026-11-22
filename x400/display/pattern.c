// linux-x400: stands in for the guest: writes a moving test pattern into the frame file (fb.h). Usage: pattern FILE W H [seconds]
#define _GNU_SOURCE
#include "fb.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
int main(int argc, char **argv)
{
  if (argc < 4) return 2;
  uint32_t w = atoi(argv[2]), hgt = atoi(argv[3]); int secs = argc > 4 ? atoi(argv[4]) : 10;
  size_t size = FB_PIXELS + (size_t)w * hgt * 4;
  int fd = open(argv[1], O_RDWR | O_CREAT, 0666);
  if (fd < 0 || ftruncate(fd, size)) { perror("frame file"); return 1; }
  uint8_t *m = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  struct fb_header *h = (struct fb_header *)m;
  h->magic = FB_MAGIC; h->width = w; h->height = hgt; h->stride = w * 4; h->format = 5;
  uint8_t *px = m + FB_PIXELS; uint64_t seq = 0;
  struct timespec t0, t; clock_gettime(CLOCK_MONOTONIC, &t0);
  for (;;) {
    clock_gettime(CLOCK_MONOTONIC, &t);
    double el = (t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) * 1e-9;
    if (el > secs) break;
    uint32_t bar = (uint32_t)(el * 400) % w;
    for (uint32_t y = 0; y < hgt; y++) {
      uint8_t *r = px + (size_t)y * w * 4;
      for (uint32_t x = 0; x < w; x++) {
        uint8_t *p = r + x * 4;
        int on = x > bar && x < bar + 60;
        p[0] = on ? 255 : (y * 255 / hgt); p[1] = on ? 255 : 40; p[2] = on ? 255 : (x * 255 / w); p[3] = 255;
      }
    }
    __atomic_store_n(&h->seq, ++seq, __ATOMIC_RELEASE);
    usleep(16000);
  }
  printf("wrote %llu frames\n", (unsigned long long)seq);
  return 0;
}
