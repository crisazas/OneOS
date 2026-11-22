/* linux-x400: virgl_test_server runs Venus only (VIRGL_RENDERER_NO_VIRGL), so the GL renderer (vrend), its winsys and the
 * DRM native-context renderer are never initialised. These stand-ins satisfy the linker; each one that could be reached
 * by a client fails instead of pretending to work. */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/uio.h>

struct virgl_context;
struct pipe_resource;
struct vrend_resource;
struct vrend_if_cbs;
struct vrend_transfer_info;
struct vrend_renderer_resource_info;
struct vrend_renderer_resource_create_args;
struct virgl_renderer_export_query;
struct virgl_gl_ctx_param;
union virgl_caps;

static void no_gl(const char *f) { fprintf(stderr, "virgl_test_server (linux-x400): %s called, but this build has no GL\n", f); }

size_t drm_renderer_capset(void *capset) { (void)capset; return 0; }
struct virgl_context *drm_renderer_create(size_t n, const char *name, int fd) { (void)n; (void)name; (void)fd; no_gl(__func__); return NULL; }
void drm_renderer_fini(void) {}
int drm_renderer_init(int fd) { (void)fd; return -ENODEV; }
void drm_renderer_reset(void) {}

int vrend_renderer_init(const struct vrend_if_cbs *cbs, uint32_t flags) { (void)cbs; (void)flags; no_gl(__func__); return -ENODEV; }
void vrend_renderer_fini(void) {}
void vrend_renderer_reset(void) {}
void vrend_renderer_prepare_reset(void) {}
void vrend_renderer_force_ctx_0(void) {}
void vrend_renderer_poll(void) {}
int vrend_renderer_get_poll_fd(void) { return -1; }
const void *vrend_renderer_get_pipe_callbacks(void) { return NULL; }
struct virgl_context *vrend_renderer_context_create(uint32_t h, uint32_t n, const char *name) { (void)h; (void)n; (void)name; no_gl(__func__); return NULL; }
int vrend_renderer_create_ctx0_fence(uint32_t id) { (void)id; return -ENODEV; }
int vrend_renderer_export_ctx0_fence(uint32_t id, int *fd) { (void)id; if (fd) *fd = -1; return -ENODEV; }
int vrend_renderer_export_query(struct pipe_resource *p, struct virgl_renderer_export_query *q) { (void)p; (void)q; return -ENODEV; }
void vrend_renderer_fill_caps(uint32_t set, uint32_t ver, union virgl_caps *caps) { (void)set; (void)ver; (void)caps; }
void vrend_renderer_get_cap_set(uint32_t set, uint32_t *max_ver, uint32_t *max_size) { (void)set; if (max_ver) *max_ver = 0; if (max_size) *max_size = 0; }
void *vrend_renderer_get_cursor_contents(struct pipe_resource *p, uint32_t *w, uint32_t *h) { (void)p; if (w) *w = 0; if (h) *h = 0; return NULL; }
void vrend_renderer_get_rect(struct pipe_resource *p, const struct iovec *iov, unsigned n, uint32_t off, int x, int y, int w, int h)
{ (void)p; (void)iov; (void)n; (void)off; (void)x; (void)y; (void)w; (void)h; }
struct pipe_resource *vrend_renderer_resource_create(const struct vrend_renderer_resource_create_args *a, void *eos) { (void)a; (void)eos; no_gl(__func__); return NULL; }
void vrend_renderer_resource_destroy(struct vrend_resource *r) { (void)r; }
void vrend_renderer_resource_get_info(struct pipe_resource *p, struct vrend_renderer_resource_info *i) { (void)p; (void)i; }
uint32_t vrend_renderer_resource_get_map_info(struct pipe_resource *p) { (void)p; return 0; }
int vrend_renderer_resource_map(struct pipe_resource *p, void **map, uint64_t *size) { (void)p; (void)map; (void)size; return -ENODEV; }
int vrend_renderer_resource_unmap(struct pipe_resource *p) { (void)p; return -ENODEV; }
int vrend_renderer_transfer_pipe(struct pipe_resource *p, const struct vrend_transfer_info *i, int m) { (void)p; (void)i; (void)m; return -ENODEV; }

int vrend_winsys_init(uint32_t flags, int fd) { (void)flags; (void)fd; return -ENODEV; }
int vrend_winsys_init_external(void *d) { (void)d; return -ENODEV; }
void vrend_winsys_cleanup(void) {}
void *vrend_winsys_create_context(struct virgl_gl_ctx_param *p) { (void)p; return NULL; }
void vrend_winsys_destroy_context(void *c) { (void)c; }
int vrend_winsys_make_context_current(void *c) { (void)c; return -ENODEV; }
int vrend_winsys_get_fd_for_texture(uint32_t t, int *fd) { (void)t; if (fd) *fd = -1; return -ENODEV; }
int vrend_winsys_get_fd_for_texture2(uint32_t t, int *fd, int *stride, int *off) { (void)t; (void)stride; (void)off; if (fd) *fd = -1; return -ENODEV; }
int vrend_winsys_get_fourcc_for_texture(uint32_t t, uint32_t fmt, int *fourcc) { (void)t; (void)fmt; (void)fourcc; return -ENODEV; }
