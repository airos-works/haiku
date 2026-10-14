/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
// Transform feedback through OpenGL ES 3.0 (zink on the PowerVR Vulkan
// driver, where it is emulated: the vertex shader runs again as a compute
// kernel, mesa-haiku-pvr.patch). Offscreen, through the system EGL.
//
// Each case draws with transform feedback active and reads the captured
// buffers back through glMapBufferRange(): the vertex order of the
// decomposed primitives (strips, fans), indexed and instanced draws,
// separate and interleaved capture, buffer overflow, pause/resume, several
// draws into one buffer, rasterizer discard, and the
// GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN query.
//   pvr_glxfb [--case NAME] [--shim] [--repeat N] [--desktop]
// --shim skips the data checks (the host shim executes nothing) and only
// requires the GL calls to go through without an error; --case runs one case.
// OpenGL ES 3.0 allows no indexed draws while transform feedback is active,
// so the indexed cases run only with --desktop: an OpenGL 3.x context (zink
// offers 3.2 here), the same shaders as GLSL 1.40, and every case.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#define MAX_VERTS		64
#define FILL			-12345.0f

static int sShim;
static int sDesktop;
static unsigned sFailures;

static int
check(int ok, const char* what)
{
	printf("%s: %s\n", ok ? "ok" : "FAIL", what);
	if (!ok)
		sFailures++;
	return ok;
}

static void
check_gl_error(const char* where)
{
	GLenum error = glGetError();
	if (error != GL_NO_ERROR) {
		printf("FAIL: GL error 0x%x after %s\n", error, where);
		sFailures++;
	}
}

static GLuint
compile(GLenum type, const char* body)
{
	// the same shader for both APIs, behind the right version line
	const char* sources[2] = {
		sDesktop ? "#version 140\n" : "#version 300 es\nprecision mediump float;\n",
		body
	};
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 2, sources, NULL);
	glCompileShader(shader);
	GLint status = 0;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
	if (status == 0) {
		char log[2048];
		glGetShaderInfoLog(shader, sizeof(log), NULL, log);
		printf("shader compile: %s\n", log);
	}
	return shader;
}

// The vertex shader: three captured outputs of a position, a texture
// coordinate and an instanced attribute, plus gl_VertexID and
// gl_InstanceID, so that every slot of the output has a known value.
static const char* kVertexShader =
	"in vec4 a_pos;\n"
	"in vec2 a_uv;\n"
	"in float a_inst;\n"
	"uniform vec4 u_scale;\n"
	"out vec4 v_pos;\n"
	"out vec2 v_uv;\n"
	"out vec2 v_ids;\n"
	"void main() {\n"
	"  gl_Position = a_pos * u_scale;\n"
	"  v_pos = gl_Position;\n"
	"  v_uv = a_uv * 2.0 + vec2(a_inst, 0.0);\n"
	"  v_ids = vec2(float(gl_VertexID), float(gl_InstanceID));\n"
	"}\n";

static const char* kFragmentShader =
	"in vec4 v_pos;\n"
	"in vec2 v_uv;\n"
	"in vec2 v_ids;\n"
	"out vec4 color;\n"
	"void main() { color = v_pos + vec4(v_uv, v_ids); }\n";

static const char* kVaryings[] = { "v_pos", "v_uv", "v_ids" };
#define VARYING_COUNT	3
#define COMPONENTS		8	/* 4 + 2 + 2 */

static GLuint
link_program(GLenum bufferMode)
{
	GLuint program = glCreateProgram();
	glAttachShader(program, compile(GL_VERTEX_SHADER, kVertexShader));
	glAttachShader(program, compile(GL_FRAGMENT_SHADER, kFragmentShader));
	glTransformFeedbackVaryings(program, VARYING_COUNT, kVaryings, bufferMode);
	glLinkProgram(program);
	GLint status = 0;
	glGetProgramiv(program, GL_LINK_STATUS, &status);
	if (status == 0) {
		char log[2048];
		glGetProgramInfoLog(program, sizeof(log), NULL, log);
		printf("program link: %s\n", log);
	}
	return program;
}

