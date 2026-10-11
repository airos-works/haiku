/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

// Summit's direct present through its own entry points
// (summit_haiku_present_framebuffer and friends, zink_haiku_present.h),
// found in the EGL vendor library as Summit's web process finds them (on
// air/OS with get_image_symbol(), elsewhere with dlsym()). An
// OpenGL ES 2 framebuffer object with a GL_BGRA8_EXT renderbuffer, made as
// Summit's web process makes its own (and needing the same extensions,
// GL_EXT_texture_format_BGRA8888 and GL_EXT_read_format_bgra), is cleared to
// a colour
// each frame and presented into a stand-in for app_server's copy of the
// screen: a malloc()ed 320x240 surface filled with 0xee, whose rows are
// padded. Two rectangles, partly outside the image, are copied each frame.
// The program checks that every present's completion callback came, that
// the rectangles' pixels (clipped to the image) are the colour and every
// other byte is still 0xee. It also checks the refusals: a framebuffer
// object that is not bound (-11), an RGBA image (-2), and with --front-bpr,
// a frame buffer that cannot be had (-3, the host) or is used.
//   pvr_glpresent [--frames N] [--front-bpr N] [--front-refused]
//       [--scene DRAWS] [--new-texture] [--library NAME] [--shim] [--lost]
// --front-bpr: also copy into the screen's frame buffer, whose rows have
// that many bytes (pvr_present prints it), where it must work, or with
// --front-refused be refused (-3, no frame buffer to be had); --scene: draw
// like a WebGL page before each present, DRAWS textured quads each with a
// glBufferSubData() of its vertices and a glUniform4f() of its colour, into
// a corner of the image the rectangles leave out (for counting what a frame
// allocates); --new-texture: the scene samples a new 512x512 texture each
// frame, the old one deleted, as WebKit makes one per frame; --library:
// where the entry points are (default libEGL_mesa.so.0 on air/OS); --shim:
// the GPU runs nothing (build.sh shim), so the rectangles must hold what the
// untouched staging buffer held, zeros; --lost: the device is lost on the
// way (injected), so only the callbacks and the end of the run count.


#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#ifdef __HAIKU__
#include <image.h>
#endif


#define SIZE			128
#define BACK_WIDTH		320
#define BACK_HEIGHT		240
#define BACK_PITCH		(BACK_WIDTH * 4 + 64)
#define FILL			0xee

// zink_haiku_present.h's interface, version 1
struct summit_haiku_present {
	uint32_t	version;
	int32_t		origin_x, origin_y;
	uint32_t	rect_count;
	const int32_t* rects;
	uint32_t	front_bytes_per_row;
	void*		back_bits;
	uint32_t	back_bytes_per_row;
	uint32_t	back_width, back_height;
	uint32_t	frames_in_flight;
	void		(*completed)(void* cookie);
	void*		cookie;
};

typedef int (*present_function)(unsigned framebuffer,
	const struct summit_haiku_present* present);
typedef void (*void_function)(void);

static volatile int sCompleted;


static void
completed(void* cookie)
{
	__atomic_add_fetch(&sCompleted, 1, __ATOMIC_SEQ_CST);
	(void)cookie;
}


// a function of the loaded library whose name ends in `library`
static void*
find_symbol(const char* library, const char* name)
{
#ifdef __HAIKU__
	image_info info;
	int32 cookie = 0;
	size_t length = strlen(library);
	while (get_next_image_info(B_CURRENT_TEAM, &cookie, &info) == B_OK) {
		size_t pathLength = strlen(info.name);
		if (pathLength < length
			|| strcmp(info.name + pathLength - length, library) != 0)
			continue;
		void* symbol = NULL;
		if (get_image_symbol(info.id, name, B_SYMBOL_TYPE_TEXT, &symbol)
				== B_OK)
			return symbol;
	}
	return NULL;
#else
	void* handle = dlopen(library, RTLD_NOW | RTLD_NOLOAD);
	return handle != NULL ? dlsym(handle, name) : NULL;
#endif
}


