// linux-x400: x400-camd, runs on the Android side as root. Opens a camera through the NDK camera2 API (needs fakesvc: cameraserver waits for
// system_server's services, see probe/fakesvc.c) and either
//   x400-camd test ID WIDTH HEIGHT OUT.ppm [N]   captures N frames and writes the last one as a PPM plus luminance statistics, or
//   x400-camd serve [SOCKET]                      listens on SOCKET (camproto.h); the camera is open only while a client is connected.
#define _GNU_SOURCE
#include "camproto.h"
#include <android/log.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraMetadata.h>
#include <camera/NdkCaptureRequest.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void say(const char *fmt, ...)
{
	char b[512];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(b, sizeof b, fmt, ap);
	va_end(ap);
	fprintf(stderr, "x400-camd: %s\n", b);
	__android_log_write(ANDROID_LOG_INFO, "x400-camd", b);
}

static uint64_t now_ns(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}

typedef void (*frame_fn)(void *user, const uint8_t *nv12, uint32_t w, uint32_t h, uint64_t seq, uint64_t ts);

struct cam {
	ACameraManager *mgr;
	ACameraDevice *dev;
	AImageReader *reader;
	ANativeWindow *win;
	ACaptureSessionOutputContainer *outs;
	ACaptureSessionOutput *out;
	ACameraOutputTarget *target;
	ACaptureRequest *req;
	ACameraCaptureSession *sess;
	uint32_t w, h, rotation, facing;
	uint64_t seq;
	uint8_t *buf;
	frame_fn cb;
	void *user;
	volatile int dead;
	int notify_fd;			// serve mode: client socket, shut down when the camera dies so the client reconnects
};

static void cam_died(struct cam *c)
{
	c->dead = 1;
	if (c->notify_fd >= 0) shutdown(c->notify_fd, SHUT_RDWR);
}

static void on_disc(void *c, ACameraDevice *d) { (void)d; say("camera disconnected"); cam_died(c); }
static void on_err(void *c, ACameraDevice *d, int e) { (void)d; say("camera error %d", e); cam_died(c); }
static void s_cb(void *c, ACameraCaptureSession *s) { (void)c; (void)s; }

// copy the three YUV_420_888 planes into a tightly packed NV12 buffer
static void pack_nv12(struct cam *c, AImage *img)
{
	int32_t w = c->w, h = c->h;
	uint8_t *y, *u, *v;
	int len, ys, us, vs, up, vp;

	AImage_getPlaneData(img, 0, &y, &len);
	AImage_getPlaneRowStride(img, 0, &ys);
	AImage_getPlaneData(img, 1, &u, &len);
	AImage_getPlaneRowStride(img, 1, &us);
	AImage_getPlanePixelStride(img, 1, &up);
	AImage_getPlaneData(img, 2, &v, &len);
	AImage_getPlaneRowStride(img, 2, &vs);
	AImage_getPlanePixelStride(img, 2, &vp);
	for (int r = 0; r < h; r++)
		memcpy(c->buf + (size_t)r * w, y + (size_t)r * ys, w);
	uint8_t *uv = c->buf + (size_t)w * h;
	for (int r = 0; r < h / 2; r++)
		for (int x = 0; x < w / 2; x++) {
			uv[(size_t)r * w + 2 * x] = u[(size_t)r * us + x * up];
			uv[(size_t)r * w + 2 * x + 1] = v[(size_t)r * vs + x * vp];
		}
}

static void on_image(void *user, AImageReader *rd)
{
	struct cam *c = user;
	AImage *img = NULL;

	if (AImageReader_acquireLatestImage(rd, &img) != AMEDIA_OK || !img)
		return;
	pack_nv12(c, img);
	AImage_delete(img);
	c->cb(c->user, c->buf, c->w, c->h, ++c->seq, now_ns());
}