// The vertex data: vertex i has position (i, 2i, 3i, 1), uv (i/8, i/4),
// and the instanced attribute 100 * instance.
static float sPositions[MAX_VERTS * 4];
static float sUVs[MAX_VERTS * 2];
static float sInstanced[MAX_VERTS];
static GLuint sVertexBuffer, sUVBuffer, sInstanceBuffer, sIndexBuffer;
static GLuint sProgramInterleaved, sProgramSeparate;
static const GLfloat kScale[4] = { 1.0f, 0.5f, 0.25f, 2.0f };

// What the shader writes for vertex `vertex` of instance `instance`.
static void
expected_record(unsigned vertex, unsigned instance, float out[COMPONENTS])
{
	out[0] = sPositions[vertex * 4 + 0] * kScale[0];
	out[1] = sPositions[vertex * 4 + 1] * kScale[1];
	out[2] = sPositions[vertex * 4 + 2] * kScale[2];
	out[3] = sPositions[vertex * 4 + 3] * kScale[3];
	out[4] = sUVs[vertex * 2 + 0] * 2.0f + sInstanced[instance];
	out[5] = sUVs[vertex * 2 + 1] * 2.0f;
	out[6] = (float)vertex;
	out[7] = (float)instance;
}

// The vertices of the decomposed primitives of a draw, in capture order:
// the n-th captured vertex comes from vertex `verts[n]` of the draw.
static unsigned
decompose(GLenum mode, unsigned count, unsigned* verts)
{
	unsigned n = 0;
	switch (mode) {
	case GL_POINTS:
	case GL_LINES:
	case GL_TRIANGLES:
		for (unsigned i = 0; i < count; i++)
			verts[n++] = i;
		if (mode == GL_LINES)
			n -= n % 2;
		if (mode == GL_TRIANGLES)
			n -= n % 3;
		break;
	case GL_LINE_STRIP:
		for (unsigned i = 0; i + 1 < count; i++) {
			verts[n++] = i;
			verts[n++] = i + 1;
		}
		break;
	case GL_TRIANGLE_STRIP:
		for (unsigned i = 0; i + 2 < count; i++) {
			if (i & 1) {
				verts[n++] = i + 1;
				verts[n++] = i;
			} else {
				verts[n++] = i;
				verts[n++] = i + 1;
			}
			verts[n++] = i + 2;
		}
		break;
	case GL_TRIANGLE_FAN:
		for (unsigned i = 0; i + 2 < count; i++) {
			verts[n++] = 0;
			verts[n++] = i + 1;
			verts[n++] = i + 2;
		}
		break;
	}
	return n;
}

static unsigned
verts_per_prim(GLenum mode)
{
	switch (mode) {
	case GL_POINTS:
		return 1;
	case GL_LINES:
	case GL_LINE_STRIP:
		return 2;
	default:
		return 3;
	}
}

struct capture {
	GLuint buffers[VARYING_COUNT];
	unsigned bufferCount;
	unsigned capacity;	// vertices
	float* data[VARYING_COUNT];
};

static const unsigned kComponentsPerVarying[VARYING_COUNT] = { 4, 2, 2 };

static void
capture_create(struct capture* c, int separate, unsigned capacity)
{
	memset(c, 0, sizeof(*c));
	c->bufferCount = separate ? VARYING_COUNT : 1;
	c->capacity = capacity;
	glGenBuffers(c->bufferCount, c->buffers);
	for (unsigned b = 0; b < c->bufferCount; b++) {
		unsigned components = separate ? kComponentsPerVarying[b] : COMPONENTS;
		unsigned floats = capacity * components;
		float* fill = malloc(floats * sizeof(float));
		for (unsigned i = 0; i < floats; i++)
			fill[i] = FILL;
		glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, c->buffers[b]);
		glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, floats * sizeof(float),
			fill, GL_DYNAMIC_READ);
		free(fill);
		glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, b, c->buffers[b]);
	}
}

