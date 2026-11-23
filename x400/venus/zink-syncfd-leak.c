/* linux-x400: LD_PRELOAD shim for Mesa 25.0.7's Zink (kernel 5.15 has no DMA_BUF_IOCTL_EXPORT_SYNC_FILE: the ioctl fails with ENOTTY).
 * zink_screen_export_dmabuf_semaphore() owns `fd` (vkGetMemoryFdKHR / dup) and closes it only when the ioctl succeeds, so in KWin every
 * barrier on a client's dma-buf leaked one descriptor (~1 per frame; fixed upstream in 25.2 by closing right after the ioctl).
 * drmIoctl is libdrm's own (ioctl, retried on EINTR/EAGAIN); here it also closes fd when libgallium's EXPORT_SYNC_FILE fails, and
 * leaves KWin's own calls alone (there fd belongs to a GraphicsBuffer).
 * Build: aarch64-linux-gnu-gcc -O2 -fno-stack-protector -shared -fPIC -o libx400_syncfd.so zink-syncfd-leak.c -ldl */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define X400_EXPORT_SYNC_FILE 0xc0086202UL

__attribute__((visibility("default"))) int drmIoctl(int fd, unsigned long request, void *arg)
{
	int ret, e;
	do {
		ret = ioctl(fd, request, arg);
	} while (ret == -1 && (errno == EINTR || errno == EAGAIN));
	if (ret && request == X400_EXPORT_SYNC_FILE) {
		Dl_info i;
		e = errno;
		if (dladdr(__builtin_return_address(0), &i) && i.dli_fname && strstr(i.dli_fname, "libgallium"))
			close(fd);
		errno = e;
	}
	return ret;
}
