// linux-x400: the wire protocol between x400-camd (Android, host side) and x400-camsrc (guest).
// SOCK_STREAM on /data/local/tmp/oneos/rootfs/tmp/x400-cam.sock (the guest sees it as /tmp/x400-cam.sock).
// The client sends one struct cam_req; x400-camd opens that camera and answers with one CAM_FORMAT header, then a CAM_FRAME header
// followed by `size` bytes of payload for every frame, until the client closes the socket (the camera is closed then) or sends
// another request. Frames are tightly packed NV12 (Y plane w*h, then interleaved UV w*h/2): plain copies, no descriptors.
#ifndef X400_CAMPROTO_H
#define X400_CAMPROTO_H
#include <stdint.h>

#define CAM_MAGIC 0x58344300u
#define CAM_SOCK_HOST "/data/local/tmp/oneos/rootfs/tmp/x400-cam.sock"
#define CAM_SOCK_GUEST "/tmp/x400-cam.sock"

enum { CAM_REQ_OPEN = 1, CAM_REQ_CLOSE = 2 };
enum { CAM_FORMAT = 1, CAM_FRAME = 2, CAM_ERROR = 3 };
enum { CAM_PIX_NV12 = 1 };

struct cam_req {
	uint32_t magic, cmd;
	uint32_t id;		// Android camera id: 0 rear, 1 front
	uint32_t width, height;
	uint32_t fps;
};

struct cam_hdr {
	uint32_t magic, type;
	uint32_t width, height, stride, pixfmt;	// stride in bytes of the Y plane as sent (== width)
	uint32_t size;		// payload bytes after this header
	uint32_t rotation;	// degrees the sensor image must be rotated clockwise to be upright (ACAMERA_SENSOR_ORIENTATION)
	uint32_t facing;	// 0 front, 1 back (ACAMERA_LENS_FACING)
	int32_t error;		// CAM_ERROR: Android camera_status_t
	uint64_t seq, ts_ns;
};
#endif
