/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
// OpenGL ES 3.0 behaviour that WebGL 2 (Summit, through ANGLE) checks, on
// zink and the PowerVR Vulkan driver, offscreen through the system EGL. Each
// case is the native form of a Khronos WebGL 2 conformance test that failed
// in Summit:
//   limits       GL_MAX_VARYING_COMPONENTS >= 60 (state/gl-get-calls)
//   varyings     a program with 15 vec4 varyings draws them exactly
//   texelfetch   texelFetch() outside the texture returns 0
//                (textures/misc/texel-fetch-undefined)
//   helpers      helper invocations write no memory (an image store from a
//                fragment shader; skipped where there are none: PowerVR
//                has no fragmentStoresAndAtomics, and Mesa's glReadPixels
//                into a pixel pack buffer used them)
//   cf           a compute shader's guarded stores stay guarded, nested in
//                various ways (Mesa's glReadPixels into a buffer is one)
//   pack         glReadPixels with PACK_ALIGNMENT / ROW_LENGTH / SKIP_*
//                into memory and into a pixel pack buffer, padding untouched
//                (reading/read-pixels-pack-parameters)
//   fboread      glReadPixels from a texture of each colour-renderable
//                format, in its own and the implementation's read format
//                (reading/read-pixels-from-fbo-test)
//   blit         glBlitFramebuffer with source and destination partly
//                outside the buffers (rendering/blitframebuffer-filter-
//                outofbounds; Mesa core asserted on one)
// The context's reset status is checked after each case: a GPU fault shows
// up as a lost context.
// --case bench times GPU-bound passes instead (core clock comparisons).
//   pvr_glconform [--case NAME] [--verbose]
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>

#ifndef GL_GUILTY_CONTEXT_RESET
#define GL_GUILTY_CONTEXT_RESET		0x8253
#endif

static unsigned sFailures;
static int sVerbose;
static GLenum (*sGetGraphicsResetStatus)(void);

static int
check(int ok, const char* what)
{
	if (!ok || sVerbose)
		printf("%s: %s\n", ok ? "ok" : "FAIL", what);
	if (!ok)
		sFailures++;
	return ok;
}

static int
checkf(int ok, const char* format, ...)
{
	char buffer[512];
	va_list args;
	va_start(args, format);
	vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	return check(ok, buffer);
}

static GLenum
gl_error(void)
{
	GLenum error = glGetError(), next;
	// drain the rest
	while ((next = glGetError()) != GL_NO_ERROR)
		;
	return error;
}

static GLuint
compile(GLenum type, const char* source)
{
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);
	GLint ok = 0;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[2048];
		glGetShaderInfoLog(shader, sizeof(log), NULL, log);
		printf("shader compile failed: %s\n", log);
	}
	return shader;
}

static GLuint
program(const char* vs, const char* fs)
{
	GLuint p = glCreateProgram();
	glAttachShader(p, compile(GL_VERTEX_SHADER, vs));
	glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs));
	glBindAttribLocation(p, 0, "aPosition");
	glLinkProgram(p);
	GLint ok = 0;
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[2048];
		glGetProgramInfoLog(p, sizeof(log), NULL, log);
		printf("program link failed: %s\n", log);
		return 0;
	}
	return p;
}

// a quad over the whole viewport, attribute 0
static void
draw_quad(void)
{
	static const float kQuad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
	static GLuint buffer;
	if (buffer == 0) {
		glGenBuffers(1, &buffer);
		glBindBuffer(GL_ARRAY_BUFFER, buffer);
		glBufferData(GL_ARRAY_BUFFER, sizeof(kQuad), kQuad, GL_STATIC_DRAW);
	}
	glBindBuffer(GL_ARRAY_BUFFER, buffer);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

// an RGBA8 texture of width x height as frame buffer, bound for drawing and
// reading
static GLuint
rgba8_fbo(int width, int height, const uint8_t* pixels, GLuint* textureOut)
{
	GLuint texture, fbo;
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA,
		GL_UNSIGNED_BYTE, pixels);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		GL_TEXTURE_2D, texture, 0);
	if (textureOut != NULL)
		*textureOut = texture;
	return fbo;
}

static void
delete_fbo(GLuint fbo, GLuint texture)
{
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glDeleteFramebuffers(1, &fbo);
	glDeleteTextures(1, &texture);
}

// -- limits ---------------------------------------------------------------

static void
case_limits(void)
{
	GLint components = 0, vectors = 0, vsOut = 0, fsIn = 0;
	glGetIntegerv(GL_MAX_VARYING_COMPONENTS, &components);
	glGetIntegerv(GL_MAX_VARYING_VECTORS, &vectors);
	glGetIntegerv(GL_MAX_VERTEX_OUTPUT_COMPONENTS, &vsOut);
	glGetIntegerv(GL_MAX_FRAGMENT_INPUT_COMPONENTS, &fsIn);
	printf("varyings: %d components, %d vectors; vertex outputs %d, fragment "
		"inputs %d\n", components, vectors, vsOut, fsIn);
	checkf(components >= 60, "GL_MAX_VARYING_COMPONENTS %d >= 60", components);
	checkf(vectors >= 15, "GL_MAX_VARYING_VECTORS %d >= 15", vectors);
	checkf(vsOut >= 64, "GL_MAX_VERTEX_OUTPUT_COMPONENTS %d >= 64", vsOut);
	checkf(fsIn >= 60, "GL_MAX_FRAGMENT_INPUT_COMPONENTS %d >= 60", fsIn);

	// every colour-renderable format that is not integer takes up to
	// GL_MAX_SAMPLES samples (rendering/blitframebuffer-test resolves a
	// multisampled SRGB8_ALPHA8 renderbuffer)
	GLint maxSamples = 0;
	glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
	static const struct { const char* name; GLenum format; } kFormats[] = {
		{ "RGBA8", GL_RGBA8 }, { "SRGB8_ALPHA8", GL_SRGB8_ALPHA8 },
		{ "RGB565", GL_RGB565 }, { "RGB10_A2", GL_RGB10_A2 },
		{ "RGBA4", GL_RGBA4 }, { "RGB5_A1", GL_RGB5_A1 }, { "R8", GL_R8 },
		{ "RG8", GL_RG8 },
	};
	for (unsigned i = 0; i < sizeof(kFormats) / sizeof(kFormats[0]); i++) {
		GLint counts = 0, samples[8] = { 0 };
		glGetInternalformativ(GL_RENDERBUFFER, kFormats[i].format,
			GL_NUM_SAMPLE_COUNTS, 1, &counts);
		glGetInternalformativ(GL_RENDERBUFFER, kFormats[i].format,
			GL_SAMPLES, 8, samples);
		checkf(counts > 0 && samples[0] >= maxSamples, "%s: %d sample "
			"counts, largest %d (GL_MAX_SAMPLES %d)", kFormats[i].name,
			counts, samples[0], maxSamples);
	}
}

