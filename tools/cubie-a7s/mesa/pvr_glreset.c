/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

// OpenGL across a GPU reset: each frame clears colour and depth, draws a
// triangle, ends the batch with glFinish() (as eglSwapBuffers() flushes the
// frame before presenting it), and then reads one depth and stencil value
// back. Packed depth/stencil is read by mapping the buffer directly (Mesa
// has no blit path for it), so, like the BGLView present, it waits for the
// batch that the flush ended. After a lost device that batch never
// ran, and zink waited for it forever: GLTeapot's render thread hung there
// and its quit with it. Run this across a reset (pvr_vkhang); it must keep
// going and then end, not hang. Under build.sh shim the reset is injected
// (PVR_TRACE_FAIL_SUBMIT).
//   pvr_glreset [--frames N] [--seconds S]
// (defaults: 300 frames, at most 60 s; a line per second)
// A desktop OpenGL context (EGL_OPENGL_API): OpenGL ES 2.0 cannot read
// depth. PASS means the run ended; the values read are not checked.


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>


typedef void (*begin_function)(GLenum mode);
typedef void (*vertex_function)(GLfloat x, GLfloat y, GLfloat z);
typedef void (*end_function)(void);


static double
now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}


int
main(int argc, char** argv)
{
	// line by line: whatever was printed survives a hang or a crash
	setvbuf(stdout, NULL, _IOLBF, 0);

	unsigned frames = 300;
	double seconds = 60;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
			frames = strtoul(argv[++i], NULL, 0);
		else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc)
			seconds = atof(argv[++i]);
		else {
			printf("usage: %s [--frames N] [--seconds S]\n", argv[0]);
			return 2;
		}
	}

	EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (display == EGL_NO_DISPLAY || !eglInitialize(display, NULL, NULL)) {
		printf("FAIL: eglInitialize\n");
		return 1;
	}
	const EGLint configAttributes[] = {
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
		EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
		EGL_NONE
	};
	EGLConfig config;
	EGLint count = 0;
	if (!eglChooseConfig(display, configAttributes, &config, 1, &count)
		|| count != 1) {
		printf("FAIL: eglChooseConfig (pbuffer, OpenGL, depth/stencil)\n");
		return 1;
	}
	const EGLint surfaceAttributes[] = {
		EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE
	};
	EGLSurface surface = eglCreatePbufferSurface(display, config,
		surfaceAttributes);
	if (!eglBindAPI(EGL_OPENGL_API)) {
		printf("FAIL: eglBindAPI(EGL_OPENGL_API)\n");
		return 1;
	}
	EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT,
		NULL);
	if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT
		|| !eglMakeCurrent(display, surface, surface, context)) {
		printf("FAIL: pbuffer, OpenGL context, eglMakeCurrent\n");
		return 1;
	}
	printf("GL_RENDERER: %s\nGL_VERSION: %s\n",
		(const char*)glGetString(GL_RENDERER),
		(const char*)glGetString(GL_VERSION));

	// immediate mode, as GLTeapot draws: desktop OpenGL only, so through
	// eglGetProcAddress()
	begin_function glBegin = (begin_function)eglGetProcAddress("glBegin");
	vertex_function glVertex3f
		= (vertex_function)eglGetProcAddress("glVertex3f");
	end_function glEnd = (end_function)eglGetProcAddress("glEnd");
	if (glBegin == NULL || glVertex3f == NULL || glEnd == NULL) {
		printf("FAIL: no glBegin/glVertex3f/glEnd\n");
		return 1;
	}

	glViewport(0, 0, 64, 64);
	glEnable(GL_DEPTH_TEST);
	double start = now_ms(), lastLine = start;
	unsigned frame = 0;
	for (; frame < frames; frame++) {
		double now = now_ms();
		if (now - start >= seconds * 1000) {
			printf("time is up\n");
			break;
		}
		glClearColor((frame % 256) / 255.0f, 0, 0, 1);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		// the depth buffer is written by this batch, which the flush ends
		glBegin(GL_TRIANGLES);
		glVertex3f(-1, -1, 0);
		glVertex3f(3, -1, 0);
		glVertex3f(-1, 3, 0);
		glEnd();
		glFlush();
		// GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8 (OpenGL 3.0,
		// EXT_packed_depth_stencil)
		GLuint depthStencil = 0;
		glReadPixels(32, 32, 1, 1, 0x84f9, 0x84fa, &depthStencil);
		if (now - lastLine >= 1000 || frame == 0) {
			printf("frame %u: depth/stencil 0x%08x, GL error 0x%x\n", frame,
				depthStencil, glGetError());
			lastLine = now;
		}
	}
	printf("%u frames in %.1f s\n", frame, (now_ms() - start) / 1000.0);

	eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroyContext(display, context);
	eglDestroySurface(display, surface);
	eglTerminate(display);
	printf("PASS\n");
	return 0;
}