static void
capture_read(struct capture* c, int separate)
{
	for (unsigned b = 0; b < c->bufferCount; b++) {
		unsigned components = separate ? kComponentsPerVarying[b] : COMPONENTS;
		unsigned bytes = c->capacity * components * sizeof(float);
		glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, c->buffers[b]);
		void* map = glMapBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER, 0, bytes,
			GL_MAP_READ_BIT);
		c->data[b] = malloc(bytes);
		if (map != NULL)
			memcpy(c->data[b], map, bytes);
		else
			printf("FAIL: glMapBufferRange of buffer %u\n", b);
		glUnmapBuffer(GL_TRANSFORM_FEEDBACK_BUFFER);
	}
}

static void
capture_destroy(struct capture* c)
{
	for (unsigned b = 0; b < c->bufferCount; b++)
		free(c->data[b]);
	glDeleteBuffers(c->bufferCount, c->buffers);
}

// The captured record n, as 8 floats.
static void
capture_record(const struct capture* c, int separate, unsigned n,
	float out[COMPONENTS])
{
	if (!separate) {
		memcpy(out, c->data[0] + n * COMPONENTS, COMPONENTS * sizeof(float));
		return;
	}
	unsigned k = 0;
	for (unsigned b = 0; b < VARYING_COUNT; b++) {
		for (unsigned i = 0; i < kComponentsPerVarying[b]; i++)
			out[k++] = c->data[b][n * kComponentsPerVarying[b] + i];
	}
}

// Checks records [0, n) against (vertex, instance) pairs and that the
// records after them (up to the capacity) are untouched.
static int
check_records(const struct capture* c, int separate, unsigned n,
	const unsigned* verts, const unsigned* instances, const char* what)
{
	unsigned wrong = 0;
	for (unsigned i = 0; i < c->capacity; i++) {
		float got[COMPONENTS], want[COMPONENTS];
		capture_record(c, separate, i, got);
		if (i < n)
			expected_record(verts[i], instances != NULL ? instances[i] : 0, want);
		else
			for (unsigned k = 0; k < COMPONENTS; k++)
				want[k] = FILL;
		if (memcmp(got, want, sizeof(got)) == 0)
			continue;
		if (wrong < 6) {
			printf("  record %u: got %g %g %g %g | %g %g | %g %g, expected "
				"%g %g %g %g | %g %g | %g %g\n", i, got[0], got[1], got[2],
				got[3], got[4], got[5], got[6], got[7], want[0], want[1],
				want[2], want[3], want[4], want[5], want[6], want[7]);
		}
		wrong++;
	}
	if (wrong != 0)
		printf("  %u of %u records wrong\n", wrong, c->capacity);
	char text[160];
	snprintf(text, sizeof(text), "%s: %u records", what, n);
	return check(sShim || wrong == 0, text);
}

static void
bind_attributes(GLuint program)
{
	GLint pos = glGetAttribLocation(program, "a_pos");
	GLint uv = glGetAttribLocation(program, "a_uv");
	GLint inst = glGetAttribLocation(program, "a_inst");
	glBindBuffer(GL_ARRAY_BUFFER, sVertexBuffer);
	glVertexAttribPointer(pos, 4, GL_FLOAT, GL_FALSE, 0, NULL);
	glEnableVertexAttribArray(pos);
	glBindBuffer(GL_ARRAY_BUFFER, sUVBuffer);
	glVertexAttribPointer(uv, 2, GL_FLOAT, GL_FALSE, 0, NULL);
	glEnableVertexAttribArray(uv);
	glBindBuffer(GL_ARRAY_BUFFER, sInstanceBuffer);
	glVertexAttribPointer(inst, 1, GL_FLOAT, GL_FALSE, 0, NULL);
	glVertexAttribDivisor(inst, 1);
	glEnableVertexAttribArray(inst);
	glUniform4fv(glGetUniformLocation(program, "u_scale"), 1, kScale);
}

