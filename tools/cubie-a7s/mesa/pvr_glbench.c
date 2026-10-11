/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

// A steady OpenGL ES 2 frame loop through the system EGL (zink on the
// PowerVR Vulkan driver), shaped like GLTeapot's on zink: every frame
// clears colour and depth, draws one lit, rotating torus of 3072 triangles
// with one glDrawElements(), flushes, and reads a region of up to 300x300
// back with glReadPixels(), as each BGLView swap does. Every interval it
// prints one line:
//   - frames/s in that window;
//   - average ms per frame for draw + glFlush() and for the readback (the
//     readback waits for the GPU, so GPU time lands there);
//   - the process's areas (its "powervr buffer" areas are MAP_BO clones);
//   - the kernel's "powervr ..." areas (needs root);
//   - the share of read-back pixels the torus covered in the last frame.
// At the end, the areas whose count changed since the first frame, by name.
//   pvr_glbench [--seconds N] [--size WxH] [--readback WxH] [--interval S]
//               [--frames N] [--resize N] [--expect TEXT] [--mark]
// --seconds: run time (default 300; 0 = until --frames or Ctrl+C);
// --size: the pbuffer (default 300x300); --readback: the region read each
// frame, centred (default 300x300, clipped to the pbuffer; 0x0 replaces
// the readback with glFinish(), to tell the two costs apart); --interval:
// seconds per line (default 10); --frames: stop after N frames;
// --resize: every N frames, draw to the next of three pbuffers, the --size
// one, 3/4 and 1/2 of it (render targets of new sizes, then the same sizes
// again); --expect: fail unless GL_RENDERER contains TEXT; --mark: print
// "== frame N" before each frame (lines for a trace to be cut at).
// Frame 0 compiles the pipeline: it is timed on its own line, outside the
// windows. FAIL means setup failed, GL reported an error, or no frame drew
// a pixel (the case under build.sh shim, where the GPU runs nothing).


#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include "pvr_bench_areas.h"


#define TORUS_RINGS		64
#define TORUS_SIDES		24
#define VERTEX_COUNT	((TORUS_RINGS + 1) * (TORUS_SIDES + 1))
#define INDEX_COUNT		(TORUS_RINGS * TORUS_SIDES * 6)

static const uint8_t kClearBytes[4] = { 26, 26, 51, 255 };

static volatile sig_atomic_t sStop;
static struct area_snapshot sOwnStart, sOwnNow, sKernelStart, sKernelNow;


static double
now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}


static void
stop_handler(int number)
{
	sStop = 1;
}


// the share of the pixels read back that the torus drew: opaque, and not
// the clear colour (memory nothing wrote, zeros under the shim, is neither)
static double
coverage_of(const uint8_t* pixels, size_t size)
{
	unsigned covered = 0;
	for (size_t i = 0; i < size; i += 4) {
		if (pixels[i + 3] == 255 && memcmp(pixels + i, kClearBytes, 3) != 0)
			covered++;
	}
	return (double)covered / (size / 4);
}


static int
parse_size(const char* text, int* _width, int* _height)
{
	if (strcmp(text, "0") == 0) {
		*_width = *_height = 0;
		return 1;
	}
	return sscanf(text, "%dx%d", _width, _height) == 2 && *_width >= 0
		&& *_height >= 0;
}


// column-major 4x4 matrices, as GL takes them
static void
multiply(float* out, const float* a, const float* b)
{
	float result[16];
	for (int column = 0; column < 4; column++) {
		for (int row = 0; row < 4; row++) {
			float sum = 0;
			for (int k = 0; k < 4; k++)
				sum += a[k * 4 + row] * b[column * 4 + k];
			result[column * 4 + row] = sum;
		}
	}
	memcpy(out, result, sizeof(result));
}


static void
model_matrix(float* m, float angle)
{
	// rotation about y by angle, then about x by 0.7 * angle
	float cy = cosf(angle), sy = sinf(angle);
	float cx = cosf(angle * 0.7f), sx = sinf(angle * 0.7f);
	const float ry[16] = { cy, 0, -sy, 0, 0, 1, 0, 0, sy, 0, cy, 0,
		0, 0, 0, 1 };
	const float rx[16] = { 1, 0, 0, 0, 0, cx, sx, 0, 0, -sx, cx, 0,
		0, 0, 0, 1 };
	multiply(m, ry, rx);
}


