/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

// What one compositing pass costs on the GPU, piece by piece: a full-screen
// pass into a framebuffer object shaped like Summit's (a GL_BGRA8_EXT
// renderbuffer, 1121x538 by default), timed from the first GL call to the
// end of glFinish(). Each case runs once to compile and warm up, then
// --frames times; its line gives the fastest and the mean frame and the
// nanoseconds per pixel drawn (fastest frame over pixels covered). The
// cases, each but "clear" after a glClear():
//   clear        the clear alone (the baseline: submit, wait, wake-up)
//   load         one constant-colour quad with no clear first (the render
//                loads the old contents)
//   solid        one constant-colour quad
//   uniform      the colour is a vec4 uniform (one UBO load per pixel on
//                zink)
//   mat4         the colour is a mat4 uniform times a vec4 uniform, as
//                TextureMapper applies u_textureColorSpaceMatrix
//   texture      a 512x512 RGBA texture (glTexImage2D with data) stretched
//                over the target
//   texture-bgra the same texture uploaded as GL_BGRA_EXT, the way WebKit's
//                tiles are
//   blend        "texture" with premultiplied blending (ONE,
//                ONE_MINUS_SRC_ALPHA)
//   rendered     per frame, a WebGL-like render into a 512x512 texture
//                (clear, depth, one quad), then the "texture" pass sampling
//                it
//   rtt          that render into the texture alone (subtract it from
//                "rendered")
//   webkit       TextureMapper's own shaders (TextureRGB + Opacity, its
//                vertex shader and matrices, the texture flipped) with
//                premultiplied blending, sampling the rendered texture
//   webkit-aa    the same with TextureMapper's antialiasing applier
//   depth        "texture" with a D24S8 renderbuffer attached and the depth
//                test on (the fast group in the board's firmware trace)
//   summit       "webkit" into Summit's real framebuffer: D24S8 attached,
//                depth and stencil tests off
// --passes N draws the quad N times (overdraw); --only NAME runs one case;
// --rgba makes the target GL_RGBA8 (SUMMIT_BGRA_TARGET=0); --size WxH the
// target; --texture WxH the sampled texture. After its frames, each case but
// "rtt" reads pixel (2, 2) back and checks its colour (within 2), which
// checks the shaders' uniform loads too; --shim skips that (the GPU runs
// nothing there).
//   pvr_glcomposite [--size WxH] [--texture WxH] [--frames N] [--passes N]
//       [--only NAME] [--rgba] [--expect TEXT] [--shim]
// PVR_LOG_RENDERS=1 (or =FILE) makes the Vulkan driver print one line per
// render job, so each case's jobs can be matched with a firmware trace.
// FAIL means setup failed, GL reported an error or a colour was wrong;
// under build.sh shim the GPU runs nothing, so the times measure only the
// CPU side.


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>


#ifndef GL_BGRA_EXT
#	define GL_BGRA_EXT				0x80E1
#endif
#define GL_BGRA8_EXT_VALUE			0x93A1
#ifndef GL_DEPTH24_STENCIL8_OES
#	define GL_DEPTH24_STENCIL8_OES	0x88F0
#endif

#define MAX_FRAMES	1000

enum {
	DRAW_NONE,
	DRAW_SOLID,
	DRAW_UNIFORM,
	DRAW_MAT4,
	DRAW_TEXTURE,
	DRAW_WEBKIT,
	DRAW_WEBKIT_AA
};

enum {
	TEXTURE_RGBA = 1,
	TEXTURE_BGRA,
	TEXTURE_RENDERED
};

struct composite_case {
	const char*	name;
	int			clear;
	int			draw;
	int			texture;
	int			blend;
	int			depthStencil;	// 1: attached, tests off; 2: depth test on
	int			renderTexture;	// render into the texture each frame
	int			compositePass;	// 0: only the render into the texture
	int			expected[4];	// pixel (2, 2) as RGBA; -1: not checked
};