static GLuint
begin_query(void)
{
	GLuint query;
	glGenQueries(1, &query);
	glBeginQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN, query);
	return query;
}

static void
end_query(GLuint query, unsigned expected, const char* what)
{
	glEndQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN);
	GLuint written = 0xffffffffu;
	glGetQueryObjectuiv(query, GL_QUERY_RESULT, &written);
	glDeleteQueries(1, &query);
	char text[160];
	snprintf(text, sizeof(text), "%s: primitives written %u (expected %u)",
		what, written, expected);
	check(sShim || written == expected, text);
}

// A draw with glDrawArrays(mode, 0, count): all of its primitives.
static void
case_draw_arrays(const char* name, GLenum mode, GLenum glmode, unsigned count,
	int separate)
{
	GLuint program = separate ? sProgramSeparate : sProgramInterleaved;
	unsigned verts[MAX_VERTS * 3];
	unsigned n = decompose(mode, count, verts);
	struct capture c;

	glUseProgram(program);
	bind_attributes(program);
	capture_create(&c, separate, n + 2);
	GLuint query = begin_query();
	glBeginTransformFeedback(glmode);
	glDrawArrays(mode, 0, count);
	glEndTransformFeedback();
	end_query(query, n / verts_per_prim(mode), name);
	check_gl_error(name);
	capture_read(&c, separate);
	check_records(&c, separate, n, verts, NULL, name);
	capture_destroy(&c);
}

// glDrawElements(GL_TRIANGLES) with a ushort index list.
static void
case_indexed(void)
{
	static const GLushort indices[] = { 5, 1, 7, 2, 2, 6, 0, 3, 4 };
	const unsigned count = sizeof(indices) / sizeof(indices[0]);
	unsigned verts[MAX_VERTS];
	struct capture c;

	for (unsigned i = 0; i < count; i++)
		verts[i] = indices[i];
	glUseProgram(sProgramInterleaved);
	bind_attributes(sProgramInterleaved);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, sIndexBuffer);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices,
		GL_STATIC_DRAW);
	capture_create(&c, 0, count + 2);
	GLuint query = begin_query();
	glBeginTransformFeedback(GL_TRIANGLES);
	glDrawElements(GL_TRIANGLES, count, GL_UNSIGNED_SHORT, NULL);
	glEndTransformFeedback();
	end_query(query, count / 3, "indexed triangles");
	check_gl_error("indexed");
	capture_read(&c, 0);
	check_records(&c, 0, count, verts, NULL, "indexed triangles");
	capture_destroy(&c);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

// glDrawElementsInstanced with a byte index list, a non-zero first index
// through the pointer, and three instances.
static void
case_instanced(void)
{
	static const GLubyte indices[] = { 9, 9, 3, 0, 6, 2, 5 };
	const unsigned first = 2, count = 5, instances = 3;
	unsigned verts[MAX_VERTS], inst[MAX_VERTS];
	struct capture c;

	for (unsigned i = 0; i < instances; i++) {
		for (unsigned v = 0; v < count; v++) {
			verts[i * count + v] = indices[first + v];
			inst[i * count + v] = i;
		}
	}
	glUseProgram(sProgramSeparate);
	bind_attributes(sProgramSeparate);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, sIndexBuffer);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices,
		GL_STATIC_DRAW);
	capture_create(&c, 1, count * instances + 2);
	GLuint query = begin_query();
	glBeginTransformFeedback(GL_POINTS);
	glDrawElementsInstanced(GL_POINTS, count, GL_UNSIGNED_BYTE,
		(const void*)(uintptr_t)first, instances);
	glEndTransformFeedback();
	end_query(query, count * instances, "instanced indexed points");
	check_gl_error("instanced");
	capture_read(&c, 1);
	check_records(&c, 1, count * instances, verts, inst,
		"instanced indexed points");
	capture_destroy(&c);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

