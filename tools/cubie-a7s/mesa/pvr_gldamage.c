/*
 * pvr_gldamage [width height frames]: what a frame drawn into a small part
 * of a framebuffer object costs on zink and the PowerVR driver, against one
 * that draws all of it, and whether the rest of the object keeps what it
 * had.
 *
 * A frame clears a 256x256 rectangle, at a place that moves from frame to
 * frame, and draws a triangle in it, scissored to it, as Summit's
 * compositor draws its damage. The attachment is loaded and stored: tiles
 * nothing was drawn in are skipped (pvr_arch_cmd_buffer.c, empty tiles), so
 * such a frame should cost about what the rectangle does, not what the
 * whole object does (2.4 ns a texel when every tile was loaded). Afterwards
 * every texel is checked: each frame's rectangle has its colour, or the
 * triangle's, or what a later frame drew over it, and the rest the colour
 * the object was filled with at first.
 *
 * SPDX-License-Identifier: MIT
 */

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <OS.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const int kRect = 256;

static GLuint
shader(GLenum type, const char *source)
{
   GLuint s = glCreateShader(type);
   glShaderSource(s, 1, &source, NULL);
   glCompileShader(s);
   return s;
}

static void
rect_of(int frame, int width, int height, int32_t *r)
{
   int columns = width / kRect, rows = height / kRect;
   int cell = (frame * 7) % (columns * rows);
   r[0] = (cell % columns) * kRect;
   r[1] = (cell / columns) * kRect;
   r[2] = r[0] + kRect - 1;
   r[3] = r[1] + kRect - 1;
}

static uint8_t
shade(int frame)
{
   return (uint8_t)(16 + (frame * 37) % 200);
}

/* one frame into the rectangle: a clear and a triangle, scissored */
static void
draw_frame(int frame, int width, int height, GLuint program)
{
   int32_t r[4];
   rect_of(frame, width, height, r);
   glEnable(GL_SCISSOR_TEST);
   /* in a framebuffer object GL's row y is the image's row y, the image's
      first row being its top as Summit composites (WebKit flips its
      projection) */
   glScissor(r[0], r[1], kRect, kRect);
   glClearColor(shade(frame) / 255.0f, 0.5f, 0.25f, 1);
   glClear(GL_COLOR_BUFFER_BIT);
   glUseProgram(program);
   glDrawArrays(GL_TRIANGLES, 0, 3);
}