static void
projection_matrix(float* m, float aspect)
{
	// 45 degrees vertically, near 1, far 10, the eye 3.5 units away
	const float f = 1.0f / tanf(22.5f * (float)M_PI / 180.0f);
	const float nearZ = 1.0f, farZ = 10.0f;
	const float projection[16] = {
		f / aspect, 0, 0, 0,
		0, f, 0, 0,
		0, 0, (farZ + nearZ) / (nearZ - farZ), -1,
		0, 0, 2 * farZ * nearZ / (nearZ - farZ), 0
	};
	const float view[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0,
		0, 0, -3.5f, 1 };
	multiply(m, projection, view);
}


// position and normal, interleaved; a torus around the y axis
static void
make_torus(GLfloat* vertices, GLushort* indices)
{
	const float major = 0.75f, minor = 0.3f;
	for (int i = 0; i <= TORUS_RINGS; i++) {
		float u = 2 * (float)M_PI * i / TORUS_RINGS;
		for (int j = 0; j <= TORUS_SIDES; j++) {
			float v = 2 * (float)M_PI * j / TORUS_SIDES;
			GLfloat* vertex = vertices + (i * (TORUS_SIDES + 1) + j) * 6;
			vertex[0] = (major + minor * cosf(v)) * cosf(u);
			vertex[1] = minor * sinf(v);
			vertex[2] = (major + minor * cosf(v)) * sinf(u);
			vertex[3] = cosf(v) * cosf(u);
			vertex[4] = sinf(v);
			vertex[5] = cosf(v) * sinf(u);
		}
	}
	for (int i = 0; i < TORUS_RINGS; i++) {
		for (int j = 0; j < TORUS_SIDES; j++) {
			GLushort a = i * (TORUS_SIDES + 1) + j;
			GLushort b = a + TORUS_SIDES + 1;
			GLushort* quad = indices + (i * TORUS_SIDES + j) * 6;
			quad[0] = a;
			quad[1] = b;
			quad[2] = a + 1;
			quad[3] = a + 1;
			quad[4] = b;
			quad[5] = b + 1;
		}
	}
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


static const char* kVertexShader =
	"uniform mat4 mvp;\n"
	"uniform mat4 model;\n"
	"attribute vec3 position;\n"
	"attribute vec3 normal;\n"
	"varying vec3 color;\n"
	"void main() {\n"
	"	vec3 n = normalize((model * vec4(normal, 0.0)).xyz);\n"
	"	vec3 l = normalize(vec3(0.4, 0.6, 1.0));\n"
	"	vec3 h = normalize(l + vec3(0.0, 0.0, 1.0));\n"
	"	float diffuse = max(dot(n, l), 0.0);\n"
	"	float specular = pow(max(dot(n, h), 0.0), 32.0);\n"
	"	color = vec3(0.12, 0.08, 0.04) + diffuse * vec3(0.8, 0.55, 0.2)\n"
	"		+ specular * vec3(0.5);\n"
	"	gl_Position = mvp * vec4(position, 1.0);\n"
	"}\n";

static const char* kFragmentShader =
	"precision mediump float;\n"
	"varying vec3 color;\n"
	"void main() { gl_FragColor = vec4(min(color, 1.0), 1.0); }\n";


// a pbuffer to draw to, with its readback region and projection
struct target {
	EGLSurface	surface;
	int			width;
	int			height;
	int			readX;
	int			readY;
	int			readWidth;
	int			readHeight;
	float		projection[16];
};


struct window {
	unsigned	frames;
	double		drawMs;
	double		readMs;
	double		start;
};


static void
report(const struct window* window, double now, double runStart,
	double coverage, int readback)
{
	double seconds = (now - window->start) / 1000.0;
	unsigned frames = window->frames != 0 ? window->frames : 1;
	area_snapshot_own(&sOwnNow);
	area_snapshot_kernel(&sKernelNow);
	printf("[%6.1f s] %5u frames %6.1f fps; ms/frame draw+flush %.3f, "
		"%s %.3f", (now - runStart) / 1000.0, window->frames,
		window->frames / seconds, window->drawMs / frames,
		readback ? "readback" : "finish", window->readMs / frames);
	area_print(stdout, "areas", &sOwnNow);
	area_print(stdout, "kernel", &sKernelNow);
	printf("; covered %.1f%%\n", coverage * 100);
}


int
main(int argc, char** argv)
{
	// line by line: whatever was printed survives a crash in the driver
	setvbuf(stdout, NULL, _IOLBF, 0);

	double seconds = 300, interval = 10;
	int width = 300, height = 300, readWidth = 300, readHeight = 300;
	unsigned maxFrames = 0, resizeEvery = 0;
	int mark = 0;
	const char* expect = NULL;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc)
			seconds = atof(argv[++i]);
		else if (strcmp(argv[i], "--interval") == 0 && i + 1 < argc)
			interval = atof(argv[++i]);
		else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
			maxFrames = strtoul(argv[++i], NULL, 0);
		else if (strcmp(argv[i], "--resize") == 0 && i + 1 < argc)
			resizeEvery = strtoul(argv[++i], NULL, 0);
		else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc
			&& parse_size(argv[i + 1], &width, &height) && width > 0
			&& height > 0)
			i++;
		else if (strcmp(argv[i], "--readback") == 0 && i + 1 < argc
			&& parse_size(argv[i + 1], &readWidth, &readHeight))
			i++;
		else if (strcmp(argv[i], "--expect") == 0 && i + 1 < argc)
			expect = argv[++i];
		else if (strcmp(argv[i], "--mark") == 0)
			mark = 1;
		else {
			printf("usage: %s [--seconds N] [--size WxH] [--readback WxH] "
				"[--interval S] [--frames N] [--resize N] [--expect TEXT] "
				"[--mark]\n", argv[0]);
			return 2;
		}
	}
	if (interval <= 0)
		interval = 10;
	const int readback = readWidth > 0 && readHeight > 0;

	// the pbuffers: the --size one, and with --resize 3/4 and 1/2 of it,
	// each with its centred readback region and its projection
	struct target targets[3];
	const int targetCount = resizeEvery != 0 ? 3 : 1;
	for (int i = 0; i < targetCount; i++) {
		struct target* target = &targets[i];
		static const int kScale[3] = { 4, 3, 2 };
		target->width = width * kScale[i] / 4 > 0 ? width * kScale[i] / 4 : 1;
		target->height = height * kScale[i] / 4 > 0
			? height * kScale[i] / 4 : 1;
		target->readWidth = readWidth < target->width
			? readWidth : target->width;
		target->readHeight = readHeight < target->height
			? readHeight : target->height;
		target->readX = (target->width - target->readWidth) / 2;
		target->readY = (target->height - target->readHeight) / 2;
		projection_matrix(target->projection,
			(float)target->width / target->height);
	}

	signal(SIGINT, stop_handler);
	signal(SIGTERM, stop_handler);

	EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	EGLint major = 0, minor = 0;
	if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor)) {
		printf("FAIL: eglInitialize\n");
		return 1;
	}
	printf("EGL %d.%d, vendor %s\n", major, minor,
		eglQueryString(display, EGL_VENDOR));

	const EGLint configAttributes[] = {
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
		EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 16,
		EGL_NONE
	};
	EGLConfig config;
	EGLint count = 0;
	if (!eglChooseConfig(display, configAttributes, &config, 1, &count)
		|| count != 1) {
		printf("FAIL: eglChooseConfig (pbuffer, ES2, RGBA8888, depth)\n");
		return 1;
	}
	EGLSurface surface = EGL_NO_SURFACE;
	for (int i = 0; i < targetCount; i++) {
		const EGLint surfaceAttributes[] = {
			EGL_WIDTH, targets[i].width, EGL_HEIGHT, targets[i].height,
			EGL_NONE
		};
		targets[i].surface = eglCreatePbufferSurface(display, config,
			surfaceAttributes);
		if (targets[i].surface == EGL_NO_SURFACE) {
			printf("FAIL: eglCreatePbufferSurface %dx%d\n", targets[i].width,
				targets[i].height);
			return 1;
		}
	}
	surface = targets[0].surface;
	eglBindAPI(EGL_OPENGL_ES_API);
	const EGLint contextAttributes[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE
	};
	EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT,
		contextAttributes);
	if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT
		|| !eglMakeCurrent(display, surface, surface, context)) {
		printf("FAIL: pbuffer, ES2 context, eglMakeCurrent\n");
		return 1;
	}

	const char* renderer = (const char*)glGetString(GL_RENDERER);
	printf("GL_RENDERER: %s\nGL_VERSION: %s\n", renderer,
		(const char*)glGetString(GL_VERSION));
	int ok = 1;
	if (expect != NULL && (renderer == NULL || strstr(renderer, expect)
			== NULL)) {
		printf("FAIL: the renderer is not \"%s\"\n", expect);
		ok = 0;
	}

	GLuint program = glCreateProgram();
	glAttachShader(program, compile(GL_VERTEX_SHADER, kVertexShader));
	glAttachShader(program, compile(GL_FRAGMENT_SHADER, kFragmentShader));
	glBindAttribLocation(program, 0, "position");
	glBindAttribLocation(program, 1, "normal");
	glLinkProgram(program);
	GLint linked = 0;
	glGetProgramiv(program, GL_LINK_STATUS, &linked);
	if (!linked) {
		printf("FAIL: program did not link\n");
		return 1;
	}
	glUseProgram(program);
	GLint mvpLocation = glGetUniformLocation(program, "mvp");
	GLint modelLocation = glGetUniformLocation(program, "model");

	GLfloat* vertices = malloc(VERTEX_COUNT * 6 * sizeof(GLfloat));
	GLushort* indices = malloc(INDEX_COUNT * sizeof(GLushort));
	make_torus(vertices, indices);
	GLuint buffers[2];
	glGenBuffers(2, buffers);
	glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
	glBufferData(GL_ARRAY_BUFFER, VERTEX_COUNT * 6 * sizeof(GLfloat),
		vertices, GL_STATIC_DRAW);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers[1]);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, INDEX_COUNT * sizeof(GLushort),
		indices, GL_STATIC_DRAW);
	free(vertices);
	free(indices);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(GLfloat),
		(const void*)0);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(GLfloat),
		(const void*)(3 * sizeof(GLfloat)));
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);

	glViewport(0, 0, width, height);
	const struct target* target = &targets[0];
	glEnable(GL_DEPTH_TEST);
	glEnable(GL_CULL_FACE);
	glClearColor(kClearBytes[0] / 255.0f, kClearBytes[1] / 255.0f,
		kClearBytes[2] / 255.0f, 1.0f);

	// the first target is the largest
	size_t readSize = readback
		? (size_t)target->readWidth * target->readHeight * 4 : 4;
	uint8_t* pixels = malloc(readSize);
	printf("pbuffer %dx%d, %d triangles per frame, ", width, height,
		INDEX_COUNT / 3);
	if (readback) {
		printf("reading back %dx%d at (%d, %d)\n", target->readWidth,
			target->readHeight, target->readX, target->readY);
	} else
		printf("glFinish() instead of a readback\n");
	if (resizeEvery != 0) {
		printf("every %u frames the next of %dx%d, %dx%d, %dx%d\n",
			resizeEvery, targets[0].width, targets[0].height, targets[1].width,
			targets[1].height, targets[2].width, targets[2].height);
	}

	double runStart = now_ms();
	struct window window = { 0, 0, 0, 0 };
	unsigned frame = 0;
	unsigned totalFrames = 0;
	unsigned glErrors = 0;
	double bestCoverage = 0;
	double firstFps = -1, lastFps = -1;
	for (;; frame++) {
		double start = now_ms();
		if (sStop || (maxFrames != 0 && frame >= maxFrames)
			|| (seconds > 0 && start - runStart >= seconds * 1000))
			break;
		if (mark)
			printf("== frame %u\n", frame);
		if (resizeEvery != 0 && frame != 0 && frame % resizeEvery == 0) {
			target = &targets[(frame / resizeEvery) % targetCount];
			if (!eglMakeCurrent(display, target->surface, target->surface,
					context)) {
				printf("FAIL: eglMakeCurrent %dx%d\n", target->width,
					target->height);
				ok = 0;
				break;
			}
			glViewport(0, 0, target->width, target->height);
			readSize = readback
				? (size_t)target->readWidth * target->readHeight * 4 : 4;
			if (mark)
				printf("== size %dx%d\n", target->width, target->height);
		}

		float model[16], mvp[16];
		model_matrix(model, frame * (float)M_PI / 180.0f);
		multiply(mvp, target->projection, model);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		glUniformMatrix4fv(mvpLocation, 1, GL_FALSE, mvp);
		glUniformMatrix4fv(modelLocation, 1, GL_FALSE, model);
		glDrawElements(GL_TRIANGLES, INDEX_COUNT, GL_UNSIGNED_SHORT,
			(const void*)0);
		glFlush();
		double drawn = now_ms();
		if (readback) {
			glReadPixels(target->readX, target->readY, target->readWidth,
				target->readHeight, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
		} else
			glFinish();
		double done = now_ms();
		totalFrames++;

		if (frame == 0) {
			// the first frame compiles the pipeline: not part of a window
			printf("frame 0: draw+flush %.2f ms, %s %.2f ms (compiles)\n",
				drawn - start, readback ? "readback" : "finish",
				done - drawn);
			area_snapshot_own(&sOwnStart);
			area_snapshot_kernel(&sKernelStart);
			window.start = now_ms();
			runStart = window.start;
			continue;
		}
		window.frames++;
		window.drawMs += drawn - start;
		window.readMs += done - drawn;

		if (done - window.start < interval * 1000)
			continue;
		GLenum error = glGetError();
		if (error != GL_NO_ERROR) {
			printf("GL error 0x%x\n", error);
			glErrors++;
		}
		double coverage = readback ? coverage_of(pixels, readSize) : 0;
		if (coverage > bestCoverage)
			bestCoverage = coverage;
		report(&window, done, runStart, coverage, readback);
		double fps = window.frames / ((done - window.start) / 1000.0);
		if (firstFps < 0)
			firstFps = fps;
		lastFps = fps;
		window = (struct window){ 0, 0, 0, done };
	}
	double end = now_ms();
	if (window.frames != 0) {
		// the rest, a shorter window
		double coverage = readback ? coverage_of(pixels, readSize) : 0;
		if (coverage > bestCoverage)
			bestCoverage = coverage;
		report(&window, end, runStart, coverage, readback);
		double fps = window.frames / ((end - window.start) / 1000.0);
		if (firstFps < 0)
			firstFps = fps;
		lastFps = fps;
	}
	GLenum error = glGetError();
	if (error != GL_NO_ERROR) {
		printf("GL error 0x%x\n", error);
		glErrors++;
	}

	printf("%u frames in %.1f s", totalFrames, (end - runStart) / 1000.0);
	if (firstFps > 0)
		printf("; first window %.1f fps, last %.1f fps", firstFps, lastFps);
	printf("\n");
	if (totalFrames > 1) {
		area_snapshot_own(&sOwnNow);
		area_snapshot_kernel(&sKernelNow);
		area_print_changes(stdout, "process areas since frame 0",
			&sOwnStart, &sOwnNow);
		area_print_changes(stdout, "kernel powervr areas since frame 0",
			&sKernelStart, &sKernelNow);
	}

	free(pixels);
	glDeleteBuffers(2, buffers);
	glDeleteProgram(program);
	eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroyContext(display, context);
	for (int i = 0; i < targetCount; i++)
		eglDestroySurface(display, targets[i].surface);
	eglTerminate(display);

	if (glErrors != 0)
		ok = 0;
	if (readback && bestCoverage == 0 && totalFrames > 1) {
		printf("no frame drew a pixel\n");
		ok = 0;
	}
	printf("%s\n", ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