// A buffer with room for 5 vertices, and 8 points drawn: 5 recorded, the
// query says 5, nothing past the end is touched.
static void
case_overflow(void)
{
	unsigned verts[MAX_VERTS];
	struct capture c;

	for (unsigned i = 0; i < 8; i++)
		verts[i] = i;
	glUseProgram(sProgramInterleaved);
	bind_attributes(sProgramInterleaved);
	capture_create(&c, 0, 5);
	// the buffer has room for exactly 5 records, and so does the binding
	glBindBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER, 0, c.buffers[0], 0,
		5 * COMPONENTS * sizeof(float));
	GLuint query = begin_query();
	glBeginTransformFeedback(GL_POINTS);
	glDrawArrays(GL_POINTS, 0, 8);
	glEndTransformFeedback();
	end_query(query, 5, "overflow");
	check_gl_error("overflow");
	capture_read(&c, 0);
	check_records(&c, 0, 5, verts, NULL, "overflow");
	capture_destroy(&c);
}

// Two draws into one buffer: the second continues after the first. Then a
// pause: a draw while paused is not recorded, one after the resume is.
static void
case_draws_and_pause(void)
{
	unsigned verts[MAX_VERTS];
	struct capture c;
	unsigned n = 0;

	for (unsigned i = 0; i < 3; i++)
		verts[n++] = i;		// first draw: 3 points from 0
	for (unsigned i = 0; i < 4; i++)
		verts[n++] = 10 + i;	// second: 4 points from 10
	for (unsigned i = 0; i < 2; i++)
		verts[n++] = 20 + i;	// after the resume: 2 from 20
	glUseProgram(sProgramInterleaved);
	bind_attributes(sProgramInterleaved);
	capture_create(&c, 0, n + 3);
	GLuint query = begin_query();
	glBeginTransformFeedback(GL_POINTS);
	glDrawArrays(GL_POINTS, 0, 3);
	glDrawArrays(GL_POINTS, 10, 4);
	glPauseTransformFeedback();
	glDrawArrays(GL_POINTS, 30, 5);
	glResumeTransformFeedback();
	glDrawArrays(GL_POINTS, 20, 2);
	glEndTransformFeedback();
	end_query(query, n, "several draws, pause and resume");
	check_gl_error("draws and pause");
	capture_read(&c, 0);
	check_records(&c, 0, n, verts, NULL, "several draws, pause and resume");
	capture_destroy(&c);
}

// Rasterizer discard: nothing is drawn, the capture happens.
static void
case_discard(void)
{
	unsigned verts[MAX_VERTS * 3];
	unsigned n = decompose(GL_TRIANGLES, 6, verts);
	struct capture c;

	glUseProgram(sProgramSeparate);
	bind_attributes(sProgramSeparate);
	glEnable(GL_RASTERIZER_DISCARD);
	capture_create(&c, 1, n + 1);
	GLuint query = begin_query();
	glBeginTransformFeedback(GL_TRIANGLES);
	glDrawArrays(GL_TRIANGLES, 0, 6);
	glEndTransformFeedback();
	end_query(query, n / 3, "rasterizer discard");
	glDisable(GL_RASTERIZER_DISCARD);
	check_gl_error("discard");
	capture_read(&c, 1);
	check_records(&c, 1, n, verts, NULL, "rasterizer discard");
	capture_destroy(&c);
}

struct named_case {
	const char* name;
	void (*run)(void);
	int desktopOnly;	// indexed draws: not allowed during ES transform feedback
};

