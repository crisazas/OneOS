/* linux-x400: an OpenGL (desktop, core) quad made entirely in the vertex shader from gl_VertexID: no vertex buffer or attribute is read from memory.
 * If it draws and gl-readback's flat quad (which reads a vertex buffer) does not, the GPU is not reading the vertex buffer Zink binds; if it does
 * not draw either, the rasteriser/colour-write path is what fails. Build in the guest: gcc -O1 -o gl-vertexid gl-vertexid.c -lEGL -lGL -lgbm */
#define GL_GLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <gbm.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define W 256
#define H 256
static unsigned char px[W * H * 4];
static GLuint sh(GLenum t, const char *src)
{
   GLuint s = glCreateShader(t); glShaderSource(s, 1, &src, NULL); glCompileShader(s);
   GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
   if (!ok) { char l[512]; glGetShaderInfoLog(s, sizeof l, NULL, l); fprintf(stderr, "shader: %s\n", l); exit(2); }
   return s;
}
int main(void)
{
   int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
   struct gbm_device *gbm = gbm_create_device(fd);
   if (!gbm) { fprintf(stderr, "gbm failed\n"); return 1; }
   PFNEGLGETPLATFORMDISPLAYEXTPROC gd = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
   EGLDisplay dpy = gd(EGL_PLATFORM_GBM_KHR, gbm, NULL);
   EGLint maj, min; if (!eglInitialize(dpy, &maj, &min)) { fprintf(stderr, "eglInitialize failed\n"); return 1; }
   eglBindAPI(EGL_OPENGL_API);
   const EGLint ca[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_SURFACE_TYPE, 0, EGL_NONE };
   EGLConfig cfg; EGLint n; eglChooseConfig(dpy, ca, &cfg, 1, &n);
   const EGLint cx[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE };
   EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, cx);
   if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) { fprintf(stderr, "context failed 0x%x\n", eglGetError()); return 1; }
   printf("renderer: %s\n", glGetString(GL_RENDERER));
   GLuint tex, fbo, vao;
   glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
   glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
   glGenVertexArrays(1, &vao); glBindVertexArray(vao);
   glViewport(0, 0, W, H);
   GLuint p = glCreateProgram();
   glAttachShader(p, sh(GL_VERTEX_SHADER, "#version 140\nvoid main(){ vec2 q[4] = vec2[4](vec2(-1.0,-1.0), vec2(1.0,-1.0), vec2(-1.0,1.0), vec2(1.0,1.0)); gl_Position = vec4(q[gl_VertexID], 0.0, 1.0); }"));
   glAttachShader(p, sh(GL_FRAGMENT_SHADER, "#version 140\nout vec4 c; void main(){ c = vec4(1.0, 1.0, 0.0, 1.0); }"));
   glLinkProgram(p); glUseProgram(p);
   glClearColor(0.0f, 0.0f, 1.0f, 1.0f); glClear(GL_COLOR_BUFFER_BIT);
   glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
   memset(px, 0xAA, sizeof px);
   glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
   const unsigned char *c = px + (H / 2 * W + W / 2) * 4;
   printf("quad from gl_VertexID: px(128,128)=%u %u %u %u (yellow = 255 255 0 255 draws; blue = 0 0 255 255 does not)\n", c[0], c[1], c[2], c[3]);
   return 0;
}