static void cam_close(struct cam *c)
{
	if (c->sess) { ACameraCaptureSession_stopRepeating(c->sess); ACameraCaptureSession_close(c->sess); }
	if (c->req) ACaptureRequest_free(c->req);
	if (c->target) ACameraOutputTarget_free(c->target);
	if (c->outs && c->out) ACaptureSessionOutputContainer_remove(c->outs, c->out);
	if (c->out) ACaptureSessionOutput_free(c->out);
	if (c->outs) ACaptureSessionOutputContainer_free(c->outs);
	if (c->dev) ACameraDevice_close(c->dev);
	if (c->reader) AImageReader_delete(c->reader);
	if (c->mgr) ACameraManager_delete(c->mgr);
	free(c->buf);
	free(c);
}

static struct cam *cam_open(const char *id, uint32_t w, uint32_t h, uint32_t fps, frame_fn cb, void *user, camera_status_t *err)
{
	struct cam *c = calloc(1, sizeof *c);
	camera_status_t st;
	ACameraMetadata *md = NULL;
	ACameraMetadata_const_entry e;

	c->w = w; c->h = h; c->cb = cb; c->user = user; c->notify_fd = -1;
	c->buf = malloc((size_t)w * h * 3 / 2);
	c->mgr = ACameraManager_create();
	if (ACameraManager_getCameraCharacteristics(c->mgr, id, &md) == ACAMERA_OK) {
		if (ACameraMetadata_getConstEntry(md, ACAMERA_SENSOR_ORIENTATION, &e) == ACAMERA_OK) c->rotation = e.data.i32[0];
		if (ACameraMetadata_getConstEntry(md, ACAMERA_LENS_FACING, &e) == ACAMERA_OK) c->facing = e.data.u8[0] == ACAMERA_LENS_FACING_BACK;
		ACameraMetadata_free(md);
	}
	ACameraDevice_StateCallbacks dcb = { .context = c, .onDisconnected = on_disc, .onError = on_err };
	st = ACameraManager_openCamera(c->mgr, id, &dcb, &c->dev);
	if (st != ACAMERA_OK) { say("openCamera %s: %d", id, st); goto fail; }
	if (AImageReader_new(w, h, AIMAGE_FORMAT_YUV_420_888, 4, &c->reader) != AMEDIA_OK) { st = ACAMERA_ERROR_UNKNOWN; goto fail; }
	AImageReader_ImageListener il = { c, on_image };
	AImageReader_setImageListener(c->reader, &il);
	AImageReader_getWindow(c->reader, &c->win);
	ACaptureSessionOutputContainer_create(&c->outs);
	ACaptureSessionOutput_create(c->win, &c->out);
	ACaptureSessionOutputContainer_add(c->outs, c->out);
	if ((st = ACameraDevice_createCaptureRequest(c->dev, TEMPLATE_PREVIEW, &c->req)) != ACAMERA_OK) { say("createCaptureRequest: %d", st); goto fail; }
	ACameraOutputTarget_create(c->win, &c->target);
	ACaptureRequest_addTarget(c->req, c->target);
	if (fps && !getenv("X400_NOFPS")) {
		int32_t range[2] = { (int32_t)fps, (int32_t)fps };
		ACaptureRequest_setEntry_i32(c->req, ACAMERA_CONTROL_AE_TARGET_FPS_RANGE, 2, range);
	}
	if (!getenv("X400_NOAF")) {
		uint8_t afm = ACAMERA_CONTROL_AF_MODE_CONTINUOUS_PICTURE;
		ACaptureRequest_setEntry_u8(c->req, ACAMERA_CONTROL_AF_MODE, 1, &afm);
	}
	ACameraCaptureSession_stateCallbacks scb = { .context = c, .onClosed = s_cb, .onReady = s_cb, .onActive = s_cb };
	if ((st = ACameraDevice_createCaptureSession(c->dev, c->outs, &scb, &c->sess)) != ACAMERA_OK) { say("createCaptureSession: %d", st); goto fail; }
	if ((st = ACameraCaptureSession_setRepeatingRequest(c->sess, NULL, 1, &c->req, NULL)) != ACAMERA_OK) { say("setRepeatingRequest: %d", st); goto fail; }
	*err = ACAMERA_OK;
	return c;
fail:
	*err = st;
	cam_close(c);
	return NULL;
}

// ---- test mode ----
struct tctx { uint8_t *last; int n; pthread_mutex_t m; pthread_cond_t cv; uint64_t t0, t1; };