// a framebuffer object with a renderbuffer of that format, as Summit makes
// it; 0 if it is not complete
static GLuint
make_target(GLenum format, GLuint* renderbuffer)
{
	while (glGetError() != GL_NO_ERROR) {
	}
	glGenRenderbuffers(1, renderbuffer);
	glBindRenderbuffer(GL_RENDERBUFFER, *renderbuffer);
	glRenderbufferStorage(GL_RENDERBUFFER, format, SIZE, SIZE);
	GLenum error = glGetError();
	GLuint framebuffer;
	glGenFramebuffers(1, &framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		GL_RENDERBUFFER, *renderbuffer);
	if (error != GL_NO_ERROR
		|| glCheckFramebufferStatus(GL_FRAMEBUFFER)
			!= GL_FRAMEBUFFER_COMPLETE) {
		printf("renderbuffer format 0x%x: GL error 0x%x, framebuffer "
			"status 0x%x\n", format, error,
			glCheckFramebufferStatus(GL_FRAMEBUFFER));
		return 0;
	}
	return framebuffer;
}


// A WebGL page's frame in small: a textured quad shader, a vertex buffer
// rewritten before each draw, a colour uniform set for each. Drawn into
// columns 60-79 of the image, which neither rectangle covers (whichever
// way rows run).
static GLint
make_scene(void)
{
	static const char* vertex =
		"attribute vec2 position;\n"
		"varying vec2 uv;\n"
		"void main() {\n"
		"	uv = position * 0.5 + 0.5;\n"
		"	gl_Position = vec4(position, 0.0, 1.0);\n"
		"}\n";
	static const char* fragment =
		"precision mediump float;\n"
		"uniform sampler2D image;\n"
		"uniform vec4 color;\n"
		"varying vec2 uv;\n"
		"void main() { gl_FragColor = texture2D(image, uv) * color; }\n";
	GLuint program = glCreateProgram();
	GLuint shaders[2] = { glCreateShader(GL_VERTEX_SHADER),
		glCreateShader(GL_FRAGMENT_SHADER) };
	glShaderSource(shaders[0], 1, &vertex, NULL);
	glShaderSource(shaders[1], 1, &fragment, NULL);
	for (int i = 0; i < 2; i++) {
		glCompileShader(shaders[i]);
		glAttachShader(program, shaders[i]);
	}
	glBindAttribLocation(program, 0, "position");
	glLinkProgram(program);
	glUseProgram(program);

	static const uint8_t texels[4 * 4 * 4] = { 255, 255, 255, 255 };
	GLuint texture;
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA,
		GL_UNSIGNED_BYTE, texels);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glUniform1i(glGetUniformLocation(program, "image"), 0);

	GLuint buffer;
	glGenBuffers(1, &buffer);
	glBindBuffer(GL_ARRAY_BUFFER, buffer);
	glBufferData(GL_ARRAY_BUFFER, 8 * sizeof(GLfloat), NULL,
		GL_DYNAMIC_DRAW);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (const void*)0);
	glEnableVertexAttribArray(0);
	return glGetUniformLocation(program, "color");
}


// a new 512x512 texture for the scene, the old one deleted
static void
new_scene_texture(void)
{
	static GLuint texture;
	static uint8_t* texels;
	if (texels == NULL) {
		texels = malloc(512 * 512 * 4);
		memset(texels, 0xff, 512 * 512 * 4);
	}
	if (texture != 0)
		glDeleteTextures(1, &texture);
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 512, 512, 0, GL_RGBA,
		GL_UNSIGNED_BYTE, texels);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
}


static void
draw_scene(unsigned draws, GLint colorLocation, unsigned frame)
{
	glViewport(60, 40, 20, 20);
	glScissor(60, 40, 20, 20);
	glEnable(GL_SCISSOR_TEST);
	for (unsigned i = 0; i < draws; i++) {
		const GLfloat d = ((frame + i) % 16) / 32.0f;
		const GLfloat quad[8] = { -1 + d, -1, 1, -1 + d, -1, 1 - d, 1 - d, 1 };
		glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(quad), quad);
		glUniform4f(colorLocation, (i % 4) / 4.0f, 0.5f, 1.0f - d, 1.0f);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	}
	glDisable(GL_SCISSOR_TEST);
	glViewport(0, 0, SIZE, SIZE);
}