// -- varyings -------------------------------------------------------------

// 15 vec4 varyings, every component a different function of the vertex
// position; the fragment shader compares each with what interpolation must
// give and writes green only if all match.
static void
case_varyings(void)
{
	char vs[4096], fs[8192];
	int n = snprintf(vs, sizeof(vs), "#version 300 es\n"
		"in vec2 aPosition;\n");
	for (int i = 0; i < 15; i++)
		n += snprintf(vs + n, sizeof(vs) - n, "out vec4 v%d;\n", i);
	n += snprintf(vs + n, sizeof(vs) - n, "void main() {\n"
		"  gl_Position = vec4(aPosition, 0.0, 1.0);\n");
	for (int i = 0; i < 15; i++) {
		n += snprintf(vs + n, sizeof(vs) - n, "  v%d = vec4(aPosition.x * "
			"%d.0, aPosition.y * %d.0, %d.0, aPosition.x + aPosition.y + "
			"%d.0);\n", i, i + 1, i + 2, i, i * 3);
	}
	snprintf(vs + n, sizeof(vs) - n, "}\n");

	n = snprintf(fs, sizeof(fs), "#version 300 es\n"
		"precision highp float;\n"
		"uniform vec2 uSize;\n"
		"out vec4 color;\n");
	for (int i = 0; i < 15; i++)
		n += snprintf(fs + n, sizeof(fs) - n, "in vec4 v%d;\n", i);
	n += snprintf(fs + n, sizeof(fs) - n, "void main() {\n"
		"  vec2 p = gl_FragCoord.xy / uSize * 2.0 - 1.0;\n"
		"  float e = 0.0;\n");
	for (int i = 0; i < 15; i++) {
		n += snprintf(fs + n, sizeof(fs) - n, "  e = max(e, length(v%d - "
			"vec4(p.x * %d.0, p.y * %d.0, %d.0, p.x + p.y + %d.0)));\n", i,
			i + 1, i + 2, i, i * 3);
	}
	snprintf(fs + n, sizeof(fs) - n, "  color = e < 0.01 ? vec4(0, 1, 0, 1) "
		": vec4(1, 0, 0, 1);\n}\n");

	GLuint p = program(vs, fs);
	if (!check(p != 0, "15 vec4 varyings link"))
		return;
	GLuint texture, fbo = rgba8_fbo(32, 32, NULL, &texture);
	glViewport(0, 0, 32, 32);
	glUseProgram(p);
	glUniform2f(glGetUniformLocation(p, "uSize"), 32, 32);
	glClearColor(0, 0, 1, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	draw_quad();
	uint8_t pixels[32 * 32 * 4];
	glReadPixels(0, 0, 32, 32, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	int bad = 0, firstBad = -1;
	for (int i = 0; i < 32 * 32; i++) {
		if (pixels[i * 4] != 0 || pixels[i * 4 + 1] != 255
				|| pixels[i * 4 + 2] != 0) {
			if (firstBad < 0)
				firstBad = i;
			bad++;
		}
	}
	checkf(bad == 0, "15 vec4 varyings interpolate exactly (%d of 1024 pixels "
		"wrong%s)", bad, bad ? ", see below" : "");
	if (bad) {
		printf("  first wrong pixel %d: %u %u %u %u\n", firstBad,
			pixels[firstBad * 4], pixels[firstBad * 4 + 1],
			pixels[firstBad * 4 + 2], pixels[firstBad * 4 + 3]);
	}
	check(gl_error() == GL_NO_ERROR, "varyings: no GL error");
	glUseProgram(0);
	glDeleteProgram(p);
	delete_fbo(fbo, texture);
}

// -- texelfetch -----------------------------------------------------------

static void
case_texelfetch(void)
{
	const char* vs = "#version 300 es\n"
		"in vec2 aPosition;\n"
		"void main() { gl_Position = vec4(aPosition, 0.0, 1.0); }\n";
	const char* fs = "#version 300 es\n"
		"precision mediump float;\n"
		"uniform sampler2D uSampler;\n"
		"uniform ivec2 uTestPos;\n"
		"out vec4 color;\n"
		"void main() { color = texelFetch(uSampler, uTestPos, 0); }\n";
	GLuint p = program(vs, fs);
	if (!check(p != 0, "texelFetch program links"))
		return;
	enum { kSize = 24 };
	uint8_t green[kSize * kSize * 4];
	for (int i = 0; i < kSize * kSize; i++) {
		green[i * 4] = 0;
		green[i * 4 + 1] = 255;
		green[i * 4 + 2] = 0;
		green[i * 4 + 3] = 255;
	}
	// the frame buffer's texture first: making it binds it to unit 0
	GLuint target, fbo = rgba8_fbo(8, 8, NULL, &target);
	GLuint tex;
	glGenTextures(1, &tex);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kSize, kSize, 0, GL_RGBA,
		GL_UNSIGNED_BYTE, green);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

	glViewport(0, 0, 8, 8);
	glUseProgram(p);
	glUniform1i(glGetUniformLocation(p, "uSampler"), 0);
	static const struct { int x, y; int inside; } kAt[] = {
		{ 0, 0, 1 }, { kSize - 1, kSize - 1, 1 }, { kSize, 0, 0 },
		{ 0, kSize, 0 }, { -1, 0, 0 }, { 0, -1, 0 }, { -1, 1, 0 },
		{ 1000, 3, 0 },
	};
	for (unsigned i = 0; i < sizeof(kAt) / sizeof(kAt[0]); i++) {
		glUniform2i(glGetUniformLocation(p, "uTestPos"), kAt[i].x, kAt[i].y);
		glClearColor(0.5f, 0.5f, 0.5f, 0.5f);
		glClear(GL_COLOR_BUFFER_BIT);
		draw_quad();
		uint8_t px[4];
		glReadPixels(4, 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
		const uint8_t want[4] = { 0, kAt[i].inside ? 255 : 0, 0,
			kAt[i].inside ? 255 : 0 };
		checkf(memcmp(px, want, 4) == 0, "texelFetch at (%d, %d) = %u %u %u "
			"%u, want %u %u %u %u", kAt[i].x, kAt[i].y, px[0], px[1], px[2],
			px[3], want[0], want[1], want[2], want[3]);
	}
	check(gl_error() == GL_NO_ERROR, "texelfetch: no GL error");
	glUseProgram(0);
	glDeleteProgram(p);
	glDeleteTextures(1, &tex);
	delete_fbo(fbo, target);
}

// -- helpers --------------------------------------------------------------

// Helper invocations (the other pixels of a 2x2 quad, run for derivatives)
// must not write memory. One pixel wide quads whose fragment shader writes
// an r32ui image: only their columns may be written. (Mesa's glReadPixels
// into a pixel pack buffer is such a shader.)
static void
case_helpers(void)
{
	const char* vs = "#version 310 es\n"
		"in vec2 aPosition;\n"
		"void main() { gl_Position = vec4(aPosition, 0.0, 1.0); }\n";
	const char* fs = "#version 310 es\n"
		"precision highp float;\n"
		"layout(r32ui, binding = 0) uniform writeonly highp uimage2D uHits;\n"
		"uniform sampler2D uTex;\n"
		"out vec4 color;\n"
		"void main() {\n"
		"  ivec2 p = ivec2(gl_FragCoord.xy);\n"
		"  // a derivative, so that the quad's other pixels run as helpers\n"
		"  vec4 t = texture(uTex, gl_FragCoord.xy / 8.0);\n"
		"  imageStore(uHits, p, uvec4(1u + (gl_HelperInvocation ? 100u : 0u)));\n"
		"  color = t;\n"
		"}\n";
	GLint images = 0;
	glGetIntegerv(GL_MAX_FRAGMENT_IMAGE_UNIFORMS, &images);
	if (images == 0) {
		// (zink offers none where the device has no fragment stores)
		printf("  helpers: no images in fragment shaders, skipped\n");
		return;
	}
	GLuint p = program(vs, fs);
	if (!check(p != 0, "helper program links"))
		return;
	GLuint target, fbo = rgba8_fbo(8, 8, NULL, &target);
	GLuint tex;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	uint8_t pixels[8 * 8 * 4] = { 0 };
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 8, 8, 0, GL_RGBA,
		GL_UNSIGNED_BYTE, pixels);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	GLuint hitsTex;
	glGenTextures(1, &hitsTex);
	glBindTexture(GL_TEXTURE_2D, hitsTex);
	glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32UI, 8, 8);
	uint32_t zero[64] = { 0 };
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 8, 8, GL_RED_INTEGER,
		GL_UNSIGNED_INT, zero);
	glBindImageTexture(0, hitsTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32UI);
	glBindTexture(GL_TEXTURE_2D, tex);
	glUseProgram(p);
	glUniform1i(glGetUniformLocation(p, "uTex"), 0);
	// columns 0 and 3, rows 0..2: helpers would land in columns 1 and 2
	// and row 3
	glViewport(0, 0, 1, 3);
	draw_quad();
	glViewport(3, 0, 1, 3);
	draw_quad();
	glMemoryBarrier(GL_FRAMEBUFFER_BARRIER_BIT
		| GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
	// read the image back through a frame buffer of its own
	GLuint readFbo;
	glGenFramebuffers(1, &readFbo);
	glBindFramebuffer(GL_FRAMEBUFFER, readFbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		GL_TEXTURE_2D, hitsTex, 0);
	uint32_t hits4[64 * 4];
	glReadPixels(0, 0, 8, 8, GL_RGBA_INTEGER, GL_UNSIGNED_INT, hits4);
	int wrong = 0;
	for (int y = 0; y < 8; y++) {
		for (int x = 0; x < 8; x++) {
			uint32_t want = (x == 0 || x == 3) && y < 3 ? 1 : 0;
			if (hits4[(y * 8 + x) * 4] != want)
				wrong++;
		}
	}
	checkf(wrong == 0, "only covered pixels write memory (%d of 64 entries "
		"wrong)", wrong);
	if (wrong || sVerbose) {
		for (int y = 3; y >= 0; y--) {
			printf("  row %d:", y);
			for (int x = 0; x < 8; x++)
				printf(" %3u", hits4[(y * 8 + x) * 4]);
			printf("\n");
		}
	}
	check(gl_error() == GL_NO_ERROR, "helpers: no GL error");
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glDeleteFramebuffers(1, &readFbo);
	glUseProgram(0);
	glDeleteProgram(p);
	glDeleteTextures(1, &hitsTex);
	glDeleteTextures(1, &tex);
	delete_fbo(fbo, target);
}

