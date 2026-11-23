/* linux-x400: does what Zink draws on Venus come back to the CPU, and does what the CPU uploads reach the GPU?
 * Same path as KWin's virtual backend: GBM device on the DRM render node -> EGL -> an FBO, then glReadPixels.
 *   1. clear to red and read it back                               (GPU -> CPU)
 *   2. upload a CPU-made texture, draw it to the FBO, read it back (CPU -> GPU -> CPU)
 * Build in the guest: gcc -O1 -o gl-readback gl-readback.c -lEGL -lGLESv2 -lgbm */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <gbm.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define W 256
#define H 256

static unsigned char px[W * H * 4];

static void report(const char *what, int x, int y)
{
   const unsigned char *p = px + (y * W + x) * 4;
   unsigned nonzero = 0;
   for (int i = 0; i < W * H * 4; i++)
      nonzero += px[i] != 0;
   printf("%-34s px(%d,%d)=%3u %3u %3u %3u   non-zero bytes: %u of %d\n", what, x, y, p[0], p[1], p[2], p[3], nonzero, W * H * 4);
}

static GLuint shader(GLenum type, const char *src)
{
   GLuint s = glCreateShader(type);
   glShaderSource(s, 1, &src, NULL);
   glCompileShader(s);
   GLint ok = 0;
   glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
   if (!ok) {
      char log[512];
      glGetShaderInfoLog(s, sizeof log, NULL, log);
      fprintf(stderr, "shader: %s\n", log);
      exit(2);
   }
   return s;
}