int
main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);

	unsigned frames = 30, frontBpr = 0;
	int shim = 0, lost = 0, frontRefused = 0;
	unsigned sceneDraws = 0;
	int newTexture = 0;
#ifdef __HAIKU__
	const char* library = "libEGL_mesa.so.0";
#else
	const char* library = "libgallium-26.2.4.so";
#endif
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
			frames = strtoul(argv[++i], NULL, 0);
		else if (strcmp(argv[i], "--front-bpr") == 0 && i + 1 < argc)
			frontBpr = strtoul(argv[++i], NULL, 0);
		else if (strcmp(argv[i], "--library") == 0 && i + 1 < argc)
			library = argv[++i];
		else if (strcmp(argv[i], "--shim") == 0)
			shim = 1;
		else if (strcmp(argv[i], "--front-refused") == 0)
			frontRefused = 1;
		else if (strcmp(argv[i], "--scene") == 0 && i + 1 < argc)
			sceneDraws = strtoul(argv[++i], NULL, 0);
		else if (strcmp(argv[i], "--new-texture") == 0)
			newTexture = 1;
		else if (strcmp(argv[i], "--lost") == 0)
			lost = 1;
		else {
			printf("usage: %s [--frames N] [--front-bpr N] [--front-refused] "
				"[--scene DRAWS] [--new-texture] [--library NAME] [--shim] "
				"[--lost]\n", argv[0]);
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
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
		EGL_NONE
	};
	EGLConfig config;
	EGLint count = 0;
	if (!eglChooseConfig(display, configAttributes, &config, 1, &count)
		|| count != 1) {
		printf("FAIL: eglChooseConfig\n");
		return 1;
	}
	const EGLint surfaceAttributes[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16,
		EGL_NONE };
	EGLSurface surface = eglCreatePbufferSurface(display, config,
		surfaceAttributes);
	eglBindAPI(EGL_OPENGL_ES_API);
	const EGLint contextAttributes[] = { EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE };
	EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT,
		contextAttributes);
	if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT
		|| !eglMakeCurrent(display, surface, surface, context)) {
		printf("FAIL: pbuffer, ES2 context, eglMakeCurrent\n");
		return 1;
	}
	printf("GL_RENDERER: %s\n", (const char*)glGetString(GL_RENDERER));
	// what Summit asks for before it makes a BGRA target; without them it
	// makes an RGBA one, which cannot be presented directly (-2)
	const char* extensions = (const char*)glGetString(GL_EXTENSIONS);
	const char* wanted[] = { "GL_EXT_texture_format_BGRA8888",
		"GL_EXT_read_format_bgra" };
	for (int i = 0; i < 2; i++) {
		if (extensions == NULL || strstr(extensions, wanted[i]) == NULL) {
			printf("FAIL: no %s: Summit would composite into RGBA\n",
				wanted[i]);
			return 1;
		}
	}

	present_function present = (present_function)find_symbol(library,
		"summit_haiku_present_framebuffer");
	void_function waitIdle = (void_function)find_symbol(library,
		"summit_haiku_present_wait_idle");
	void_function release = (void_function)find_symbol(library,
		"summit_haiku_present_release");
	if (present == NULL || waitIdle == NULL || release == NULL) {
		printf("FAIL: no summit_haiku_present_* in %s\n", library);
		return 1;
	}

	int ok = 1;
	uint8_t* back = malloc((size_t)BACK_PITCH * BACK_HEIGHT);
	memset(back, FILL, (size_t)BACK_PITCH * BACK_HEIGHT);
	// both edges included; the image lands at (40, 30)
	const int32_t origin[2] = { 40, 30 };
	const int32_t rects[] = {
		20, 10, 99, 69,			// partly left of and above the image
		120, 100, 200, 199,		// partly right of and below it
	};
	struct summit_haiku_present request = {
		.version = 1,
		.origin_x = origin[0],
		.origin_y = origin[1],
		.rect_count = 2,
		.rects = rects,
		.front_bytes_per_row = 0,
		.back_bits = back,
		.back_bytes_per_row = BACK_PITCH,
		.back_width = BACK_WIDTH,
		.back_height = BACK_HEIGHT,
		.frames_in_flight = 2,
		.completed = completed,
		.cookie = NULL,
	};

	// the refusals: an RGBA image, and a framebuffer object not bound
	GLuint rgbaRenderbuffer;
	GLuint rgba = make_target(0x8058 /* GL_RGBA8_OES */, &rgbaRenderbuffer);
	int result = present(rgba, &request);
	printf("RGBA image: %d (expected -2)\n", result);
	ok &= result == -2;
	GLuint bgraRenderbuffer;
	GLuint bgra = make_target(0x93a1 /* GL_BGRA8_EXT */, &bgraRenderbuffer);
	if (rgba == 0 || bgra == 0) {
		printf("FAIL: no %s target: Summit would composite into RGBA\n",
			bgra == 0 ? "GL_BGRA8_EXT" : "GL_RGBA8_OES");
		return 1;
	}
	result = present(rgba, &request);
	printf("a framebuffer object that is not bound: %d (expected -11)\n",
		result);
	ok &= result == -11;
	if (frontBpr != 0) {
		request.front_bytes_per_row = frontBpr;
		glClearColor(0, 0, 0, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		result = present(bgra, &request);
		printf("with the frame buffer (%u bytes per row): %d (expected %s)\n",
			frontBpr, result, frontRefused ? "-3" : "0");
		if (frontRefused) {
			ok &= result == -3;
			request.front_bytes_per_row = 0;
		} else
			ok &= result == 0;
	}

	// the frames: B, G, R, A = 191, 128, 64, 255
	const uint8_t color[4] = { 191, 128, 64, 255 };
	int presented = frontBpr != 0 && !frontRefused ? 1 : 0;
	GLint colorLocation = -1;
	if (sceneDraws != 0)
		colorLocation = make_scene();
	glViewport(0, 0, SIZE, SIZE);
	for (unsigned frame = 0; frame < frames; frame++) {
		glClearColor(64 / 255.0f, 128 / 255.0f, 191 / 255.0f, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		if (sceneDraws != 0 && newTexture)
			new_scene_texture();
		if (sceneDraws != 0)
			draw_scene(sceneDraws, colorLocation, frame);
		result = present(bgra, &request);
		if (result != 0) {
			printf("FAIL: frame %u: present %d\n", frame, result);
			ok = 0;
			break;
		}
		presented++;
	}
	waitIdle();
	printf("%d presents, %d completions\n", presented, sCompleted);
	ok &= sCompleted == presented;

	// what landed: the rectangles clipped to the image are the colour (or
	// zeros under the shim), the rest is untouched, the padding included
	unsigned inside = 0, wrongInside = 0, wrongOutside = 0;
	for (int y = 0; y < BACK_HEIGHT; y++) {
		for (int x = 0; x < BACK_PITCH; x++) {
			int column = x / 4;
			int in = 0;
			for (int r = 0; r < 2 && column < BACK_WIDTH; r++) {
				const int32_t* rect = rects + r * 4;
				if (column >= rect[0] && column <= rect[2] && y >= rect[1]
					&& y <= rect[3] && column >= origin[0]
					&& column < origin[0] + SIZE && y >= origin[1]
					&& y < origin[1] + SIZE)
					in = 1;
			}
			uint8_t value = back[(size_t)y * BACK_PITCH + x];
			if (in) {
				inside++;
				uint8_t want = shim ? 0 : color[x % 4];
				if (value != want && !lost)
					wrongInside++;
			} else if (value != FILL)
				wrongOutside++;
		}
	}
	printf("back copy: %u bytes in the rectangles, %u wrong; %u bytes "
		"outside changed\n", inside, wrongInside, wrongOutside);
	ok &= wrongInside == 0 && wrongOutside == 0 && inside != 0;

	release();
	free(back);
	glDeleteFramebuffers(1, &rgba);
	glDeleteFramebuffers(1, &bgra);
	glDeleteRenderbuffers(1, &rgbaRenderbuffer);
	glDeleteRenderbuffers(1, &bgraRenderbuffer);
	eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroyContext(display, context);
	eglDestroySurface(display, surface);
	eglTerminate(display);
	printf("%s\n", ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
