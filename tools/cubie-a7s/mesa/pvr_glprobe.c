/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

// Offscreen OpenGL ES 2 through the system EGL (libglvnd, then Mesa's
// libEGL_mesa: zink on the PowerVR Vulkan driver, or softpipe), with every
// pixel read back and checked byte for byte:
//   1. a clear to (0.2, 0.4, 0.6, 1.0) = (51, 102, 153, 255);
//   2. one triangle over it, a constant colour (0.8, 0.6, 0.2, 0.4) =
//      (204, 153, 51, 102), corners at window points (0, 0), (64.25, 0) and
//      (0, 64.25): pixel (x, y), y counted from the bottom as glReadPixels
//      returns rows, is covered if and only if x + y <= 63.
// It prints the EGL and GL strings first, so the renderer is known before
// anything is drawn ("zink Vulkan 1.x(PowerVR ...)" on the GPU).
//   pvr_glprobe [--expect TEXT] [--ppm FILE] [--repeat N]
// --expect fails the run unless GL_RENDERER contains TEXT (e.g. "zink" or
// "softpipe"); --ppm writes the triangle image (RGB, top row first);
// --repeat does all of it N times in one process, eglTerminate() included.
// Zink unloads the Vulkan driver with its screen and loads it again for the
// next, which is what made Summit's WebProcess crash on air/OS: a library
// with thread-local storage unloaded, then zink's new cache thread lost its
// first TLS write (see tls_generation_check.c).


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>


#define SIZE			64
#define FILL_BYTE		0xee

static const uint8_t kClearBytes[4] = { 51, 102, 153, 255 };
static const uint8_t kTriangleBytes[4] = { 204, 153, 51, 102 };


static double
now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}


static int
check(int ok, const char* what)
{
	printf("%s: %s\n", ok ? "ok" : "FAIL", what);
	return ok;
}


static GLuint
compile(GLenum type, const char* source)
{
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);
	GLint status = 0;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
	if (status == 0) {
		char log[1024];
		glGetShaderInfoLog(shader, sizeof(log), NULL, log);
		printf("shader compile: %s\n", log);
	}
	return shader;
}


// Compares the SIZE x SIZE pixels with the reference ("triangle": the
// triangle drawn) and prints the first mismatches and a coarse map.
static unsigned
check_pixels(const uint8_t* pixels, int triangle, int alphaBits)
{
	uint8_t clear[4], color[4];
	memcpy(clear, kClearBytes, 4);
	memcpy(color, kTriangleBytes, 4);
	if (alphaBits == 0)
		clear[3] = color[3] = 255;

	unsigned wrong = 0, covered = 0, cleared = 0, untouched = 0;
	for (unsigned y = 0; y < SIZE; y++) {
		for (unsigned x = 0; x < SIZE; x++) {
			const uint8_t* got = pixels + (y * SIZE + x) * 4;
			const uint8_t* want = triangle && x + y <= SIZE - 1 ? color : clear;
			if (memcmp(got, color, 4) == 0)
				covered++;
			else if (memcmp(got, clear, 4) == 0)
				cleared++;
			else if (got[0] == FILL_BYTE && got[1] == FILL_BYTE)
				untouched++;
			if (memcmp(got, want, 4) == 0)
				continue;
			if (wrong < 16) {
				printf("  pixel (%2u, %2u): %3u %3u %3u %3u, expected "
					"%3u %3u %3u %3u\n", x, y, got[0], got[1], got[2], got[3],
					want[0], want[1], want[2], want[3]);
			}
			wrong++;
		}
	}
	printf("  %u wrong; %u triangle, %u clear, %u never written, %u other\n",
		wrong, covered, cleared, untouched,
		SIZE * SIZE - covered - cleared - untouched);
	if (wrong != 0) {
		printf("  every 4th row and column, top first (T triangle, . clear, "
			"- never written, ? other):\n");
		for (int y = SIZE - 1; y >= 0; y -= 4) {
			printf("  ");
			for (unsigned x = 0; x < SIZE; x += 4) {
				const uint8_t* got = pixels + (y * SIZE + x) * 4;
				putchar(memcmp(got, color, 4) == 0 ? 'T'
					: memcmp(got, clear, 4) == 0 ? '.'
					: got[0] == FILL_BYTE ? '-' : '?');
			}
			putchar('\n');
		}
	}
	return wrong;
}


static void
write_ppm(const char* path, const uint8_t* pixels)
{
	FILE* file = fopen(path, "wb");
	if (file == NULL) {
		printf("cannot write %s\n", path);
		return;
	}
	fprintf(file, "P6\n%d %d\n255\n", SIZE, SIZE);
	for (int y = SIZE - 1; y >= 0; y--) {
		for (unsigned x = 0; x < SIZE; x++)
			fwrite(pixels + (y * SIZE + x) * 4, 1, 3, file);
	}
	fclose(file);
	printf("image written to %s\n", path);
}


