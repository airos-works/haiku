/*
 * pvr_glupload [frames]: what a texture upload costs after draws on zink
 * and the PowerVR driver, as Summit's compositor uploads tiles while it
 * composites (summit#119: "a texture upload after a composition's first
 * draws waits for the GPU to finish all of them").
 *
 * Each frame draws 5 quads sampling texture A into a 1920x1080
 * framebuffer object, then uploads 256x256 texels with glTexSubImage2D:
 *   into A          the texture the pending draws read
 *   into B          a texture they do not read
 *   with no draws   the same upload into B before any draw of the frame
 * and then draws once more with the uploaded texture. The upload's own
 * time (glTexSubImage2D returning) and the frame's (to glFinish) are
 * timed, for RGBA and for BGRA (GL_EXT_texture_format_BGRA8888, what
 * WebKit uploads). An upload that waits for the draws before it shows up
 * as an upload as long as the frame.
 *
 * SPDX-License-Identifier: MIT
 */

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <OS.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const int kWidth = 1920, kHeight = 1080, kTexture = 512, kUpload = 256;

static GLuint
shader(GLenum type, const char *source)
{
   GLuint s = glCreateShader(type);
   glShaderSource(s, 1, &source, NULL);
   glCompileShader(s);
   return s;
}

static GLuint
texture(GLenum format, const uint8_t *pixels)
{
   GLuint t;
   glGenTextures(1, &t);
   glBindTexture(GL_TEXTURE_2D, t);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
   glTexImage2D(GL_TEXTURE_2D, 0, format, kTexture, kTexture, 0, format,
      GL_UNSIGNED_BYTE, pixels);
   return t;
}

static void
draws(GLuint tex, int count)
{
   glBindTexture(GL_TEXTURE_2D, tex);
   for (int i = 0; i < count; i++)
      glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

int
main(int argc, char **argv)
{
   int frames = argc > 1 ? atoi(argv[1]) : 10;
   setvbuf(stdout, NULL, _IONBF, 0);

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
   printf("renderer: %s, %d frames\n", glGetString(GL_RENDERER), frames);

   GLuint program = glCreateProgram();
   glAttachShader(program, shader(GL_VERTEX_SHADER,
      "attribute vec2 position; varying vec2 uv;"
      " void main() { uv = position * 0.5 + 0.5;"
      " gl_Position = vec4(position, 0.0, 1.0); }"));
   glAttachShader(program, shader(GL_FRAGMENT_SHADER,
      "precision mediump float; varying vec2 uv; uniform sampler2D t;"
      " void main() { gl_FragColor = texture2D(t, uv); }"));
   glBindAttribLocation(program, 0, "position");
   glLinkProgram(program);
   glUseProgram(program);
   static const GLfloat quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
   GLuint vertices;
   glGenBuffers(1, &vertices);
   glBindBuffer(GL_ARRAY_BUFFER, vertices);
   glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
   glEnableVertexAttribArray(0);
   glEnable(GL_BLEND);
   glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

   GLuint fbo, color;
   glGenFramebuffers(1, &fbo);
   glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   glGenRenderbuffers(1, &color);
   glBindRenderbuffer(GL_RENDERBUFFER, color);
   glRenderbufferStorage(GL_RENDERBUFFER, 0x8058 /* GL_RGBA8 */, kWidth,
      kHeight);
   glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
      GL_RENDERBUFFER, color);
   glViewport(0, 0, kWidth, kHeight);

   uint8_t *pixels = malloc(kTexture * kTexture * 4);
   for (int i = 0; i < kTexture * kTexture * 4; i++)
      pixels[i] = (uint8_t)(i * 7);

   const GLenum formats[] = { GL_RGBA, GL_BGRA_EXT };
   const char *formatNames[] = { "RGBA", "BGRA" };
   const char *caseNames[] = { "into A (the draws read it)",
      "into B (not read)", "into B, no draws before" };
   int failures = 0;
   for (int f = 0; f < 2; f++) {
      GLuint a = texture(formats[f], pixels);
      GLuint b = texture(formats[f], pixels);
      if (glGetError() != GL_NO_ERROR) {
         printf("%s: textures not supported\n", formatNames[f]);
         continue;
      }
      for (int c = 0; c < 3; c++) {
         bigtime_t uploadTotal = 0, frameTotal = 0, uploadWorst = 0;
         for (int i = 0; i < frames; i++) {
            bigtime_t start = system_time();
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            if (c != 2)
               draws(a, 5);
            GLuint target = c == 0 ? a : b;
            glBindTexture(GL_TEXTURE_2D, target);
            bigtime_t before = system_time();
            glTexSubImage2D(GL_TEXTURE_2D, 0, (i * 64) % kUpload,
               (i * 32) % kUpload, kUpload, kUpload, formats[f],
               GL_UNSIGNED_BYTE, pixels);
            bigtime_t upload = system_time() - before;
            draws(target, 1);
            if (c == 2)
               draws(a, 5);
            glFinish();
            frameTotal += system_time() - start;
            uploadTotal += upload;
            if (upload > uploadWorst)
               uploadWorst = upload;
         }
         printf("%s %-28s upload %6.2f ms (worst %6.2f), frame %6.2f ms\n",
            formatNames[f], caseNames[c], uploadTotal / 1000.0 / frames,
            uploadWorst / 1000.0, frameTotal / 1000.0 / frames);
      }
      if (glGetError() != GL_NO_ERROR)
         failures++;
   }
   printf("%s\n", failures == 0 ? "PASSED" : "FAILED");
   return failures == 0 ? 0 : 1;
}