static const struct composite_case kCases[] = {
	{ "clear", 1, DRAW_NONE, 0, 0, 0, 0, 1, { 255, 255, 255, 255 } },
	{ "load", 0, DRAW_SOLID, 0, 0, 0, 0, 1, { 64, 128, 191, 255 } },
	{ "solid", 1, DRAW_SOLID, 0, 0, 0, 0, 1, { 64, 128, 191, 255 } },
	{ "uniform", 1, DRAW_UNIFORM, 0, 0, 0, 0, 1, { 51, 102, 153, 255 } },
	{ "mat4", 1, DRAW_MAT4, 0, 0, 0, 0, 1, { 26, 77, 153, 255 } },
	{ "texture", 1, DRAW_TEXTURE, TEXTURE_RGBA, 0, 0, 0, 1,
		{ 0, 0, 48, 128 } },
	{ "texture-bgra", 1, DRAW_TEXTURE, TEXTURE_BGRA, 0, 0, 0, 1,
		{ 48, 0, 0, 128 } },
	{ "blend", 1, DRAW_TEXTURE, TEXTURE_RGBA, 1, 0, 0, 1,
		{ 127, 127, 175, 255 } },
	{ "rendered", 1, DRAW_TEXTURE, TEXTURE_RENDERED, 0, 0, 1, 1,
		{ 64, 128, 191, 255 } },
	{ "rtt", 1, DRAW_NONE, TEXTURE_RENDERED, 0, 0, 1, 0, { -1 } },
	{ "webkit", 1, DRAW_WEBKIT, TEXTURE_RENDERED, 1, 0, 1, 1,
		{ 83, 141, 198, 255 } },
	{ "webkit-aa", 1, DRAW_WEBKIT_AA, TEXTURE_RENDERED, 1, 0, 1, 1,
		{ 255, 255, 255, 255 } },
	{ "depth", 1, DRAW_TEXTURE, TEXTURE_RGBA, 0, 2, 0, 1,
		{ 0, 0, 48, 128 } },
	{ "summit", 1, DRAW_WEBKIT, TEXTURE_RENDERED, 1, 1, 1, 1,
		{ 83, 141, 198, 255 } },
};

#define CASE_COUNT	(sizeof(kCases) / sizeof(kCases[0]))


static const char* kSimpleVertex =
	"attribute vec2 a_position;\n"
	"varying vec2 v_texCoord;\n"
	"void main()\n"
	"{\n"
	"	v_texCoord = a_position * 0.5 + 0.5;\n"
	"	gl_Position = vec4(a_position, 0.0, 1.0);\n"
	"}\n";

static const char* kSolidFragment =
	"precision mediump float;\n"
	"void main() { gl_FragColor = vec4(0.25, 0.5, 0.75, 1.0); }\n";

static const char* kUniformFragment =
	"precision mediump float;\n"
	"uniform vec4 u_color;\n"
	"void main() { gl_FragColor = u_color; }\n";

static const char* kMat4Fragment =
	"precision mediump float;\n"
	"uniform mat4 u_matrix;\n"
	"uniform vec4 u_color;\n"
	"void main() { gl_FragColor = u_matrix * u_color; }\n";

static const char* kTextureFragment =
	"precision mediump float;\n"
	"uniform sampler2D s_sampler;\n"
	"varying vec2 v_texCoord;\n"
	"void main() { gl_FragColor = texture2D(s_sampler, v_texCoord); }\n";

