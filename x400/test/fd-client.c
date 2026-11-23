// linux-x400: stand-in for the display helper when frames go by descriptor (x400/display/fdframe.h). Connects to capture's FILE.sock,
// maps each buffer once, reads every pixel row of each frame (a checksum, so the read is real) and answers DONE.
// Counts frames per second and the frames whose first and last row differ in age is not detectable here, so it only reports rates.
// Usage: fd-client FILE.sock [SECONDS]   (build with the NDK: aarch64-linux-android30-clang -O2 -o fd-client fd-client.c)
#define _GNU_SOURCE
#include "../display/fdframe.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

int main(int argc, char **argv)
{
  if (argc < 2) return 2;
  double secs = argc > 2 ? atof(argv[2]) : 10;
  struct sockaddr_un sa = { .sun_family = AF_UNIX }; snprintf(sa.sun_path, sizeof sa.sun_path, "%s", argv[1]);
  int s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (s < 0 || connect(s, (struct sockaddr *)&sa, sizeof sa)) { perror("connect"); return 1; }
  struct { uint32_t id; uint8_t *base; size_t size; } bufs[32] = {0};
  double t0 = now(); unsigned long frames = 0, maps = 0, sum_all = 0; double busy = 0;
  while (now() - t0 < secs) {
    struct fd_msg m; struct iovec iov = { &m, sizeof m }; char cb[CMSG_SPACE(sizeof(int))];
    struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = cb, .msg_controllen = sizeof cb };
    ssize_t n = recvmsg(s, &mh, 0);
    if (n <= 0) { fprintf(stderr, "socket closed (%zd)\n", n); break; }
    int fd = -1;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) if (c->cmsg_type == SCM_RIGHTS) memcpy(&fd, CMSG_DATA(c), sizeof fd);
    if (m.type == FDM_DROP) { for (int i = 0; i < 32; i++) if (bufs[i].base && bufs[i].id == m.id) { munmap(bufs[i].base, bufs[i].size); bufs[i].base = NULL; } if (fd >= 0) close(fd); continue; }
    int k = -1; for (int i = 0; i < 32; i++) if (bufs[i].base && bufs[i].id == m.id) k = i;
    if (k < 0 && fd >= 0) {
      for (int i = 0; i < 32; i++) if (!bufs[i].base) { k = i; break; }
      size_t size = (size_t)m.mapoffset + m.maxsize; uint8_t *b = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
      if (b == MAP_FAILED) { perror("mmap"); return 1; }
      bufs[k].id = m.id; bufs[k].base = b; bufs[k].size = size; maps++;
    }
    if (fd >= 0) close(fd);
    double b0 = now();
    if (k >= 0) { const uint32_t *p = (const uint32_t *)(bufs[k].base + m.mapoffset + m.offset); unsigned long x = 0; size_t words = (size_t)m.stride / 4 * m.height; for (size_t i = 0; i < words; i += 16) x += p[i]; sum_all += x; }
    busy += now() - b0;
    struct fd_reply r = { FDM_DONE, m.id }; if (send(s, &r, sizeof r, MSG_NOSIGNAL) != (ssize_t)sizeof r) break;
    frames++;
  }
  double dt = now() - t0;
  printf("fd-client: %lu frames in %.1f s = %.1f fps, %lu buffers mapped, reading took %.1f ms per frame (checksum %lu)\n", frames, dt, frames / dt, maps, frames ? busy * 1000 / frames : 0, sum_all);
  return 0;
}
