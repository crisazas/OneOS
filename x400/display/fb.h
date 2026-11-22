// linux-x400: the frame file the guest writes and the display helper shows.
// Header (little endian) then pixels at FB_PIXELS; seq is written last, after the pixels and a release fence.
#ifndef X400_FB_H
#define X400_FB_H
#include <stdint.h>
#define FB_MAGIC 0x42463458u          /* "X4FB" */
#define FB_PIXELS 4096
struct fb_header { uint32_t magic, width, height, stride, format; uint32_t pad[3]; volatile uint64_t seq; };
#endif