// TextureMapperShaderProgram.cpp (WebKit, Summit's tree), the OpenGL ES 2
// path, with its appliers switched by the same defines: TextureRGB and
// Opacity, and Antialiasing for webkit-aa. The parts no applier here uses
// (filters, YUV, blur, rounded-rect clips, tone mapping) are left out;
// their uniforms would be inactive.
static const char* kWebKitVertex =
	"#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
	"#define TextureSpaceMatrixPrecision highp\n"
	"#else\n"
	"#define TextureSpaceMatrixPrecision mediump\n"
	"#endif\n"
	"precision TextureSpaceMatrixPrecision float;\n"
	"attribute vec4 a_vertex;\n"
	"varying vec2 v_texCoord;\n"
	"varying vec2 v_transformedTexCoord;\n"
	"varying float v_antialias;\n"
	"varying highp vec4 v_nonProjectedPosition;\n"
	"uniform mat4 u_modelViewMatrix;\n"
	"uniform mat4 u_projectionMatrix;\n"
	"uniform mat4 u_textureSpaceMatrix;\n"
	"void noop(vec2 position) { }\n"
	"vec4 toViewportSpace(vec2 pos)"
	" { return u_modelViewMatrix * vec4(pos, 0., 1.); }\n"
	"void applyAntialiasing(vec2 position)\n"
	"{\n"
	"	const vec2 center = vec2(0.5, 0.5);\n"
	"	const float antialiasInflationDistance = 1.;\n"
	"	vec2 controlPoint = a_vertex.zw;\n"
	"	bool isCenter = distance(position, controlPoint) > 0.;\n"
	"	if (isCenter) {\n"
	"		vec4 controlPointInViewportCoordinates"
	" = toViewportSpace(controlPoint);\n"
	"		float viewportSpaceDistance = distance(v_nonProjectedPosition.xy"
	" * controlPointInViewportCoordinates.w,"
	" controlPointInViewportCoordinates.xy * v_nonProjectedPosition.w);\n"
	"		if (controlPointInViewportCoordinates.w > 0.)\n"
	"			viewportSpaceDistance /= controlPointInViewportCoordinates.w;\n"
	"		v_antialias = (viewportSpaceDistance + antialiasInflationDistance"
	" * v_nonProjectedPosition.w) / antialiasInflationDistance;\n"
	"	} else {\n"
	"		vec4 centerInViewportCoordinates = toViewportSpace(center);\n"
	"		vec2 direction = v_nonProjectedPosition.xy"
	" * centerInViewportCoordinates.w - centerInViewportCoordinates.xy"
	" * v_nonProjectedPosition.w;\n"
	"		if (length(direction) > 0.) {\n"
	"			float oldDistance = distance(v_nonProjectedPosition.xyz,"
	" centerInViewportCoordinates.xyz);\n"
	"			v_nonProjectedPosition += vec4(normalize(direction)"
	" * antialiasInflationDistance * v_nonProjectedPosition.w, 0., 0.);\n"
	"			float newDistance = distance(v_nonProjectedPosition.xyz,"
	" centerInViewportCoordinates.xyz);\n"
	"			v_texCoord += normalize(position - center)"
	" * (newDistance - oldDistance) / oldDistance;\n"
	"		}\n"
	"		v_antialias = 0.;\n"
	"	}\n"
	"}\n"
	"void main(void)\n"
	"{\n"
	"	vec2 position = a_vertex.xy;\n"
	"	v_texCoord = position;\n"
	"	v_transformedTexCoord"
	" = (u_textureSpaceMatrix * vec4(position, 0., 1.)).xy;\n"
	"	v_nonProjectedPosition = toViewportSpace(position);\n"
	"	applyAntialiasingIfNeeded(position);\n"
	"	gl_Position = u_projectionMatrix * v_nonProjectedPosition;\n"
	"}\n";

static const char* kWebKitFragment =
	"#if defined(ENABLE_Antialiasing)\n"
	"#define transformTexCoord fragmentTransformTexCoord\n"
	"#else\n"
	"#define transformTexCoord vertexTransformTexCoord\n"
	"#endif\n"
	"#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
	"#define TextureSpaceMatrixPrecision highp\n"
	"#else\n"
	"#define TextureSpaceMatrixPrecision mediump\n"
	"#endif\n"
	"precision TextureSpaceMatrixPrecision float;\n"
	"uniform mat4 u_textureSpaceMatrix;\n"
	"uniform mat4 u_textureColorSpaceMatrix;\n"
	"precision mediump float;\n"
	"varying float v_antialias;\n"
	"varying vec2 v_texCoord;\n"
	"varying vec2 v_transformedTexCoord;\n"
	"varying highp vec4 v_nonProjectedPosition;\n"
	"uniform sampler2D s_sampler;\n"
	"uniform float u_opacity;\n"
	"void noop(inout vec4 dummyParameter) { }\n"
	"void noop(inout vec4 dummyParameter, vec2 texCoord) { }\n"
	"void noop(inout vec2 dummyParameter) { }\n"
	"float antialias()\n"
	"{\n"
	"	if (v_nonProjectedPosition.w <= 0.)\n"
	"		return 1.;\n"
	"	return smoothstep(0., 1., v_antialias / v_nonProjectedPosition.w);\n"
	"}\n"
	"vec2 fragmentTransformTexCoord()\n"
	"{\n"
	"	vec4 clampedPosition = clamp(vec4(v_texCoord, 0., 1.), 0., 1.);\n"
	"	return (u_textureSpaceMatrix * clampedPosition).xy;\n"
	"}\n"
	"vec2 vertexTransformTexCoord() { return v_transformedTexCoord; }\n"
	"void applyTextureRGB(inout vec4 color, vec2 texCoord)"
	" { color = u_textureColorSpaceMatrix * texture2D(s_sampler, texCoord); }\n"
	"void applyOpacity(inout vec4 color) { color *= u_opacity; }\n"
	"void applyAntialiasing(inout vec4 color) { color *= antialias(); }\n"
	"void main(void)\n"
	"{\n"
	"	vec4 color = vec4(1., 1., 1., 1.);\n"
	"	vec2 texCoord = transformTexCoord();\n"
	"	applyManualRepeatIfNeeded(texCoord);\n"
	"	applyClampUVBoundsIfNeeded(texCoord);\n"
	"	applyTextureRGBIfNeeded(color, texCoord);\n"
	"	applyPremultiplyIfNeeded(color);\n"
	"	applySolidColorIfNeeded(color);\n"
	"	applyAntialiasingIfNeeded(color);\n"
	"	applyOpacityIfNeeded(color);\n"
	"	gl_FragColor = color;\n"
	"}\n";