static void run_points(void) { case_draw_arrays("points", GL_POINTS, GL_POINTS, 7, 0); }
static void run_lines(void) { case_draw_arrays("lines", GL_LINES, GL_LINES, 7, 1); }
static void run_line_strip(void) { case_draw_arrays("line strip", GL_LINE_STRIP, GL_LINES, 5, 0); }
static void run_triangles(void) { case_draw_arrays("triangles", GL_TRIANGLES, GL_TRIANGLES, 10, 1); }
static void run_triangle_strip(void) { case_draw_arrays("triangle strip", GL_TRIANGLE_STRIP, GL_TRIANGLES, 6, 0); }
static void run_triangle_fan(void) { case_draw_arrays("triangle fan", GL_TRIANGLE_FAN, GL_TRIANGLES, 6, 1); }

static const struct named_case kCases[] = {
	{ "points", run_points, 0 },
	{ "lines", run_lines, 0 },
	{ "line-strip", run_line_strip, 0 },
	{ "triangles", run_triangles, 0 },
	{ "triangle-strip", run_triangle_strip, 0 },
	{ "triangle-fan", run_triangle_fan, 0 },
	{ "indexed", case_indexed, 1 },
	{ "instanced", case_instanced, 1 },
	{ "overflow", case_overflow, 1 },	// ES: an error, not an overflow
	{ "draws-pause", case_draws_and_pause, 0 },
	{ "discard", case_discard, 0 },
};