// One run: EGL up, the checks, EGL down. Returns whether everything passed.
static int
probe(const char* expect, const char* ppmPath)
{
	double start = now_ms();
	EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	EGLint major = 0, minor = 0;
	if (!check(display != EGL_NO_DISPLAY
			&& eglInitialize(display, &major, &minor), "eglInitialize"))
		return 0;
	printf("eglInitialize: %.1f ms\n", now_ms() - start);
	printf("EGL %d.%d, vendor %s, version %s, client APIs %s\n", major, minor,
		eglQueryString(display, EGL_VENDOR),
		eglQueryString(display, EGL_VERSION),
		eglQueryString(display, EGL_CLIENT_APIS));

	const EGLint configAttributes[] = {
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
		EGL_ALPHA_SIZE, 8,
		EGL_NONE
	};
	EGLConfig config;
	EGLint count = 0;
	if (!check(eglChooseConfig(display, configAttributes, &config, 1, &count)
			&& count == 1, "eglChooseConfig (pbuffer, ES2, RGBA8888)"))
		return 0;
	EGLint alphaBits = 0;
	eglGetConfigAttrib(display, config, EGL_ALPHA_SIZE, &alphaBits);

	const EGLint surfaceAttributes[] = {
		EGL_WIDTH, SIZE, EGL_HEIGHT, SIZE, EGL_NONE
	};
	EGLSurface surface = eglCreatePbufferSurface(display, config,
		surfaceAttributes);
	eglBindAPI(EGL_OPENGL_ES_API);
	const EGLint contextAttributes[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE
	};
	start = now_ms();
	EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT,
		contextAttributes);
	if (!check(surface != EGL_NO_SURFACE && context != EGL_NO_CONTEXT
			&& eglMakeCurrent(display, surface, surface, context),
			"pbuffer, ES2 context, eglMakeCurrent"))
		return 0;
	printf("context: %.1f ms\n", now_ms() - start);

	const char* renderer = (const char*)glGetString(GL_RENDERER);
	printf("GL_VENDOR: %s\nGL_RENDERER: %s\nGL_VERSION: %s\n"
		"GL_SHADING_LANGUAGE_VERSION: %s\n",
		(const char*)glGetString(GL_VENDOR), renderer,
		(const char*)glGetString(GL_VERSION),
		(const char*)glGetString(GL_SHADING_LANGUAGE_VERSION));
	int ok = 1;
	if (expect != NULL) {
		ok &= check(renderer != NULL && strstr(renderer, expect) != NULL,
			"renderer is the expected one");
	}

	uint8_t* pixels = malloc(SIZE * SIZE * 4);
	glViewport(0, 0, SIZE, SIZE);

	// 1. clear only
	start = now_ms();
	glClearColor(0.2f, 0.4f, 0.6f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	memset(pixels, FILL_BYTE, SIZE * SIZE * 4);
	glReadPixels(0, 0, SIZE, SIZE, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	printf("clear + readback: %.2f ms, GL error 0x%x\n", now_ms() - start,
		glGetError());
	ok &= check(check_pixels(pixels, 0, alphaBits) == 0, "clear");

	// 2. the triangle
	start = now_ms();
	GLuint program = glCreateProgram();
	glAttachShader(program, compile(GL_VERTEX_SHADER,
		"attribute vec2 position;\n"
		"void main() { gl_Position = vec4(position, 0.0, 1.0); }\n"));
	// highp: mediump may be 16-bit, where 0.8 is 0.7998 (203.95 / 255)
	glAttachShader(program, compile(GL_FRAGMENT_SHADER,
		"#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
		"precision highp float;\n"
		"#else\n"
		"precision mediump float;\n"
		"#endif\n"
		"void main() { gl_FragColor = vec4(0.8, 0.6, 0.2, 0.4); }\n"));
	glBindAttribLocation(program, 0, "position");
	glLinkProgram(program);
	GLint linked = 0;
	glGetProgramiv(program, GL_LINK_STATUS, &linked);
	if (!check(linked, "program linked"))
		return 0;
	glUseProgram(program);
	printf("program: %.2f ms\n", now_ms() - start);

	// window (0, 0), (64.25, 0), (0, 64.25)
	static const GLfloat kVertices[] = {
		-1.0f, -1.0f, 1.0078125f, -1.0f, -1.0f, 1.0078125f
	};
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, kVertices);
	glEnableVertexAttribArray(0);
	start = now_ms();
	glClear(GL_COLOR_BUFFER_BIT);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	memset(pixels, FILL_BYTE, SIZE * SIZE * 4);
	glReadPixels(0, 0, SIZE, SIZE, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	printf("draw + readback: %.2f ms (first draw compiles), GL error 0x%x\n",
		now_ms() - start, glGetError());
	ok &= check(check_pixels(pixels, 1, alphaBits) == 0, "triangle");

	// the same again: a second frame, the pipeline compiled already
	start = now_ms();
	glClear(GL_COLOR_BUFFER_BIT);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	memset(pixels, FILL_BYTE, SIZE * SIZE * 4);
	glReadPixels(0, 0, SIZE, SIZE, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	printf("second draw + readback: %.2f ms\n", now_ms() - start);
	ok &= check(check_pixels(pixels, 1, alphaBits) == 0, "triangle again");
	if (ppmPath != NULL)
		write_ppm(ppmPath, pixels);

	free(pixels);
	eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroyContext(display, context);
	eglDestroySurface(display, surface);
	eglTerminate(display);
	return ok;
}


int
main(int argc, char** argv)
{
	// line by line: whatever was printed survives a crash in the driver
	setvbuf(stdout, NULL, _IOLBF, 0);

	const char* expect = NULL;
	const char* ppmPath = NULL;
	int repeat = 1;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--expect") == 0 && i + 1 < argc)
			expect = argv[++i];
		else if (strcmp(argv[i], "--ppm") == 0 && i + 1 < argc)
			ppmPath = argv[++i];
		else if (strcmp(argv[i], "--repeat") == 0 && i + 1 < argc)
			repeat = atoi(argv[++i]);
		else {
			printf("usage: %s [--expect TEXT] [--ppm FILE] [--repeat N]\n",
				argv[0]);
			return 2;
		}
	}
	if (repeat < 1)
		repeat = 1;

	int ok = 1;
	for (int run = 0; run < repeat; run++) {
		if (repeat > 1)
			printf("== run %d of %d\n", run + 1, repeat);
		if (!probe(expect, ppmPath))
			ok = 0;
	}
	printf("%s\n", ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