static const char* kWebKitOptions =
	"#define ENABLE_TextureRGB\n"
	"#define applyTextureRGBIfNeeded applyTextureRGB\n"
	"#define ENABLE_Opacity\n"
	"#define applyOpacityIfNeeded applyOpacity\n"
	"#define applyManualRepeatIfNeeded noop\n"
	"#define applyClampUVBoundsIfNeeded noop\n"
	"#define applyPremultiplyIfNeeded noop\n"
	"#define applySolidColorIfNeeded noop\n";

static const char* kAntialiasOn =
	"#define ENABLE_Antialiasing\n"
	"#define applyAntialiasingIfNeeded applyAntialiasing\n";

static const char* kAntialiasOff =
	"#define applyAntialiasingIfNeeded noop\n";


static int sWidth = 1121, sHeight = 538;
static int sTextureWidth = 512, sTextureHeight = 512;
static unsigned sFrames = 20, sPasses = 1;
static GLuint sQuadBuffer, sWebKitBuffer;
static GLuint sPrograms[DRAW_WEBKIT_AA + 1];
static GLuint sTextures[TEXTURE_RENDERED + 1];
static GLuint sRenderFramebuffer, sRenderDepth;
static int sGLErrors, sWrongPixels, sShim;


static double
now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}


static int
check_gl(const char* what)
{
	GLenum error = glGetError();
	if (error == GL_NO_ERROR)
		return 1;
	printf("FAIL: %s: GL error 0x%x\n", what, error);
	sGLErrors++;
	return 0;
}


static GLuint
compile(GLenum type, const char* const* parts, int count)
{
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, count, parts, NULL);
	glCompileShader(shader);
	GLint ok = 0;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[1024] = "";
		glGetShaderInfoLog(shader, sizeof(log), NULL, log);
		printf("FAIL: shader: %s\n", log);
		return 0;
	}
	return shader;
}


static GLuint
link_program(const char* const* vertex, int vertexCount,
	const char* const* fragment, int fragmentCount, const char* attribute)
{
	GLuint vs = compile(GL_VERTEX_SHADER, vertex, vertexCount);
	GLuint fs = compile(GL_FRAGMENT_SHADER, fragment, fragmentCount);
	if (vs == 0 || fs == 0)
		return 0;
	GLuint program = glCreateProgram();
	glAttachShader(program, vs);
	glAttachShader(program, fs);
	glBindAttribLocation(program, 0, attribute);
	glLinkProgram(program);
	GLint ok = 0;
	glGetProgramiv(program, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[1024] = "";
		glGetProgramInfoLog(program, sizeof(log), NULL, log);
		printf("FAIL: link: %s\n", log);
		return 0;
	}
	return program;
}


// column-major 4x4 matrices
static void
matrix_identity(float* m)
{
	memset(m, 0, 16 * sizeof(float));
	m[0] = m[5] = m[10] = m[15] = 1;
}


