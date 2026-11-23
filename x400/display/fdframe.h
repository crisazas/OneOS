// linux-x400: the messages between capture (guest) and the display helper (host) when frames go by descriptor instead of through the
// frame file. SOCK_SEQPACKET on FILE.sock; capture listens, the helper connects. A FRAME message carries, the first time a buffer id
// is used, the buffer's descriptor (SCM_RIGHTS): a memfd, or with FDF_DMABUF a linear BGRA dma-buf that KWin rendered the frame into.
// The buffer is the helper's until it answers DONE with the same id.
#ifndef X400_FDFRAME_H
#define X400_FDFRAME_H
#include <stdint.h>
enum { FDM_FRAME = 1, FDM_DROP = 2, FDM_DONE = 3 };
enum { FDF_DMABUF = 1 };
struct fd_msg { uint32_t type, id, has_fd, width, height, stride, offset, mapoffset, maxsize, flags; uint64_t seq; };
struct fd_reply { uint32_t type, id; };
#endif