// -- cf ---------------------------------------------------------------------

// Control flow in a compute shader: 8x8 invocations, only those with x < 3
// and y < 2 may store. Each variant nests the guarded store differently
// (Mesa's compute download for glReadPixels into a buffer is variant 4).
static void
case_cf(void)
{
	static const char* kBodies[] = {
		// 0: unsigned comparisons
		"if (uint(p.x) < uint(uW) && uint(p.y) < uint(uH)) hit(p);",
		// 1: both coordinates
		"if (p.x < uW && p.y < uH) hit(p);",
		// 2: all() of a vector comparison
		"if (all(lessThan(p, ivec2(uW, uH)))) hit(p);",
		// 3: a uniform if inside the divergent one
		"if (all(lessThan(p, ivec2(uW, uH)))) { if (uOne == 1) hit(p); }",
		// 4: uniform if/else chains inside, as in Mesa's download shader
		"if (all(lessThan(p, ivec2(uW, uH)))) {\n"
		"  if (uOne < 32) { if (uOne == 16) miss(); else hit(p); }\n"
		"  else miss();\n"
		"}",
		// 5: a divergent if inside a divergent if
		"if (p.x < uW) { if (p.y < uH) hit(p); }",
		// 6: a uniform loop inside
		"if (all(lessThan(p, ivec2(uW, uH)))) { for (int i = 0; i < uOne; i++) hit(p); }",
		// 7: early return
		"if (any(greaterThanEqual(p, ivec2(uW, uH)))) return; hit(p);",
	};
	const int n = sizeof(kBodies) / sizeof(kBodies[0]);
	for (int v = 0; v < n; v++) {
		char cs[2048];
		snprintf(cs, sizeof(cs), "#version 310 es\n"
			"layout(local_size_x = 8, local_size_y = 8) in;\n"
			"layout(std430, binding = 0) buffer Hits { uint hits[64]; };\n"
			"uniform int uW, uH, uOne;\n"
			"void hit(ivec2 p) { hits[p.y * 8 + p.x] = 1u; }\n"
			"void miss() { hits[63] = 99u; }\n"
			"void main() {\n"
			"  ivec2 p = ivec2(gl_GlobalInvocationID.xy);\n"
			"  %s\n"
			"}\n", kBodies[v]);
		GLuint shader = compile(GL_COMPUTE_SHADER, cs);
		GLuint p = glCreateProgram();
		glAttachShader(p, shader);
		glLinkProgram(p);
		GLint ok = 0;
		glGetProgramiv(p, GL_LINK_STATUS, &ok);
		if (!checkf(ok, "cf %d: compute program links", v))
			continue;
		GLuint ssbo;
		glGenBuffers(1, &ssbo);
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo);
		uint32_t zero[64] = { 0 };
		glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(zero), zero,
			GL_DYNAMIC_READ);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ssbo);
		glUseProgram(p);
		glUniform1i(glGetUniformLocation(p, "uW"), 3);
		glUniform1i(glGetUniformLocation(p, "uH"), 2);
		glUniform1i(glGetUniformLocation(p, "uOne"), 1);
		glDispatchCompute(1, 1, 1);
		glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
		uint32_t hits[64];
		const uint32_t* mapped = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0,
			sizeof(hits), GL_MAP_READ_BIT);
		if (mapped != NULL) {
			memcpy(hits, mapped, sizeof(hits));
			glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
		} else
			memset(hits, 0xff, sizeof(hits));
		int wrong = 0;
		for (int i = 0; i < 64; i++) {
			uint32_t want = (i % 8) < 3 && (i / 8) < 2 ? 1 : 0;
			if (hits[i] != want)
				wrong++;
		}
		checkf(wrong == 0, "cf %d: only x < 3, y < 2 store (%d of 64 wrong): "
			"%s", v, wrong, kBodies[v]);
		if (wrong || sVerbose) {
			for (int y = 3; y >= 0; y--) {
				printf("  row %d:", y);
				for (int x = 0; x < 8; x++)
					printf(" %2u", hits[y * 8 + x]);
				printf("\n");
			}
		}
		glUseProgram(0);
		glDeleteProgram(p);
		glDeleteShader(shader);
		glDeleteBuffers(1, &ssbo);
	}
	check(gl_error() == GL_NO_ERROR, "cf: no GL error");
}