static int
make_programs(void)
{
	const char* simple[] = { kSimpleVertex };
	const char* solid[] = { kSolidFragment };
	const char* uniform[] = { kUniformFragment };
	const char* mat4[] = { kMat4Fragment };
	const char* texture[] = { kTextureFragment };
	sPrograms[DRAW_SOLID] = link_program(simple, 1, solid, 1, "a_position");
	sPrograms[DRAW_UNIFORM] = link_program(simple, 1, uniform, 1,
		"a_position");
	sPrograms[DRAW_MAT4] = link_program(simple, 1, mat4, 1, "a_position");
	sPrograms[DRAW_TEXTURE] = link_program(simple, 1, texture, 1,
		"a_position");
	for (int aa = 0; aa < 2; aa++) {
		const char* vertex[] = { kWebKitOptions,
			aa ? kAntialiasOn : kAntialiasOff, kWebKitVertex };
		const char* fragment[] = { kWebKitOptions,
			aa ? kAntialiasOn : kAntialiasOff, kWebKitFragment };
		sPrograms[aa ? DRAW_WEBKIT_AA : DRAW_WEBKIT] = link_program(vertex, 3,
			fragment, 3, "a_vertex");
	}
	for (int i = DRAW_SOLID; i <= DRAW_WEBKIT_AA; i++) {
		if (sPrograms[i] == 0)
			return 0;
	}

	GLuint program = sPrograms[DRAW_UNIFORM];
	glUseProgram(program);
	glUniform4f(glGetUniformLocation(program, "u_color"), 0.2f, 0.4f, 0.6f,
		1);
	program = sPrograms[DRAW_MAT4];
	glUseProgram(program);
	float m[16];
	matrix_identity(m);
	m[0] = 0.5f;
	m[5] = 0.75f;
	glUniformMatrix4fv(glGetUniformLocation(program, "u_matrix"), 1,
		GL_FALSE, m);
	glUniform4f(glGetUniformLocation(program, "u_color"), 0.2f, 0.4f, 0.6f,
		1);
	program = sPrograms[DRAW_TEXTURE];
	glUseProgram(program);
	glUniform1i(glGetUniformLocation(program, "s_sampler"), 0);

	// TextureMapper's matrices for a layer covering the whole target: the
	// unit square scaled to the target in pixels, an orthographic
	// projection, the texture flipped (ShouldFlipTexture), no colour change
	for (int i = DRAW_WEBKIT; i <= DRAW_WEBKIT_AA; i++) {
		program = sPrograms[i];
		glUseProgram(program);
		float modelView[16], projection[16], textureSpace[16], color[16];
		matrix_identity(modelView);
		modelView[0] = (float)sWidth;
		modelView[5] = (float)sHeight;
		matrix_identity(projection);
		projection[0] = 2.0f / sWidth;
		projection[5] = 2.0f / sHeight;
		projection[12] = -1;
		projection[13] = -1;
		matrix_identity(textureSpace);
		textureSpace[5] = -1;
		textureSpace[13] = 1;
		matrix_identity(color);
		glUniformMatrix4fv(glGetUniformLocation(program, "u_modelViewMatrix"),
			1, GL_FALSE, modelView);
		glUniformMatrix4fv(glGetUniformLocation(program,
			"u_projectionMatrix"), 1, GL_FALSE, projection);
		glUniformMatrix4fv(glGetUniformLocation(program,
			"u_textureSpaceMatrix"), 1, GL_FALSE, textureSpace);
		glUniformMatrix4fv(glGetUniformLocation(program,
			"u_textureColorSpaceMatrix"), 1, GL_FALSE, color);
		glUniform1f(glGetUniformLocation(program, "u_opacity"), 0.9f);
		glUniform1i(glGetUniformLocation(program, "s_sampler"), 0);
	}
	glUseProgram(0);
	return check_gl("programs");
}