static void t_frame(void *u, const uint8_t *nv12, uint32_t w, uint32_t h, uint64_t seq, uint64_t ts)
{
	struct tctx *t = u;

	pthread_mutex_lock(&t->m);
	if (seq == 1) t->t0 = ts;
	t->t1 = ts;
	memcpy(t->last, nv12, (size_t)w * h * 3 / 2);
	t->n = seq;
	pthread_cond_broadcast(&t->cv);
	pthread_mutex_unlock(&t->m);
}

static uint8_t clamp(double v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }

static int test_mode(const char *id, uint32_t w, uint32_t h, const char *out, int n)
{
	struct tctx t = { .m = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };
	camera_status_t st;
	struct cam *c;

	t.last = malloc((size_t)w * h * 3 / 2);
	c = cam_open(id, w, h, 30, t_frame, &t, &st);
	if (!c) { printf("open failed: %d\n", st); return 1; }
	struct timespec dl;
	clock_gettime(CLOCK_REALTIME, &dl);
	dl.tv_sec += 15;
	pthread_mutex_lock(&t.m);
	while (t.n < n && pthread_cond_timedwait(&t.cv, &t.m, &dl) == 0)
		;
	pthread_mutex_unlock(&t.m);
	printf("camera %s %ux%u: %d frames, rotation %u, facing %s", id, w, h, t.n, c->rotation, c->facing ? "back" : "front");
	if (t.n > 1) printf(", %.1f fps", (t.n - 1) * 1e9 / (double)(t.t1 - t.t0));
	printf("\n");
	if (t.n < n) { printf("FAIL: only %d of %d frames\n", t.n, n); cam_close(c); return 2; }
	double sum = 0, sq = 0, grad = 0;
	int hist[16] = { 0 };
	for (size_t i = 0; i < (size_t)w * h; i++) { double v = t.last[i]; sum += v; sq += v * v; hist[t.last[i] >> 4]++; }
	double px = (double)w * h, mean = sum / px, sd = __builtin_sqrt(sq / px - mean * mean);
	// noise measure: mean absolute difference between horizontal neighbours (white noise ~85, a real scene is smooth)
	for (uint32_t r = 0; r < h; r++)
		for (uint32_t x = 1; x < w; x++) { int d = (int)t.last[(size_t)r * w + x] - t.last[(size_t)r * w + x - 1]; grad += d < 0 ? -d : d; }
	grad /= px;
	printf("luma mean %.1f stddev %.1f neighbour-diff %.2f\nhist(16 bins):", mean, sd, grad);
	for (int i = 0; i < 16; i++) printf(" %d", hist[i]);
	printf("\n");
	FILE *f = fopen(out, "wb");
	if (f) {
		fprintf(f, "P6\n%u %u\n255\n", w, h);
		const uint8_t *uv = t.last + (size_t)w * h;
		for (uint32_t r = 0; r < h; r++)
			for (uint32_t x = 0; x < w; x++) {
				int Y = t.last[(size_t)r * w + x], U = uv[(size_t)(r / 2) * w + (x & ~1u)] - 128, V = uv[(size_t)(r / 2) * w + (x & ~1u) + 1] - 128;
				uint8_t p[3] = { clamp(Y + 1.402 * V), clamp(Y - 0.344 * U - 0.714 * V), clamp(Y + 1.772 * U) };
				fwrite(p, 1, 3, f);
			}
		fclose(f);
		printf("wrote %s\n", out);
	}
	cam_close(c);
	return 0;
}

// ---- serve mode ----
struct sctx { int fd; volatile int fail; uint32_t rot, facing; };

static int write_all(int fd, const void *b, size_t n)
{
	const char *p = b;

	while (n) {
		ssize_t r = send(fd, p, n, MSG_NOSIGNAL);
		if (r < 0 && errno == EINTR) continue;
		if (r <= 0) return -1;
		p += r; n -= r;
	}
	return 0;
}