// -- pack -----------------------------------------------------------------

static const uint8_t kRowColors[4][4] = {
	{ 249, 102, 0, 255 }, { 2, 200, 102, 255 }, { 134, 87, 234, 255 },
	{ 99, 5, 76, 255 },
};

static int
pack_iteration(int x, int y, int width, int height, int alignment,
	int rowLength, int skipPixels, int skipRows, int usePbo)
{
	const int bpp = 4;
	int actualWidth = rowLength > 0 ? rowLength : width;
	int rowBytes = actualWidth * bpp;
	int stride = (rowBytes + alignment - 1) / alignment * alignment;
	int size = stride * (height - 1) + width * bpp;
	int skip = skipPixels * bpp + skipRows * stride;
	int total = skip + size;
	uint8_t* buffer = malloc(total);
	uint8_t* want = malloc(total);
	for (int i = 0; i < total; i++)
		buffer[i] = want[i] = (uint8_t)(1 + i % 4);
	for (int row = 0; row < height; row++) {
		for (int col = 0; col < width; col++) {
			int fx = x + col, fy = y + row;
			if (fx < 0 || fx >= 4 || fy < 0 || fy >= 4)
				continue;
			memcpy(want + skip + row * stride + col * bpp, kRowColors[fy], 4);
		}
	}

	glPixelStorei(GL_PACK_ALIGNMENT, alignment);
	glPixelStorei(GL_PACK_ROW_LENGTH, rowLength);
	glPixelStorei(GL_PACK_SKIP_PIXELS, skipPixels);
	glPixelStorei(GL_PACK_SKIP_ROWS, skipRows);
	GLuint pbo = 0;
	if (usePbo) {
		glGenBuffers(1, &pbo);
		glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
		glBufferData(GL_PIXEL_PACK_BUFFER, total, buffer, GL_STATIC_READ);
		glReadPixels(x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, 0);
		void* mapped = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, total,
			GL_MAP_READ_BIT);
		if (mapped != NULL)
			memcpy(buffer, mapped, total);
		glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
		glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
		glDeleteBuffers(1, &pbo);
	} else
		glReadPixels(x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, buffer);
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	glPixelStorei(GL_PACK_ROW_LENGTH, 0);
	glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
	glPixelStorei(GL_PACK_SKIP_ROWS, 0);

	GLenum error = gl_error();
	int firstBad = -1;
	for (int i = 0; i < total; i++) {
		if (buffer[i] != want[i]) {
			firstBad = i;
			break;
		}
	}
	int ok = checkf(error == GL_NO_ERROR && firstBad < 0, "readPixels %s "
		"(%d,%d %dx%d) alignment %d row length %d skip %d px %d rows: %s",
		usePbo ? "into a buffer" : "into memory", x, y, width, height,
		alignment, rowLength, skipPixels, skipRows,
		error != GL_NO_ERROR ? "GL error" : firstBad < 0 ? "exact"
			: "differs");
	if (firstBad >= 0) {
		printf("  first difference at byte %d of %d (stride %d, skip %d): "
			"got", firstBad, total, stride, skip);
		for (int i = 0; i < total && i < 48; i++)
			printf(" %u", buffer[i]);
		printf("\n  want");
		for (int i = 0; i < total && i < 48; i++)
			printf(" %u", want[i]);
		printf("\n");
	}
	free(buffer);
	free(want);
	return ok;
}