int main(void)
{
   const char *node = getenv("X400_RENDER_NODE") ? getenv("X400_RENDER_NODE") : "/dev/dri/renderD128";
   int fd = open(node, O_RDWR | O_CLOEXEC);
   if (fd < 0) { perror(node); return 1; }
   struct gbm_device *gbm = gbm_create_device(fd);
   if (!gbm) { fprintf(stderr, "gbm_create_device failed\n"); return 1; }

   PFNEGLGETPLATFORMDISPLAYEXTPROC getDisplay = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
   EGLDisplay dpy = getDisplay(EGL_PLATFORM_GBM_KHR, gbm, NULL);
   EGLint major, minor;
   if (!eglInitialize(dpy, &major, &minor)) { fprintf(stderr, "eglInitialize failed 0x%x\n", eglGetError()); return 1; }
   eglBindAPI(EGL_OPENGL_ES_API);
   const EGLint cfg_attr[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, 0, EGL_NONE };
   EGLConfig cfg; EGLint n;
   eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n);
   const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
   EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
   if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
      fprintf(stderr, "context failed 0x%x\n", eglGetError());
      return 1;
   }
   printf("EGL %d.%d  renderer: %s\n", major, minor, glGetString(GL_RENDERER));

   GLuint tex, fbo;
   glGenTextures(1, &tex);
   glBindTexture(GL_TEXTURE_2D, tex);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   glGenFramebuffers(1, &fbo);
   glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
   if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "FBO incomplete\n"); return 1; }
   glViewport(0, 0, W, H);

   /* 1. GPU -> CPU */
   glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT);
   memset(px, 0xAA, sizeof px);
   glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
   report("1. clear red, glReadPixels", W / 2, H / 2);

   /* 2. CPU -> GPU -> CPU: a texture filled by the CPU, drawn over the FBO's own texture through a second one */
   static unsigned char up[W * H * 4];
   for (int i = 0; i < W * H; i++) { up[i * 4] = 0; up[i * 4 + 1] = 200; up[i * 4 + 2] = 0; up[i * 4 + 3] = 255; }
   GLuint src;
   glGenTextures(1, &src);
   glBindTexture(GL_TEXTURE_2D, src);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, up);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

   /* 2a. the uploaded texture read straight back through an FBO, with no shader: separates "the upload never arrived" from "the draw failed" */
   GLuint fbo2;
   glGenFramebuffers(1, &fbo2);
   glBindFramebuffer(GL_FRAMEBUFFER, fbo2);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, src, 0);
   memset(px, 0xAA, sizeof px);
   glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
   report("2a. CPU texture read back, no shader", W / 2, H / 2);
   glBindFramebuffer(GL_FRAMEBUFFER, fbo);

   /* 2b. the same quad with a constant colour and no texture: separates "no draw call writes anything" from "only sampling fails" */
   GLuint flat = glCreateProgram();
   glAttachShader(flat, shader(GL_VERTEX_SHADER, "attribute vec2 p; void main(){ gl_Position = vec4(p, 0.0, 1.0); }"));
   glAttachShader(flat, shader(GL_FRAGMENT_SHADER, "precision mediump float; void main(){ gl_FragColor = vec4(1.0, 1.0, 0.0, 1.0); }"));
   glBindAttribLocation(flat, 0, "p");
   glLinkProgram(flat);
   glUseProgram(flat);
   static const GLfloat quad0[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
   glEnableVertexAttribArray(0);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad0);
   glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT);
   glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
   memset(px, 0xAA, sizeof px);
   glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
   report("2b. flat yellow quad, no texture", W / 2, H / 2);

   GLuint prog = glCreateProgram();
   glAttachShader(prog, shader(GL_VERTEX_SHADER, "attribute vec2 p; varying vec2 uv; void main(){ uv = p * 0.5 + 0.5; gl_Position = vec4(p, 0.0, 1.0); }"));
   glAttachShader(prog, shader(GL_FRAGMENT_SHADER, "precision mediump float; varying vec2 uv; uniform sampler2D t; void main(){ gl_FragColor = texture2D(t, uv); }"));
   glBindAttribLocation(prog, 0, "p");
   glLinkProgram(prog);
   glUseProgram(prog);
   static const GLfloat quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
   glEnableVertexAttribArray(0);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
   glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT);
   glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
   memset(px, 0xAA, sizeof px);
   glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
   report("2. CPU texture drawn, read back", W / 2, H / 2);


   /* 3. a draw on a texture that was never cleared and never written: if the draw writes anything, even into a cleared-to-zero texture, it shows here
    *    (rules out "the clear wins over the draw" and "the draw lands in some other image") */
   GLuint tex3, fbo3;
   glGenTextures(1, &tex3);
   glBindTexture(GL_TEXTURE_2D, tex3);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
   glGenFramebuffers(1, &fbo3);
   glBindFramebuffer(GL_FRAMEBUFFER, fbo3);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex3, 0);
   glViewport(0, 0, W, H);
   glUseProgram(flat);
   glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
   memset(px, 0xAA, sizeof px);
   glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
   report("3. flat quad on a never-cleared texture", W / 2, H / 2);

   /* 4. the same on the window-system-less default: draw to the FBO, then glFinish before reading (is it only a missing wait?) */
   glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT);
   glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
   glFinish();
   usleep(200000);
   memset(px, 0xAA, sizeof px);
   glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
   report("4. flat quad, glFinish + 200 ms", W / 2, H / 2);


   /* 6. the same quad from a real GL vertex buffer object filled by glBufferData (the data crosses the CPU->GPU boundary as a buffer, not as client
    *    memory): if this draws and the client-array quad does not, the streaming upload of Zink is what fails */
   {
      GLuint vbo;
      glGenBuffers(1, &vbo);
      glBindBuffer(GL_ARRAY_BUFFER, vbo);
      glBufferData(GL_ARRAY_BUFFER, sizeof quad0, quad0, GL_STATIC_DRAW);
      glUseProgram(flat);
      glEnableVertexAttribArray(0);
      glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (const void *)0);
      glBindFramebuffer(GL_FRAMEBUFFER, fbo);
      glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
      glClear(GL_COLOR_BUFFER_BIT);
      glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
      memset(px, 0xAA, sizeof px);
      glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
      report("6. flat quad from a real VBO", W / 2, H / 2);
      glBindBuffer(GL_ARRAY_BUFFER, 0);
   }

   /* 5. occlusion query around the flat quad: does the rasteriser produce any sample at all? passed>0 with an unchanged colour means the colour
    *    write is lost; passed==0 means the quad is clipped, culled or never rasterised */
   {
      void (*genq)(GLsizei, GLuint *) = (void *)eglGetProcAddress("glGenQueriesEXT");
      void (*beginq)(GLenum, GLuint) = (void *)eglGetProcAddress("glBeginQueryEXT");
      void (*endq)(GLenum) = (void *)eglGetProcAddress("glEndQueryEXT");
      void (*getq)(GLuint, GLenum, GLuint *) = (void *)eglGetProcAddress("glGetQueryObjectuivEXT");
#define GL_ANY_SAMPLES_PASSED_EXT 0x8C2F
#define GL_QUERY_RESULT_EXT 0x8866
#define GL_QUERY_RESULT_AVAILABLE_EXT 0x8867
      if (genq && beginq && endq && getq) {
         GLuint q; genq(1, &q);
         glBindFramebuffer(GL_FRAMEBUFFER, fbo);
         glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
         glClear(GL_COLOR_BUFFER_BIT);
         beginq(GL_ANY_SAMPLES_PASSED_EXT, q);
         glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
         endq(GL_ANY_SAMPLES_PASSED_EXT);
         GLuint passed = 99, avail = 0;
         for (int i = 0; i < 200 && !avail; i++) { getq(q, GL_QUERY_RESULT_AVAILABLE_EXT, &avail); if (!avail) usleep(10000); }
         getq(q, GL_QUERY_RESULT_EXT, &passed);
         printf("5. occlusion query around the flat quad: available=%u any_samples_passed=%u\n", avail, passed);
      } else {
         printf("5. occlusion query: entry points missing (EXT_occlusion_query_boolean not exposed)\n");
      }
   }

   printf("expected: 1 -> 255 0 0 255 ; 2a and 2 -> 0 200 0 255 ; 2b, 3, 4 -> 255 255 0 255\n");
   return 0;
}