static int
make_textures(void)
{
	const size_t size = (size_t)sTextureWidth * sTextureHeight * 4;
	uint8_t* pixels = malloc(size);
	if (pixels == NULL)
		return 0;
	for (int y = 0; y < sTextureHeight; y++) {
		for (int x = 0; x < sTextureWidth; x++) {
			uint8_t* p = pixels + ((size_t)y * sTextureWidth + x) * 4;
			// premultiplied, half transparent in a checkerboard
			int opaque = ((x / 32) ^ (y / 32)) & 1;
			p[0] = (uint8_t)(x * 255 / sTextureWidth) / (opaque ? 1 : 2);
			p[1] = (uint8_t)(y * 255 / sTextureHeight) / (opaque ? 1 : 2);
			p[2] = 96 / (opaque ? 1 : 2);
			p[3] = opaque ? 255 : 128;
		}
	}

	glGenTextures(TEXTURE_RENDERED, sTextures + 1);
	for (int i = TEXTURE_RGBA; i <= TEXTURE_RENDERED; i++) {
		glBindTexture(GL_TEXTURE_2D, sTextures[i]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		if (i == TEXTURE_BGRA) {
			// WebKit's tiles: BGRA bytes, GL_BGRA_EXT as both formats
			glTexImage2D(GL_TEXTURE_2D, 0, GL_BGRA_EXT, sTextureWidth,
				sTextureHeight, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, pixels);
			if (glGetError() != GL_NO_ERROR) {
				printf("note: no GL_BGRA_EXT textures; texture-bgra samples "
					"RGBA\n");
				glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, sTextureWidth,
					sTextureHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
			}
		} else {
			// the WebGL drawing buffer is made as GraphicsContextGLCoordinated
			// makes it: glTexImage2D(GL_RGBA) without data
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, sTextureWidth,
				sTextureHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE,
				i == TEXTURE_RENDERED ? NULL : pixels);
		}
	}
	glBindTexture(GL_TEXTURE_2D, 0);
	free(pixels);

	// the WebGL side: its drawing buffer with a depth buffer of its own
	glGenFramebuffers(1, &sRenderFramebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, sRenderFramebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		GL_TEXTURE_2D, sTextures[TEXTURE_RENDERED], 0);
	glGenRenderbuffers(1, &sRenderDepth);
	glBindRenderbuffer(GL_RENDERBUFFER, sRenderDepth);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16,
		sTextureWidth, sTextureHeight);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
		GL_RENDERBUFFER, sRenderDepth);
	GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	if (status != GL_FRAMEBUFFER_COMPLETE) {
		printf("FAIL: the render-to-texture framebuffer: 0x%x\n", status);
		return 0;
	}
	return check_gl("textures");
}


static void
make_quads(void)
{
	// a full-target triangle strip in clip space
	static const float quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
	glGenBuffers(1, &sQuadBuffer);
	glBindBuffer(GL_ARRAY_BUFFER, sQuadBuffer);
	glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);

	// TextureMapper's unit square: a_vertex.xy the corner, .zw the control
	// point (the corner itself: no antialiasing inflation)
	static const float unit[] = {
		0, 0, 0, 0,
		1, 0, 1, 0,
		0, 1, 0, 1,
		1, 1, 1, 1
	};
	glGenBuffers(1, &sWebKitBuffer);
	glBindBuffer(GL_ARRAY_BUFFER, sWebKitBuffer);
	glBufferData(GL_ARRAY_BUFFER, sizeof(unit), unit, GL_STATIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
}


static void
draw_quad(int draw)
{
	glUseProgram(sPrograms[draw]);
	if (draw == DRAW_WEBKIT || draw == DRAW_WEBKIT_AA) {
		glBindBuffer(GL_ARRAY_BUFFER, sWebKitBuffer);
		glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, NULL);
	} else {
		glBindBuffer(GL_ARRAY_BUFFER, sQuadBuffer);
		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
	}
	glEnableVertexAttribArray(0);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}


// what WebGL does each frame into its drawing buffer: clear colour and
// depth, one quad with the depth test on
static void
render_into_texture(void)
{
	glBindFramebuffer(GL_FRAMEBUFFER, sRenderFramebuffer);
	glViewport(0, 0, sTextureWidth, sTextureHeight);
	glClearColor(0.1f, 0.6f, 0.3f, 1);
	glClearDepthf(1);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glEnable(GL_DEPTH_TEST);
	draw_quad(DRAW_SOLID);
	glDisable(GL_DEPTH_TEST);
}