static void
case_pack(void)
{
	uint8_t pixels[4 * 4 * 4];
	for (int y = 0; y < 4; y++) {
		for (int x = 0; x < 4; x++)
			memcpy(pixels + (y * 4 + x) * 4, kRowColors[y], 4);
	}
	GLuint texture, fbo = rgba8_fbo(4, 4, pixels, &texture);
	for (int pbo = 0; pbo <= 1; pbo++) {
		static const int kAlignments[] = { 1, 2, 4, 8 };
		for (int w = 1; w <= 3; w++) {
			for (int a = 0; a < 4; a++)
				pack_iteration(0, 0, w, 3, kAlignments[a], 0, 0, 0, pbo);
		}
		pack_iteration(0, 0, 3, 3, 8, 3, 0, 0, pbo);
		pack_iteration(-1, -1, 3, 3, 8, 3, 0, 0, pbo);
		pack_iteration(0, 0, 2, 3, 8, 4, 0, 0, pbo);
		pack_iteration(0, 0, 2, 3, 4, 4, 1, 0, pbo);
		pack_iteration(0, 0, 2, 3, 4, 4, 0, 1, pbo);
		pack_iteration(1, 1, 2, 2, 8, 5, 2, 1, pbo);
		pack_iteration(0, 0, 4, 4, 4, 0, 0, 0, pbo);
	}
	delete_fbo(fbo, texture);
}

// -- fboread --------------------------------------------------------------

enum { NORM, UINT, SINT };

static const struct fbo_case {
	const char* name;
	GLenum internalFormat, format, type, readFormat, readType;
	int kind, srcChannels, tolerance, srgb;
	double clear[4];
} kFboCases[] = {
#define C(f, fmt, t, rf, rt, k, ch, tol, ...) \
	{ #f, GL_##f, GL_##fmt, GL_##t, GL_##rf, GL_##rt, k, ch, tol, 0, \
		{ __VA_ARGS__ } }
	C(R8, RED, UNSIGNED_BYTE, RGBA, UNSIGNED_BYTE, NORM, 1, 6, 0.5, 0, 0, 0),
	C(R8UI, RED_INTEGER, UNSIGNED_BYTE, RGBA_INTEGER, UNSIGNED_INT, UINT, 1, 0,
		250, 0, 0, 0),
	C(R8I, RED_INTEGER, BYTE, RGBA_INTEGER, INT, SINT, 1, 0, -126, 0, 0, 0),
	C(R16UI, RED_INTEGER, UNSIGNED_SHORT, RGBA_INTEGER, UNSIGNED_INT, UINT, 1,
		0, 30001, 0, 0, 0),
	C(R16I, RED_INTEGER, SHORT, RGBA_INTEGER, INT, SINT, 1, 0, -14189, 0, 0, 0),
	C(R32UI, RED_INTEGER, UNSIGNED_INT, RGBA_INTEGER, UNSIGNED_INT, UINT, 1, 0,
		126726, 0, 0, 0),
	C(R32I, RED_INTEGER, INT, RGBA_INTEGER, INT, SINT, 1, 0, -126726, 0, 0, 0),
	C(RG8, RG, UNSIGNED_BYTE, RGBA, UNSIGNED_BYTE, NORM, 2, 6, 0.5, 0.7, 0, 0),
	C(RG8UI, RG_INTEGER, UNSIGNED_BYTE, RGBA_INTEGER, UNSIGNED_INT, UINT, 2, 0,
		250, 124, 0, 0),
	C(RG8I, RG_INTEGER, BYTE, RGBA_INTEGER, INT, SINT, 2, 0, -55, 124, 0, 0),
	C(RG16UI, RG_INTEGER, UNSIGNED_SHORT, RGBA_INTEGER, UNSIGNED_INT, UINT, 2,
		0, 30001, 18, 0, 0),
	C(RG16I, RG_INTEGER, SHORT, RGBA_INTEGER, INT, SINT, 2, 0, -14189, 6735, 0,
		0),
	C(RG32UI, RG_INTEGER, UNSIGNED_INT, RGBA_INTEGER, UNSIGNED_INT, UINT, 2, 0,
		126726, 1976, 0, 0),
	C(RG32I, RG_INTEGER, INT, RGBA_INTEGER, INT, SINT, 2, 0, -126726, 126726,
		0, 0),
	C(RGB8, RGB, UNSIGNED_BYTE, RGBA, UNSIGNED_BYTE, NORM, 3, 6, 0.5, 1, 0, 0),
	C(RGB565, RGB, UNSIGNED_BYTE, RGBA, UNSIGNED_BYTE, NORM, 3, 7, 0.5, 0.7,
		0.2, 0),
	C(RGBA8, RGBA, UNSIGNED_BYTE, RGBA, UNSIGNED_BYTE, NORM, 4, 6, 0.5, 0, 1,
		0.7),
	{ "SRGB8_ALPHA8", GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE, GL_RGBA,
		GL_UNSIGNED_BYTE, NORM, 4, 6, 1, { 0.5, 0, 1, 0.7 } },
	C(RGB5_A1, RGBA, UNSIGNED_BYTE, RGBA, UNSIGNED_BYTE, NORM, 4, 7, 0.5, 0,
		0.7, 1),
	C(RGBA4, RGBA, UNSIGNED_BYTE, RGBA, UNSIGNED_BYTE, NORM, 4, 10, 0.5, 0.7,
		1, 0),
	C(RGBA8UI, RGBA_INTEGER, UNSIGNED_BYTE, RGBA_INTEGER, UNSIGNED_INT, UINT, 4,
		0, 127, 0, 255, 178),
	C(RGBA8I, RGBA_INTEGER, BYTE, RGBA_INTEGER, INT, SINT, 4, 0, -55, 56, 80,
		127),
	C(RGB10_A2UI, RGBA_INTEGER, UNSIGNED_INT_2_10_10_10_REV, RGBA_INTEGER,
		UNSIGNED_INT, UINT, 4, 0, 178, 0, 127, 3),
	C(RGBA16UI, RGBA_INTEGER, UNSIGNED_SHORT, RGBA_INTEGER, UNSIGNED_INT, UINT,
		4, 0, 14189, 6735, 0, 19),
	C(RGBA16I, RGBA_INTEGER, SHORT, RGBA_INTEGER, INT, SINT, 4, 0, 14189,
		-6735, 0, 19),
	C(RGBA32UI, RGBA_INTEGER, UNSIGNED_INT, RGBA_INTEGER, UNSIGNED_INT, UINT, 4,
		0, 126726, 6726, 98765, 2015),
	C(RGBA32I, RGBA_INTEGER, INT, RGBA_INTEGER, INT, SINT, 4, 0, 126726, -6726,
		-98765, 2015),
	C(RGB10_A2, RGBA, UNSIGNED_INT_2_10_10_10_REV, RGBA, UNSIGNED_BYTE, NORM, 4,
		6, 0.7, 0, 0.5, 1),
#undef C
};