int
main(int argc, char **argv)
{
   int width = argc > 2 ? atoi(argv[1]) : 1920;
   int height = argc > 2 ? atoi(argv[2]) : 1080;
   int frames = argc > 3 ? atoi(argv[3]) : 120;

   EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
   eglInitialize(display, NULL, NULL);
   eglBindAPI(EGL_OPENGL_ES_API);
   const EGLint configAttributes[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8, EGL_NONE };
   EGLConfig config;
   EGLint count = 0;
   eglChooseConfig(display, configAttributes, &config, 1, &count);
   const EGLint surfaceAttributes[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16,
      EGL_NONE };
   EGLSurface surface = eglCreatePbufferSurface(display, config,
      surfaceAttributes);
   const EGLint contextAttributes[] = { EGL_CONTEXT_CLIENT_VERSION, 2,
      EGL_NONE };
   EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT,
      contextAttributes);
   if (!eglMakeCurrent(display, surface, surface, context)) {
      printf("no EGL context\n");
      return 1;
   }
   printf("renderer: %s, %dx%d, %d frames\n", glGetString(GL_RENDERER),
      width, height, frames);

   GLuint program = glCreateProgram();
   glAttachShader(program, shader(GL_VERTEX_SHADER,
      "attribute vec2 position;"
      " void main() { gl_Position = vec4(position, 0.0, 1.0); }"));
   glAttachShader(program, shader(GL_FRAGMENT_SHADER,
      "precision mediump float; void main() {"
      " gl_FragColor = vec4(0.2, 0.8, 0.4, 1.0); }"));
   glBindAttribLocation(program, 0, "position");
   glLinkProgram(program);
   GLint linked = 0;
   glGetProgramiv(program, GL_LINK_STATUS, &linked);
   if (!linked) {
      printf("no shader program\n");
      return 1;
   }
   /* a triangle over the lower left of the object: some rectangles get a
      part of it */
   static const GLfloat triangle[] = { -1, -1, 0.2f, -1, -1, 0.6f };
   GLuint vertices;
   glGenBuffers(1, &vertices);
   glBindBuffer(GL_ARRAY_BUFFER, vertices);
   glBufferData(GL_ARRAY_BUFFER, sizeof(triangle), triangle, GL_STATIC_DRAW);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
   glEnableVertexAttribArray(0);

   GLuint fbo, color;
   glGenFramebuffers(1, &fbo);
   glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   glGenRenderbuffers(1, &color);
   glBindRenderbuffer(GL_RENDERBUFFER, color);
   glRenderbufferStorage(GL_RENDERBUFFER, 0x8058 /* GL_RGBA8 */, width,
      height);
   glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
      GL_RENDERBUFFER, color);
   glViewport(0, 0, width, height);

   /* the whole object, then frames into its rectangles; then frames that
      draw all of it, for comparison */
   glDisable(GL_SCISSOR_TEST);
   glClearColor(0.75f, 0.0f, 0.75f, 1);
   glClear(GL_COLOR_BUFFER_BIT);
   glFinish();
   bigtime_t start = system_time();
   for (int frame = 0; frame < frames; frame++) {
      draw_frame(frame, width, height, program);
      /* a frame is presented and waited for */
      glFinish();
   }
   bigtime_t partial = system_time() - start;

   /* check: what the last pass left */
   uint8_t *pixels = malloc((size_t)width * height * 4);
   glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
   int *owner = malloc(sizeof(int) * width * height);
   for (int i = 0; i < width * height; i++)
      owner[i] = -1;
   for (int frame = 0; frame < frames; frame++) {
      int32_t r[4];
      rect_of(frame, width, height, r);
      for (int y = r[1]; y <= r[3]; y++)
         for (int x = r[0]; x <= r[2]; x++)
            owner[y * width + x] = frame;
   }
   long wrong = 0, triangles = 0;
   for (int y = 0; y < height; y++) {
      for (int x = 0; x < width; x++) {
         const uint8_t *p = pixels + ((size_t)y * width + x) * 4;
         int frame = owner[y * width + x];
         bool ok;
         if (frame < 0)
            ok = p[0] == 191 && p[1] == 0 && p[2] == 191;
         else if (p[0] == 51 && p[1] == 204 && p[2] == 102) {
            /* the triangle's colour, in the rectangle it was drawn in */
            ok = true;
            triangles++;
         } else
            ok = abs(p[0] - shade(frame)) <= 1 && abs(p[1] - 128) <= 1;
         if (!ok && wrong++ < 4) {
            printf("  (%d,%d): %u %u %u, frame %d\n", x, y, p[0], p[1], p[2],
               frame);
         }
      }
   }
   /* frames that draw all of the object */
   glDisable(GL_SCISSOR_TEST);
   start = system_time();
   for (int frame = 0; frame < frames; frame++) {
      glClearColor(shade(frame) / 255.0f, 0.5f, 0.25f, 1);
      glClear(GL_COLOR_BUFFER_BIT);
      glDrawArrays(GL_TRIANGLES, 0, 3);
      glFinish();
   }
   bigtime_t whole = system_time() - start;

   printf("a 256x256 part of the object: %.2f ms a frame\n",
      partial / 1000.0 / frames);
   printf("all of the object:            %.2f ms a frame\n",
      whole / 1000.0 / frames);
   printf("texels checked: %ld wrong (%ld of the triangle)\n", wrong,
      triangles);
   printf("%s\n", wrong == 0 ? "PASSED" : "FAILED");
   return wrong == 0 ? 0 : 1;
}