static int
run_case(const struct composite_case* c, GLuint framebuffer,
	GLuint framebufferDS, double* clearBest)
{
	double times[MAX_FRAMES];
	const unsigned frames = sFrames;
	for (unsigned frame = 0; frame <= frames; frame++) {
		double start = now_ms();
		if (c->renderTexture)
			render_into_texture();
		if (c->compositePass) {
			glBindFramebuffer(GL_FRAMEBUFFER,
				c->depthStencil ? framebufferDS : framebuffer);
			glViewport(0, 0, sWidth, sHeight);
			if (c->clear) {
				glClearColor(1, 1, 1, 1);
				glClearDepthf(1);
				glClear(GL_COLOR_BUFFER_BIT
					| (c->depthStencil == 2 ? GL_DEPTH_BUFFER_BIT : 0));
			}
			if (c->depthStencil == 2) {
				glEnable(GL_DEPTH_TEST);
				glDepthFunc(GL_LEQUAL);
			}
			if (c->blend) {
				glEnable(GL_BLEND);
				glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
			}
			if (c->texture != 0) {
				glActiveTexture(GL_TEXTURE0);
				glBindTexture(GL_TEXTURE_2D, sTextures[c->texture]);
			}
			for (unsigned pass = 0; pass < sPasses && c->draw != DRAW_NONE;
					pass++) {
				draw_quad(c->draw);
			}
			glDisable(GL_BLEND);
			glDisable(GL_DEPTH_TEST);
			glBindTexture(GL_TEXTURE_2D, 0);
		}
		glFinish();
		double elapsed = now_ms() - start;
		if (!check_gl(c->name))
			return 0;
		if (frame > 0)
			times[frame - 1] = elapsed;
	}

	// the last frame's pixel (2, 2): the uniform cases check that the
	// shaders' uniform loads read the right values
	char check[64] = "";
	if (c->expected[0] >= 0) {
		uint8_t pixel[4] = { 0 };
		glBindFramebuffer(GL_FRAMEBUFFER,
			c->depthStencil ? framebufferDS : framebuffer);
		glReadPixels(2, 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		if (!check_gl("glReadPixels"))
			return 0;
		int wrong = 0;
		for (int i = 0; i < 4; i++) {
			int difference = pixel[i] - c->expected[i];
			if (difference < -2 || difference > 2)
				wrong = 1;
		}
		if (wrong && !sShim) {
			sWrongPixels++;
			snprintf(check, sizeof(check), ", WRONG %u,%u,%u,%u",
				pixel[0], pixel[1], pixel[2], pixel[3]);
		} else if (!sShim)
			snprintf(check, sizeof(check), ", pixel ok");
	}

	double best = times[0], total = 0;
	for (unsigned i = 0; i < frames; i++) {
		total += times[i];
		if (times[i] < best)
			best = times[i];
	}
	const double pixels = c->compositePass
		? (double)sWidth * sHeight * (c->draw != DRAW_NONE ? sPasses : 1)
		: (double)sTextureWidth * sTextureHeight;
	if (strcmp(c->name, "clear") == 0)
		*clearBest = best;
	double over = best - *clearBest;
	printf("%-13s %8.2f ms best %8.2f ms mean %7.1f ns/px"
		" (%+.2f ms over clear%s)\n", c->name, best, total / frames,
		best * 1e6 / pixels, over, check);
	return 1;
}


int
main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);

	const char* only = NULL;
	const char* expect = NULL;
	int rgba = 0;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--size") == 0 && i + 1 < argc)
			sscanf(argv[++i], "%dx%d", &sWidth, &sHeight);
		else if (strcmp(argv[i], "--texture") == 0 && i + 1 < argc)
			sscanf(argv[++i], "%dx%d", &sTextureWidth, &sTextureHeight);
		else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
			sFrames = strtoul(argv[++i], NULL, 0);
		else if (strcmp(argv[i], "--passes") == 0 && i + 1 < argc)
			sPasses = strtoul(argv[++i], NULL, 0);
		else if (strcmp(argv[i], "--only") == 0 && i + 1 < argc)
			only = argv[++i];
		else if (strcmp(argv[i], "--expect") == 0 && i + 1 < argc)
			expect = argv[++i];
		else if (strcmp(argv[i], "--rgba") == 0)
			rgba = 1;
		else if (strcmp(argv[i], "--shim") == 0)
			sShim = 1;
		else {
			printf("usage: %s [--size WxH] [--texture WxH] [--frames N] "
				"[--passes N] [--only NAME] [--rgba] [--expect TEXT] "
				"[--shim]\n", argv[0]);
			return 2;
		}
	}
	if (sFrames < 1 || sFrames > MAX_FRAMES || sPasses < 1 || sWidth < 1
		|| sHeight < 1 || sTextureWidth < 1 || sTextureHeight < 1) {
		printf("FAIL: bad arguments\n");
		return 2;
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
		printf("FAIL: EGL context\n");
		return 1;
	}
	const char* renderer = (const char*)glGetString(GL_RENDERER);
	printf("GL_RENDERER: %s\n", renderer ? renderer : "?");
	if (expect != NULL && (renderer == NULL || !strstr(renderer, expect))) {
		printf("FAIL: the renderer is not %s\n", expect);
		return 1;
	}

	// Summit's target: a GL_BGRA8_EXT renderbuffer (GL_RGBA8 with --rgba,
	// or when BGRA8 is refused), alone and with its D24S8 renderbuffer
	GLuint color, depthStencil, framebuffers[2];
	glGenRenderbuffers(1, &color);
	glBindRenderbuffer(GL_RENDERBUFFER, color);
	const char* target = "GL_BGRA8_EXT";
	if (!rgba) {
		glRenderbufferStorage(GL_RENDERBUFFER, GL_BGRA8_EXT_VALUE, sWidth,
			sHeight);
		if (glGetError() != GL_NO_ERROR)
			rgba = 1;
	}
	if (rgba) {
		target = "GL_RGBA8";
		glRenderbufferStorage(GL_RENDERBUFFER, 0x8058 /* GL_RGBA8_OES */,
			sWidth, sHeight);
	}
	glGenRenderbuffers(1, &depthStencil);
	glBindRenderbuffer(GL_RENDERBUFFER, depthStencil);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8_OES, sWidth,
		sHeight);
	glGenFramebuffers(2, framebuffers);
	for (int i = 0; i < 2; i++) {
		glBindFramebuffer(GL_FRAMEBUFFER, framebuffers[i]);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			GL_RENDERBUFFER, color);
		if (i == 1) {
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
				GL_RENDERBUFFER, depthStencil);
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT,
				GL_RENDERBUFFER, depthStencil);
		}
		GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		if (status != GL_FRAMEBUFFER_COMPLETE) {
			printf("FAIL: framebuffer %d: 0x%x\n", i, status);
			return 1;
		}
	}
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	if (!check_gl("framebuffers") || !make_programs() || !make_textures())
		return 1;
	make_quads();

	printf("target %dx%d %s, texture %dx%d, %u frames of %u pass(es) "
		"per case\n", sWidth, sHeight, target, sTextureWidth,
		sTextureHeight, sFrames, sPasses);
	double clearBest = 0;
	int ran = 0;
	for (unsigned i = 0; i < CASE_COUNT; i++) {
		const struct composite_case* c = &kCases[i];
		// every case is measured against the clear
		if (only != NULL && strcmp(only, c->name) != 0
			&& strcmp(c->name, "clear") != 0)
			continue;
		if (!run_case(c, framebuffers[0], framebuffers[1], &clearBest))
			break;
		ran++;
	}
	if (only != NULL && ran < 2 && strcmp(only, "clear") != 0)
		printf("FAIL: no case %s\n", only);

	glDeleteFramebuffers(2, framebuffers);
	glDeleteFramebuffers(1, &sRenderFramebuffer);
	glDeleteRenderbuffers(1, &color);
	glDeleteRenderbuffers(1, &depthStencil);
	glDeleteRenderbuffers(1, &sRenderDepth);
	glDeleteTextures(TEXTURE_RENDERED, sTextures + 1);
	eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroyContext(display, context);
	eglDestroySurface(display, surface);
	eglTerminate(display);
	if (sWrongPixels != 0)
		printf("%d case(s) drew the wrong colour\n", sWrongPixels);
	int ok = sGLErrors == 0 && sWrongPixels == 0
		&& (only == NULL ? ran == (int)CASE_COUNT
			: ran >= 2 || strcmp(only, "clear") == 0);
	printf("%s\n", ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