static double
to_srgb(double v)
{
	if (v <= 0)
		return 0;
	if (v < 0.0031308)
		return 12.92 * v;
	if (v < 1)
		return 1.055 * pow(v, 0.41666) - 0.055;
	return 1;
}

static int
read_channels(GLenum format)
{
	switch (format) {
		case GL_RED: case GL_RED_INTEGER: return 1;
		case GL_RG: case GL_RG_INTEGER: return 2;
		case GL_RGB: case GL_RGB_INTEGER: return 3;
		default: return 4;
	}
}

// reads 2x2 pixels in readFormat/readType and compares the first pixel
static void
fbo_read(const struct fbo_case* c, GLenum readFormat, GLenum readType,
	const char* how)
{
	// 2x2 pixels of up to four 32-bit components
	union {
		uint32_t u32[16];
		int32_t i32[16];
		uint16_t u16[32];
		int16_t i16[32];
		uint8_t u8[64];
		int8_t i8[64];
	} raw;
	memset(&raw, 0xa5, sizeof(raw));
	glReadPixels(0, 0, 2, 2, readFormat, readType, &raw);
	GLenum error = gl_error();
	if (!checkf(error == GL_NO_ERROR, "%s: readPixels %s (0x%x/0x%x): GL "
			"error 0x%x", c->name, how, readFormat, readType, error))
		return;

	double got[4] = { 0 }, want[4];
	int tolerance = c->tolerance;
	int channels = read_channels(readFormat);
	for (int i = 0; i < 4; i++) {
		want[i] = c->clear[i];
		if (c->srgb && i < 3)
			want[i] = to_srgb(want[i]);
	}
	// a normalized source is compared in the read type's scale (per channel)
	double scale[4] = { 1, 1, 1, 1 };
	uint16_t v16 = raw.u16[0];
	switch (readType) {
		case GL_UNSIGNED_BYTE:
			for (int i = 0; i < channels; i++)
				got[i] = raw.u8[i];
			scale[0] = scale[1] = scale[2] = scale[3] = 255;
			break;
		case GL_BYTE:
			for (int i = 0; i < channels; i++)
				got[i] = raw.i8[i];
			break;
		case GL_UNSIGNED_SHORT:
			for (int i = 0; i < channels; i++)
				got[i] = raw.u16[i];
			scale[0] = scale[1] = scale[2] = scale[3] = 65535;
			break;
		case GL_SHORT:
			for (int i = 0; i < channels; i++)
				got[i] = raw.i16[i];
			break;
		case GL_UNSIGNED_INT:
			for (int i = 0; i < channels; i++)
				got[i] = raw.u32[i];
			break;
		case GL_INT:
			for (int i = 0; i < channels; i++)
				got[i] = raw.i32[i];
			break;
		case GL_UNSIGNED_SHORT_5_6_5:
			got[0] = v16 >> 11;
			got[1] = (v16 >> 5) & 0x3f;
			got[2] = v16 & 0x1f;
			channels = 3;
			scale[0] = scale[2] = 31;
			scale[1] = 63;
			break;
		case GL_UNSIGNED_SHORT_5_5_5_1:
			got[0] = v16 >> 11;
			got[1] = (v16 >> 6) & 0x1f;
			got[2] = (v16 >> 1) & 0x1f;
			got[3] = v16 & 1;
			scale[0] = scale[1] = scale[2] = 31;
			break;
		case GL_UNSIGNED_SHORT_4_4_4_4:
			got[0] = v16 >> 12;
			got[1] = (v16 >> 8) & 0xf;
			got[2] = (v16 >> 4) & 0xf;
			got[3] = v16 & 0xf;
			scale[0] = scale[1] = scale[2] = scale[3] = 15;
			break;
		case GL_UNSIGNED_INT_2_10_10_10_REV:
			got[0] = raw.u32[0] & 0x3ff;
			got[1] = (raw.u32[0] >> 10) & 0x3ff;
			got[2] = (raw.u32[0] >> 20) & 0x3ff;
			got[3] = raw.u32[0] >> 30;
			scale[0] = scale[1] = scale[2] = 1023;
			scale[3] = 3;
			if (c->kind == NORM)
				tolerance = 25;
			break;
		default:
			printf("  %s: read type 0x%x not compared\n", c->name, readType);
			return;
	}
	if (c->kind == NORM) {
		for (int i = 0; i < 4; i++)
			want[i] *= scale[i];
	}
	int n = c->srcChannels < channels ? c->srcChannels : channels;
	int ok = 1;
	for (int i = 0; i < n; i++) {
		if (fabs(got[i] - want[i]) > tolerance)
			ok = 0;
	}
	checkf(ok, "%s: readPixels %s = %g %g %g %g, want %g %g %g %g", c->name,
		how, got[0], got[1], got[2], got[3], want[0], want[1], want[2],
		want[3]);
}