static void s_frame(void *u, const uint8_t *nv12, uint32_t w, uint32_t h, uint64_t seq, uint64_t ts)
{
	struct sctx *s = u;
	struct cam_hdr hd = { CAM_MAGIC, CAM_FRAME, w, h, w, CAM_PIX_NV12, w * h * 3 / 2, s->rot, s->facing, 0, seq, ts };

	if (s->fail) return;
	if (write_all(s->fd, &hd, sizeof hd) || write_all(s->fd, nv12, hd.size)) s->fail = 1;
}

static void serve_client(int fd)
{
	struct cam_req rq;
	struct cam *c = NULL;
	struct sctx s = { .fd = fd };

	for (;;) {
		ssize_t r = recv(fd, &rq, sizeof rq, MSG_WAITALL);
		if (r != (ssize_t)sizeof rq || rq.magic != CAM_MAGIC) break;
		if (c) { cam_close(c); c = NULL; }
		if (rq.cmd != CAM_REQ_OPEN) continue;
		char id[16];
		camera_status_t st;
		snprintf(id, sizeof id, "%u", rq.id);
		say("client opens camera %s %ux%u@%u", id, rq.width, rq.height, rq.fps);
		s.fail = 0;
		c = cam_open(id, rq.width, rq.height, rq.fps, s_frame, &s, &st);
		struct cam_hdr hd = { .magic = CAM_MAGIC, .width = rq.width, .height = rq.height, .stride = rq.width, .pixfmt = CAM_PIX_NV12 };
		if (!c) {
			hd.type = CAM_ERROR; hd.error = st;
			write_all(fd, &hd, sizeof hd);
			continue;
		}
		c->notify_fd = fd;
		s.rot = hd.rotation = c->rotation; s.facing = hd.facing = c->facing;
		hd.type = CAM_FORMAT;
		write_all(fd, &hd, sizeof hd);
	}
	if (c) cam_close(c);
	say("client gone, camera closed");
}

static void *client_thread(void *arg)
{
	serve_client((int)(intptr_t)arg);
	close((int)(intptr_t)arg);
	return NULL;
}

// One thread per client. (Not a process: libbinder aborts in a forked child, and the binder thread pool is already running.)
// Only one client can have a given camera open at a time: a second open of the same id is refused by cameraserver and reported as CAM_ERROR.
static int serve_mode(const char *path)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	int ls = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

	signal(SIGPIPE, SIG_IGN);
	snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);
	unlink(path);
	if (bind(ls, (struct sockaddr *)&sa, sizeof sa) || listen(ls, 4)) { perror("x400-camd: bind"); return 1; }
	chmod(path, 0666);
	say("listening on %s", path);
	for (;;) {
		int fd = accept4(ls, NULL, NULL, SOCK_CLOEXEC);
		pthread_t t;

		if (fd < 0) { if (errno == EINTR) continue; perror("accept"); sleep(1); continue; }
		if (pthread_create(&t, NULL, client_thread, (void *)(intptr_t)fd)) { close(fd); continue; }
		pthread_detach(t);
	}
}

// The AImageReader's BufferQueue producer is a binder object in this process: cameraserver's dequeueBuffer calls come in here, so
// the binder thread pool must run (without it the first request hangs forever). The NDK stub does not export these two.
static void start_binder_threads(void)
{
	void *h = dlopen("libbinder_ndk.so", RTLD_NOW);
	int (*setmax)(uint32_t) = h ? dlsym(h, "ABinderProcess_setThreadPoolMaxThreadCount") : NULL;
	void (*start)(void) = h ? dlsym(h, "ABinderProcess_startThreadPool") : NULL;

	if (!setmax || !start) { say("no ABinderProcess_* in libbinder_ndk.so"); exit(1); }
	setmax(4);
	start();
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	start_binder_threads();
	if (argc >= 6 && !strcmp(argv[1], "test"))
		return test_mode(argv[2], atoi(argv[3]), atoi(argv[4]), argv[5], argc > 6 ? atoi(argv[6]) : 5);
	if (argc >= 2 && !strcmp(argv[1], "serve"))
		return serve_mode(argc > 2 ? argv[2] : CAM_SOCK_HOST);
	fprintf(stderr, "usage: x400-camd test ID W H OUT.ppm [N] | serve [SOCKET]\n");
	return 2;
}
