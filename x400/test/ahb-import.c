// linux-x400: can a dma-buf the guest made become an AHardwareBuffer on this tablet? (stage C of removing the CPU copy.)
//  1. allocate a normal AHardwareBuffer, look at its native_handle (fds, ints) and what its fd is;
//  2. make a dma-buf from /dev/dma_heap/<heap>, wrap it in a native_handle shaped like step 1's, AHardwareBuffer_createFromHandle;
//  3. if that worked, lock the imported buffer, write a pattern through it, and read it back through the dma-buf fd (same memory?).
// Built with the NDK, run as root on the tablet with One UI up: aarch64-linux-android30-clang -O1 -I<dir with vndk/ and cutils/> -o ahb-import ahb-import.c -landroid -lnativewindow
// Usage: ahb-import [HEAP (default system)] [W H]
#define _GNU_SOURCE
#include <android/hardware_buffer.h>
#include <cutils/native_handle.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

enum { FROM_HANDLE_REGISTER = 2, FROM_HANDLE_CLONE = 3 };
extern const native_handle_t *AHardwareBuffer_getNativeHandle(const AHardwareBuffer *buffer);
extern int AHardwareBuffer_createFromHandle(const AHardwareBuffer_Desc *desc, const native_handle_t *handle, int32_t method, AHardwareBuffer **outBuffer);

static void show_fd(int fd, const char *tag)
{
  char link[64], target[256] = "?"; snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
  ssize_t n = readlink(link, target, sizeof target - 1); if (n > 0) target[n] = 0;
  struct stat st; fstat(fd, &st);
  printf("  %s fd %d -> %s, size %lld\n", tag, fd, target, (long long)st.st_size);
}

int main(int argc, char **argv)
{
  const char *heap = argc > 1 ? argv[1] : "system";
  uint32_t W = argc > 3 ? atoi(argv[2]) : 2112, H = argc > 3 ? atoi(argv[3]) : 1320;

  AHardwareBuffer_Desc d = { .width = W, .height = H, .layers = 1, .format = 5 /* HAL_PIXEL_FORMAT_BGRA_8888 */,
                             .usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE };
  AHardwareBuffer *own = NULL;
  int r = AHardwareBuffer_allocate(&d, &own);
  printf("1. AHardwareBuffer_allocate %ux%u BGRA, CPU+GPU_SAMPLED: %d\n", W, H, r);
  if (r) return 1;
  AHardwareBuffer_Desc got; AHardwareBuffer_describe(own, &got);
  printf("   stride %u (pixels) = %u bytes, format %u, usage 0x%llx\n", got.stride, got.stride * 4, got.format, (unsigned long long)got.usage);
  const native_handle_t *nh = AHardwareBuffer_getNativeHandle(own);
  printf("   native_handle: version %d numFds %d numInts %d\n", nh->version, nh->numFds, nh->numInts);
  for (int i = 0; i < nh->numFds; i++) show_fd(nh->data[i], "own");
  printf("   ints:"); for (int i = 0; i < nh->numInts; i++) printf(" 0x%x", nh->data[nh->numFds + i]); printf("\n");

  int hfd = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC); (void)hfd;
  char path[128]; snprintf(path, sizeof path, "/dev/dma_heap/%s", heap);
  int heapfd = open(path, O_RDONLY | O_CLOEXEC);
  if (heapfd < 0) { printf("2. open %s: %s\n", path, strerror(errno)); return 1; }
  size_t size = (size_t)got.stride * 4 * H;
  struct dma_heap_allocation_data a = { .len = size, .fd_flags = O_RDWR | O_CLOEXEC };
  if (ioctl(heapfd, DMA_HEAP_IOCTL_ALLOC, &a)) { printf("2. DMA_HEAP_IOCTL_ALLOC %zu bytes in %s: %s\n", size, heap, strerror(errno)); return 1; }
  printf("2. dma-buf from %s, %zu bytes\n", heap, size);
  show_fd(a.fd, "mine");

  size_t nbytes = sizeof(native_handle_t) + sizeof(int) * (nh->numFds + nh->numInts);
  native_handle_t *mine = malloc(nbytes); memcpy(mine, nh, nbytes);
  int idx = 0; mine->data[idx] = dup(a.fd);
  printf("   handle copied from the allocated one, its first fd replaced by mine\n");

  AHardwareBuffer *imp = NULL;
  r = AHardwareBuffer_createFromHandle(&got, mine, FROM_HANDLE_CLONE, &imp);
  printf("   createFromHandle(CLONE): %d (%s)\n", r, r == 0 ? "ok" : strerror(-r));
  if (r) { r = AHardwareBuffer_createFromHandle(&got, mine, FROM_HANDLE_REGISTER, &imp); printf("   createFromHandle(REGISTER): %d (%s)\n", r, r == 0 ? "ok" : strerror(-r)); }
  if (r) return 1;

  void *p = NULL;
  r = AHardwareBuffer_lock(imp, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, NULL, &p);
  printf("3. lock imported: %d, ptr %p\n", r, p);
  if (r || !p) return 1;
  memset(p, 0x5a, 4096); ((unsigned char *)p)[4096] = 0xa5;
  AHardwareBuffer_unlock(imp, NULL);

  unsigned char *m = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, a.fd, 0);
  struct dma_buf_sync s = { .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ }; ioctl(a.fd, DMA_BUF_IOCTL_SYNC, &s);
  printf("   through the dma-buf: first byte 0x%02x, byte 4096 0x%02x  -> %s\n", m[0], m[4096], (m[0] == 0x5a && m[4096] == 0xa5) ? "SAME MEMORY" : "NOT the same memory");
  s.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ; ioctl(a.fd, DMA_BUF_IOCTL_SYNC, &s);
  return 0;
}