static void
case_fboread(void)
{
	for (unsigned i = 0; i < sizeof(kFboCases) / sizeof(kFboCases[0]); i++) {
		const struct fbo_case* c = &kFboCases[i];
		GLuint fbo, texture;
		glGenFramebuffers(1, &fbo);
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);
		glGenTextures(1, &texture);
		glBindTexture(GL_TEXTURE_2D, texture);
		glTexImage2D(GL_TEXTURE_2D, 0, c->internalFormat, 2, 2, 0, c->format,
			c->type, NULL);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			GL_TEXTURE_2D, texture, 0);
		GLenum error = gl_error();
		checkf(error == GL_NO_ERROR, "%s: texture + frame buffer: GL error "
			"0x%x", c->name, error);
		GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		if (status != GL_FRAMEBUFFER_COMPLETE) {
			printf("  %s: frame buffer status 0x%x, skipped\n", c->name, status);
			delete_fbo(fbo, texture);
			continue;
		}
		if (c->kind == UINT) {
			GLuint v[4];
			for (int j = 0; j < 4; j++)
				v[j] = (GLuint)c->clear[j];
			glClearBufferuiv(GL_COLOR, 0, v);
		} else if (c->kind == SINT) {
			GLint v[4];
			for (int j = 0; j < 4; j++)
				v[j] = (GLint)c->clear[j];
			glClearBufferiv(GL_COLOR, 0, v);
		} else {
			glClearColor(c->clear[0], c->clear[1], c->clear[2], c->clear[3]);
			glClear(GL_COLOR_BUFFER_BIT);
		}
		error = gl_error();
		checkf(error == GL_NO_ERROR, "%s: clear: GL error 0x%x", c->name,
			error);

		GLint implFormat = 0, implType = 0;
		glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT, &implFormat);
		glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE, &implType);
		fbo_read(c, c->readFormat, c->readType, "as the test reads");
		if (c->internalFormat == GL_RGB10_A2) {
			fbo_read(c, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV,
				"as 2_10_10_10_REV");
		}
		char how[64];
		snprintf(how, sizeof(how), "in the implementation format 0x%x/0x%x",
			implFormat, implType);
		fbo_read(c, implFormat, implType, how);
		delete_fbo(fbo, texture);
		if (sGetGraphicsResetStatus != NULL
				&& sGetGraphicsResetStatus() != GL_NO_ERROR) {
			check(0, "context lost");
			return;
		}
	}
}

// -- blit -----------------------------------------------------------------

// rendering/blitframebuffer-filter-outofbounds: 8x8 read and draw buffers,
// the read one all (0x20, 0x20, 0x20, 0xff) where it exists
static void
case_blit(void)
{
	static const struct {
		int args[8];
		double changed[4];
	} kBlits[] = {
		{ { -2, -2, 4, 4, 1, 1, 4, 4 }, { 2, 2, 4, 4 } },
		{ { -2, -2, 4, 4, 1, 1, 7, 7 }, { 3, 3, 7, 7 } },
		{ { 0, 0, 6, 6, 7, 7, 10, 10 }, { 7, 7, 8, 8 } },
		{ { 0, 0, 6, 6, 4, 4, 10, 10 }, { 4, 4, 8, 8 } },
		{ { -2, -2, 4, 4, 7, 7, 10, 10 }, { 8, 8, 8, 8 } },
		{ { -2, -2, 4, 4, 4, 4, 10, 10 }, { 6, 6, 8, 8 } },
		{ { -2, -2, 4, 4, 2, 2, 10, 10 }, { 2 + 2.0 / 6 * 8, 2 + 2.0 / 6 * 8,
			8, 8 } },
		{ { -2, -2, 4, 4, 3, 3, 10, 10 }, { 5, 5, 8, 8 } },
		{ { -2, -2, 4, 4, 10, 10, 2, 2 }, { 2, 2, 7, 7 } },
		{ { -2, -2, 4, 4, 10, 10, 3, 3 }, { 3, 3, 8, 8 } },
	};
	uint8_t grey[8 * 8 * 4];
	for (int i = 0; i < 8 * 8; i++) {
		grey[i * 4] = grey[i * 4 + 1] = grey[i * 4 + 2] = 0x20;
		grey[i * 4 + 3] = 0xff;
	}
	GLuint readTexture, readFbo = rgba8_fbo(8, 8, grey, &readTexture);
	GLuint drawTexture, drawFbo = rgba8_fbo(8, 8, NULL, &drawTexture);
	for (int f = 0; f < 2; f++) {
		GLenum filter = f == 0 ? GL_LINEAR : GL_NEAREST;
		for (unsigned b = 0; b < sizeof(kBlits) / sizeof(kBlits[0]); b++) {
			const int* a = kBlits[b].args;
			const double* changed = kBlits[b].changed;
			glBindFramebuffer(GL_FRAMEBUFFER, drawFbo);
			glClearColor(0, 0, 0, 0);
			glClear(GL_COLOR_BUFFER_BIT);
			glBindFramebuffer(GL_READ_FRAMEBUFFER, readFbo);
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFbo);
			glBlitFramebuffer(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7],
				GL_COLOR_BUFFER_BIT, filter);
			glBindFramebuffer(GL_FRAMEBUFFER, drawFbo);
			uint8_t px[8 * 8 * 4];
			glReadPixels(0, 0, 8, 8, GL_RGBA, GL_UNSIGNED_BYTE, px);
			GLenum error = gl_error();
			int bad = 0;
			for (int y = 0; y < 8; y++) {
				for (int x = 0; x < 8; x++) {
					int in = x >= changed[0] && x < changed[2]
						&& y >= changed[1] && y < changed[3];
					const uint8_t* p = px + (y * 8 + x) * 4;
					int want = in ? 0x20 : 0, wantA = in ? 0xff : 0;
					if (abs(p[0] - want) > 3 || abs(p[3] - wantA) > 3)
						bad++;
				}
			}
			checkf(error == GL_NO_ERROR && bad == 0, "blit %s %d,%d %d,%d -> "
				"%d,%d %d,%d: %d pixels wrong%s", f == 0 ? "linear" : "nearest",
				a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], bad,
				error != GL_NO_ERROR ? ", GL error" : "");
		}
	}
	delete_fbo(readFbo, readTexture);
	delete_fbo(drawFbo, drawTexture);
}

// -- bench (only with --case bench) -----------------------------------------

static double
now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