static int
run(const char* only)
{
	EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	EGLint major = 0, minor = 0;
	if (!check(display != EGL_NO_DISPLAY
			&& eglInitialize(display, &major, &minor), "eglInitialize"))
		return 0;
	printf("EGL %d.%d, vendor %s, version %s\n", major, minor,
		eglQueryString(display, EGL_VENDOR),
		eglQueryString(display, EGL_VERSION));

	EGLint configAttributes[] = {
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RENDERABLE_TYPE, sDesktop ? EGL_OPENGL_BIT : EGL_OPENGL_ES3_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
		EGL_ALPHA_SIZE, 8,
		EGL_NONE
	};
	EGLConfig config;
	EGLint count = 0;
	if (!eglChooseConfig(display, configAttributes, &config, 1, &count)
			|| count != 1) {
		if (sDesktop)
			return check(0, "eglChooseConfig (OpenGL)");
		printf("no ES3 config; trying an ES2 one\n");
		configAttributes[3] = EGL_OPENGL_ES2_BIT;
		if (!check(eglChooseConfig(display, configAttributes, &config, 1,
					&count) && count == 1, "eglChooseConfig"))
			return 0;
	}
	const EGLint surfaceAttributes[] = { EGL_WIDTH, 64, EGL_HEIGHT, 64,
		EGL_NONE };
	EGLSurface surface = eglCreatePbufferSurface(display, config,
		surfaceAttributes);
	eglBindAPI(sDesktop ? EGL_OPENGL_API : EGL_OPENGL_ES_API);
	const EGLint esContextAttributes[] = {
		EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE
	};
	const EGLint glContextAttributes[] = {
		EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE
	};
	EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT,
		sDesktop ? glContextAttributes : esContextAttributes);
	if (context == EGL_NO_CONTEXT && sDesktop) {
		// EGL 1.4 without EGL_KHR_create_context: the highest version the
		// implementation offers (a compatibility context)
		printf("no versioned OpenGL context; taking the default one\n");
		context = eglCreateContext(display, config, EGL_NO_CONTEXT, NULL);
	}
	if (!check(surface != EGL_NO_SURFACE && context != EGL_NO_CONTEXT
			&& eglMakeCurrent(display, surface, surface, context),
			sDesktop ? "pbuffer, OpenGL 3.1 context, eglMakeCurrent"
				: "pbuffer, ES3 context, eglMakeCurrent"))
		return 0;

	const char* version = (const char*)glGetString(GL_VERSION);
	printf("GL_RENDERER: %s\nGL_VERSION: %s\nGL_SHADING_LANGUAGE_VERSION: "
		"%s\n", glGetString(GL_RENDERER), version,
		glGetString(GL_SHADING_LANGUAGE_VERSION));
	if (sDesktop) {
		if (!check(version[0] >= '3', "OpenGL 3.x"))
			return 0;
	} else if (!check(strncmp(version, "OpenGL ES 3.", 12) == 0,
			"OpenGL ES 3.x"))
		return 0;

	GLint maxInterleaved = 0, maxSeparate = 0, maxBuffers = 0;
	glGetIntegerv(GL_MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS,
		&maxInterleaved);
	glGetIntegerv(GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_COMPONENTS, &maxSeparate);
	glGetIntegerv(GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS, &maxBuffers);
	printf("transform feedback: %d interleaved components, %d separate "
		"components, %d separate attribs\n", maxInterleaved, maxSeparate,
		maxBuffers);

	for (unsigned i = 0; i < MAX_VERTS; i++) {
		sPositions[i * 4 + 0] = (float)i;
		sPositions[i * 4 + 1] = (float)(2 * i);
		sPositions[i * 4 + 2] = (float)(3 * i);
		sPositions[i * 4 + 3] = 1.0f;
		sUVs[i * 2 + 0] = i / 8.0f;
		sUVs[i * 2 + 1] = i / 4.0f;
		sInstanced[i] = 100.0f * i;
	}
	glGenBuffers(1, &sVertexBuffer);
	glBindBuffer(GL_ARRAY_BUFFER, sVertexBuffer);
	glBufferData(GL_ARRAY_BUFFER, sizeof(sPositions), sPositions,
		GL_STATIC_DRAW);
	glGenBuffers(1, &sUVBuffer);
	glBindBuffer(GL_ARRAY_BUFFER, sUVBuffer);
	glBufferData(GL_ARRAY_BUFFER, sizeof(sUVs), sUVs, GL_STATIC_DRAW);
	glGenBuffers(1, &sInstanceBuffer);
	glBindBuffer(GL_ARRAY_BUFFER, sInstanceBuffer);
	glBufferData(GL_ARRAY_BUFFER, sizeof(sInstanced), sInstanced,
		GL_STATIC_DRAW);
	glGenBuffers(1, &sIndexBuffer);

	GLuint vao;
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);

	sProgramInterleaved = link_program(GL_INTERLEAVED_ATTRIBS);
	sProgramSeparate = link_program(GL_SEPARATE_ATTRIBS);
	check_gl_error("setup");

	glViewport(0, 0, 64, 64);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);

	for (unsigned i = 0; i < sizeof(kCases) / sizeof(kCases[0]); i++) {
		if (only != NULL && strcmp(only, kCases[i].name) != 0)
			continue;
		if (kCases[i].desktopOnly && !sDesktop) {
			printf("-- %s: skipped (indexed draws need --desktop)\n",
				kCases[i].name);
			continue;
		}
		printf("-- %s\n", kCases[i].name);
		kCases[i].run();
	}
	glFinish();
	check_gl_error("end");

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
	int repeat = 1;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--case") == 0 && i + 1 < argc)
			only = argv[++i];
		else if (strcmp(argv[i], "--shim") == 0)
			sShim = 1;
		else if (strcmp(argv[i], "--desktop") == 0)
			sDesktop = 1;
		else if (strcmp(argv[i], "--repeat") == 0 && i + 1 < argc)
			repeat = atoi(argv[++i]);
		else {
			fprintf(stderr, "usage: pvr_glxfb [--case NAME] [--shim] "
				"[--repeat N] [--desktop]\n");
			return 2;
		}
	}
	for (int i = 0; i < repeat; i++) {
		if (!run(only))
			break;
	}
	printf("%s\n", sFailures == 0 ? "PASS" : "FAIL");
	return sFailures == 0 ? 0 : 1;
}