// GPU-bound passes at 1280x720, to compare core clocks: a fragment shader
// of 64 dependent ALU iterations (shader core), and 16 blended full screen
// quads (fill), each frame timed to glFinish().
static void
case_bench(void)
{
	const char* vs = "#version 300 es\n"
		"in vec2 aPosition;\n"
		"void main() { gl_Position = vec4(aPosition, 0.0, 1.0); }\n";
	const char* alu = "#version 300 es\n"
		"precision highp float;\n"
		"out vec4 color;\n"
		"void main() {\n"
		"  vec2 p = gl_FragCoord.xy * 0.001;\n"
		"  float x = p.x, y = p.y;\n"
		"  for (int i = 0; i < 64; i++) {\n"
		"    x = fract(x * 1.37 + y * 0.71 + 0.13);\n"
		"    y = fract(y * 1.11 + x * 0.53 + 0.29);\n"
		"  }\n"
		"  color = vec4(x, y, 0.5, 1.0);\n"
		"}\n";
	const char* flat = "#version 300 es\n"
		"precision mediump float;\n"
		"out vec4 color;\n"
		"void main() { color = vec4(0.1, 0.2, 0.3, 0.1); }\n";
	GLuint pAlu = program(vs, alu), pFlat = program(vs, flat);
	if (!check(pAlu != 0 && pFlat != 0, "bench programs link"))
		return;
	GLuint target, fbo = rgba8_fbo(1280, 720, NULL, &target);
	glViewport(0, 0, 1280, 720);
	for (int pass = 0; pass < 2; pass++) {
		glUseProgram(pass == 0 ? pAlu : pFlat);
		if (pass == 1) {
			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		}
		double best = 1e9, total = 0;
		const int frames = 30;
		for (int f = 0; f < frames + 2; f++) {
			double start = now_ms();
			for (int q = 0; q < (pass == 0 ? 1 : 16); q++)
				draw_quad();
			glFinish();
			double ms = now_ms() - start;
			if (f >= 2) {
				total += ms;
				if (ms < best)
					best = ms;
			}
		}
		glDisable(GL_BLEND);
		double mpix = 1280.0 * 720 * (pass == 0 ? 1 : 16) / (best * 1000);
		printf("bench %s: %.2f ms per frame (best %.2f), %.0f Mpixel/s\n",
			pass == 0 ? "alu 64 iterations" : "fill 16 blended quads",
			total / frames, best, mpix);
	}
	// glFinish() with nothing to do, and after one small draw (WebGL 2's
	// occlusion-query test calls gl.finish() 160000 times)
	glUseProgram(pFlat);
	glViewport(0, 0, 8, 8);
	for (int draw = 0; draw < 2; draw++) {
		glFinish();
		double start = now_ms();
		for (int i = 0; i < 200; i++) {
			if (draw)
				draw_quad();
			glFinish();
		}
		printf("bench glFinish%s: %.3f ms each\n",
			draw ? " after an 8x8 draw" : " with nothing to do",
			(now_ms() - start) / 200);
	}
	check(gl_error() == GL_NO_ERROR, "bench: no GL error");
	glUseProgram(0);
	glDeleteProgram(pAlu);
	glDeleteProgram(pFlat);
	delete_fbo(fbo, target);
}

// -------------------------------------------------------------------------

static const struct {
	const char* name;
	void (*run)(void);
} kCases[] = {
	{ "limits", case_limits },
	{ "varyings", case_varyings },
	{ "texelfetch", case_texelfetch },
	{ "helpers", case_helpers },
	{ "cf", case_cf },
	{ "pack", case_pack },
	{ "fboread", case_fboread },
	{ "blit", case_blit },
	// not part of a plain run
	{ "bench", case_bench },
};

static int
run(const char* only)
{
	EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	EGLint major = 0, minor = 0;
	if (!check(display != EGL_NO_DISPLAY
			&& eglInitialize(display, &major, &minor), "eglInitialize"))
		return 0;
	EGLint configAttributes[] = {
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
		EGL_ALPHA_SIZE, 8,
		EGL_NONE
	};
	EGLConfig config;
	EGLint count = 0;
	if (!check(eglChooseConfig(display, configAttributes, &config, 1, &count)
			&& count == 1, "eglChooseConfig (ES3)"))
		return 0;
	const EGLint surfaceAttributes[] = { EGL_WIDTH, 64, EGL_HEIGHT, 64,
		EGL_NONE };
	EGLSurface surface = eglCreatePbufferSurface(display, config,
		surfaceAttributes);
	eglBindAPI(EGL_OPENGL_ES_API);
	const EGLint contextAttributes[] = { EGL_CONTEXT_CLIENT_VERSION, 3,
		EGL_NONE };
	EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT,
		contextAttributes);
	if (!check(surface != EGL_NO_SURFACE && context != EGL_NO_CONTEXT
			&& eglMakeCurrent(display, surface, surface, context),
			"pbuffer, ES3 context"))
		return 0;
	printf("GL_RENDERER: %s\nGL_VERSION: %s\n", glGetString(GL_RENDERER),
		glGetString(GL_VERSION));
	sGetGraphicsResetStatus = (GLenum (*)(void))eglGetProcAddress(
		"glGetGraphicsResetStatusEXT");
	if (sGetGraphicsResetStatus == NULL) {
		sGetGraphicsResetStatus = (GLenum (*)(void))eglGetProcAddress(
			"glGetGraphicsResetStatusKHR");
	}

	GLuint vao;
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);
	for (unsigned i = 0; i < sizeof(kCases) / sizeof(kCases[0]); i++) {
		if (only != NULL ? strcmp(only, kCases[i].name) != 0
				: strcmp(kCases[i].name, "bench") == 0)
			continue;
		unsigned before = sFailures;
		kCases[i].run();
		glFinish();
		GLenum reset = sGetGraphicsResetStatus != NULL
			? sGetGraphicsResetStatus() : GL_NO_ERROR;
		check(reset == GL_NO_ERROR, "context not lost");
		printf("-- %s: %s\n", kCases[i].name,
			sFailures == before ? "pass" : "FAIL");
		if (reset != GL_NO_ERROR)
			break;
	}

	eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroyContext(display, context);
	eglDestroySurface(display, surface);
	eglTerminate(display);
	return 1;
}

int
main(int argc, char** argv)
{
	const char* only = NULL;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--case") == 0 && i + 1 < argc)
			only = argv[++i];
		else if (strcmp(argv[i], "--verbose") == 0)
			sVerbose = 1;
		else {
			fprintf(stderr, "usage: pvr_glconform [--case NAME] [--verbose]\n");
			return 2;
		}
	}
	run(only);
	printf("%s\n", sFailures == 0 ? "PASS" : "FAIL");
	return sFailures == 0 ? 0 : 1;
}
