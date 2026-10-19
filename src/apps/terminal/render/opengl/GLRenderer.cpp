/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	Terminal's OpenGL renderer: draws the cells and images with the GPU into
	an offscreen frame buffer the size of the view in frame buffer pixels,
	and reads back the rows that changed.

	The context comes from a BGLView that is never shown: BGLView makes its
	context when it is constructed, with any of Haiku's OpenGL stacks (Mesa's
	own renderers, Zink on NVK, radeonsi through glvnd). Presenting through
	the BGLView itself would draw at the view's logical size, which is
	blurry on a scaled desktop.

	Colors are written with red and blue swapped, so that the GL_RGBA
	readback gives the bytes of B_RGB32 (B, G, R, A) and needs no swizzle,
	which some drivers do with the processor.
*/


#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include <GLView.h>
#include <OS.h>

#define GL_GLEXT_PROTOTYPES 0
#include <GL/gl.h>
#include <GL/glext.h>

#include "TermRenderer.h"


#define GL_FUNCTIONS(F) \
	F(PFNGLCREATESHADERPROC, glCreateShader) \
	F(PFNGLSHADERSOURCEPROC, glShaderSource) \
	F(PFNGLCOMPILESHADERPROC, glCompileShader) \
	F(PFNGLGETSHADERIVPROC, glGetShaderiv) \
	F(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog) \
	F(PFNGLCREATEPROGRAMPROC, glCreateProgram) \
	F(PFNGLATTACHSHADERPROC, glAttachShader) \
	F(PFNGLLINKPROGRAMPROC, glLinkProgram) \
	F(PFNGLGETPROGRAMIVPROC, glGetProgramiv) \
	F(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog) \
	F(PFNGLUSEPROGRAMPROC, glUseProgram) \
	F(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation) \
	F(PFNGLUNIFORM1IPROC, glUniform1i) \
	F(PFNGLUNIFORM1FPROC, glUniform1f) \
	F(PFNGLUNIFORM2FPROC, glUniform2f) \
	F(PFNGLUNIFORM2IPROC, glUniform2i) \
	F(PFNGLUNIFORM4FPROC, glUniform4f) \
	F(PFNGLUNIFORM4IPROC, glUniform4i) \
	F(PFNGLACTIVETEXTUREPROC, glActiveTexture) \
	F(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers) \
	F(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer) \
	F(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D) \
	F(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus) \
	F(PFNGLDELETEFRAMEBUFFERSPROC, glDeleteFramebuffers) \
	F(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays) \
	F(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray) \
	F(PFNGLBLENDFUNCSEPARATEPROC, glBlendFuncSeparate) \
	F(PFNGLGENBUFFERSPROC, glGenBuffers) \
	F(PFNGLBINDBUFFERPROC, glBindBuffer) \
	F(PFNGLBUFFERDATAPROC, glBufferData) \
	F(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer) \
	F(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray) \
	F(PFNGLBINDATTRIBLOCATIONPROC, glBindAttribLocation)

#define DECLARE(type, name) static type p_##name;
GL_FUNCTIONS(DECLARE)
#undef DECLARE


static const char* const kVertexShader =
	"#version 130\n"
	"uniform vec4 uRect;\n"
	"uniform vec2 uViewport;\n"
	"uniform vec4 uTextureRect;\n"
	"in vec2 aCorner;\n"
	"out vec2 vTexture;\n"
	"void main() {\n"
	"	vec2 corner = aCorner;\n"
	"	vec2 pixel = uRect.xy + corner * uRect.zw;\n"
	"	vTexture = mix(uTextureRect.xy, uTextureRect.zw, corner);\n"
	"	gl_Position = vec4(pixel / uViewport * 2.0 - 1.0, 0.0, 1.0);\n"
	"}\n";

// the cells: backgrounds (uMode 0), text and decorations over them (uMode
// 1), or both at once (uMode 2, where no image lies between them)
static const char* const kCellShader =
	"#version 130\n"
	"uniform usampler2D uCells;\n"
	"uniform sampler2D uAtlas0;\n"
	"uniform sampler2D uAtlas1;\n"
	"uniform sampler2D uAtlas2;\n"
	"uniform sampler2D uAtlas3;\n"
	"uniform sampler2D uColor0;\n"	// color glyphs, B, G, R, A
	"uniform sampler2D uColor1;\n"
	"uniform sampler2D uColor2;\n"
	"uniform sampler2D uColor3;\n"
	"uniform int uMode;\n"
	"uniform int uSkipDefault;\n"
	"uniform ivec2 uCellSize;\n"
	"uniform ivec2 uGrid;\n"
	"uniform ivec2 uSlots;\n"		// per row, per page
	"uniform float uPageSize;\n"
	"uniform ivec4 uMetrics;\n"		// underline, thickness, strike, line
	"out vec4 fragColor;\n"
	"vec3 color(uint value) {\n"
	"	return vec3(float((value >> 16) & 255u), float((value >> 8) & 255u),\n"
	"		float(value & 255u)) / 255.0;\n"
	"}\n"
	"float coverage(int page, vec2 position) {\n"
	"	if (page == 0) return texture(uAtlas0, position).r;\n"
	"	if (page == 1) return texture(uAtlas1, position).r;\n"
	"	if (page == 2) return texture(uAtlas2, position).r;\n"
	"	return texture(uAtlas3, position).r;\n"
	"}\n"
	"vec4 colorGlyph(int page, vec2 position) {\n"
	"	if (page == 0) return texture(uColor0, position);\n"
	"	if (page == 1) return texture(uColor1, position);\n"
	"	if (page == 2) return texture(uColor2, position);\n"
	"	return texture(uColor3, position);\n"
	"}\n"
	"float band(int y, int top, int height) {\n"
	"	return (y >= top && y < top + height) ? 1.0 : 0.0;\n"
	"}\n"
	"void main() {\n"
	"	ivec2 pixel = ivec2(gl_FragCoord.xy);\n"
	"	ivec2 cell = pixel / uCellSize;\n"
	"	if (cell.x >= uGrid.x || cell.y >= uGrid.y) discard;\n"
	"	ivec2 local = pixel - cell * uCellSize;\n"
	"	uvec4 data = texelFetch(uCells, cell, 0);\n"
	"	uint flags = (data.z >> 24) | ((data.w >> 24) << 8);\n"
	"	if (uMode == 0) {\n"
	"		if (uSkipDefault != 0 && (flags & 128u) != 0u) discard;\n"
	"		fragColor = vec4(color(data.z).bgr, 1.0);\n"
	"		return;\n"
	"	}\n"
	"	float alpha = float(data.y >> 24) / 255.0;\n"
	"	bool colored = (flags & 64u) != 0u;\n"
	"	float text = 0.0;\n"
	"	vec4 emoji = vec4(0.0);\n"
	"	if (data.x != 0u) {\n"
	"		int slot = int(data.x);\n"
	"		int page = slot / uSlots.y;\n"
	"		int inPage = slot - page * uSlots.y;\n"
	"		ivec2 origin = ivec2(inPage % uSlots.x, inPage / uSlots.x)\n"
	"			* uCellSize;\n"
	"		if ((flags & 32u) != 0u) origin.x += uCellSize.x;\n"
	"		vec2 position = (vec2(origin + local) + 0.5) / uPageSize;\n"
	"		if (colored) emoji = colorGlyph(page, position);\n"
	"		else text = coverage(page, position);\n"
	"	}\n"
	"	int x = local.x;\n"
	"	int y = local.y;\n"
	"	int width = uCellSize.x;\n"
	"	int height = uCellSize.y;\n"
	"	int top = uMetrics.x;\n"
	"	int thickness = uMetrics.y;\n"
	"	float marks = 0.0;\n"	// strike through and overline
	"	if ((flags & 8u) != 0u) marks = max(marks, band(y, uMetrics.z, uMetrics.w));\n"
	"	if ((flags & 16u) != 0u) marks = max(marks, band(y, 0, uMetrics.w));\n"
	"	text = max(text, marks) * alpha;\n"
	"	vec3 glyphColor = color(data.y) * text;\n"
	"	float glyphAlpha = text;\n"
	"	if (colored) {\n"
	"		emoji *= alpha;\n"
	"		float m = marks * alpha;\n"
	"		glyphColor = color(data.y) * m + emoji.bgr * (1.0 - m);\n"
	"		glyphAlpha = m + emoji.a * (1.0 - m);\n"
	"	}\n"
	"	float line = 0.0;\n"
	"	int style = int(flags & 7u);\n"
	"	if (style == 1) {\n"
	"		line = band(y, top, thickness);\n"
	"	} else if (style == 2) {\n"
	"		int second = top + thickness * 2;\n"
	"		int first = top;\n"
	"		if (second + thickness > height) {\n"
	"			second = height - thickness;\n"
	"			first = max(0, second - thickness * 2);\n"
	"		}\n"
	"		line = max(band(y, first, thickness), band(y, second, thickness));\n"
	"	} else if (style == 3) {\n"
	"		float amplitude = max(1.0, float(thickness) * 1.25);\n"
	"		float center = min(float(top) + amplitude,\n"
	"			float(height) - amplitude - float(thickness) / 2.0);\n"
	"		float wave = center + amplitude\n"
	"			* sin((float(x) + 0.5) * 6.2831853 / float(width));\n"
	"		float distance = abs(float(y) + 0.5 - wave);\n"
	"		line = clamp(float(thickness) / 2.0 + 0.75 - distance, 0.0, 1.0);\n"
	"	} else if (style == 4) {\n"
	"		int dot = max(1, thickness);\n"
	"		line = ((x / dot) % 2 == 0) ? band(y, top, thickness) : 0.0;\n"
	"	} else if (style == 5) {\n"
	"		int dash = max(2, width / 3);\n"
	"		line = (x < dash || x >= dash * 2) ? band(y, top, thickness) : 0.0;\n"
	"	}\n"
	"	line *= alpha;\n"
	"	if (uMode == 1 && glyphAlpha <= 0.0 && line <= 0.0) discard;\n"
	"	vec3 premultiplied = color(data.w) * line\n"
	"		+ glyphColor * (1.0 - line);\n"
	"	float total = line + glyphAlpha * (1.0 - line);\n"
	"	if (uMode == 2) {\n"
	"		premultiplied += color(data.z) * (1.0 - total);\n"
	"		total = 1.0;\n"
	"	}\n"
	"	fragColor = vec4(premultiplied.bgr, total);\n"
	"}\n";

static const char* const kSolidShader =
	"#version 130\n"
	"uniform vec4 uColor;\n"
	"out vec4 fragColor;\n"
	"void main() {\n"
	"	fragColor = vec4(uColor.bgr * uColor.a, uColor.a);\n"
	"}\n";

static const char* const kImageShader =
	"#version 130\n"
	"uniform sampler2D uImage;\n"
	"in vec2 vTexture;\n"
	"out vec4 fragColor;\n"
	"void main() {\n"
	"	vec4 pixel = texture(uImage, vTexture);\n"
	"	if (pixel.a <= 0.0) discard;\n"
	"	fragColor = vec4(pixel.bgr * pixel.a, pixel.a);\n"
	"}\n";


class GLRenderer : public TermRenderer {
public:
								GLRenderer();
	virtual						~GLRenderer();

			status_t			Init(const char** error);

	virtual	const char*			Name() const	{ return fName.c_str(); }
	virtual	bool				IsBroken() const	{ return fBroken; }

	virtual	bool				Render(const RenderFrame& frame,
									const GlyphAtlasInfo& atlas,
									uint8* bits, int32 bytesPerRow,
									int32& _firstRow, int32& _lastRow);

private:
			struct Program {
				GLuint	program;
				GLint	rect;
				GLint	viewport;
				GLint	textureRect;
			};

			struct ImageTexture {
				GLuint	texture;
				uint64	serial;
				int32	unusedFrames;
			};

			bool				_Compile(Program& program,
									const char* fragmentSource,
									const char** error);
			void				_UseProgram(Program& program);
			void				_SetRect(Program& program, float x, float y,
									float width, float height);
			bool				_EnsureTarget(int32 width, int32 height);
			void				_UploadAtlas(const GlyphAtlasInfo& atlas);
			void				_UploadCells(const RenderFrame& frame,
									int32 firstRow, int32 lastRow);
			void				_FillRect(float x, float y, float width,
									float height, uint32 color);
			void				_DrawCells(const RenderFrame& frame,
									const GlyphAtlasInfo& atlas, int32 mode,
									bool skipDefault, int32 top,
									int32 bottom);
			void				_DrawImages(const RenderFrame& frame,
									int32 top, int32 bottom, int32 minimumZ,
									int32 maximumZ);
			void				_DrawCursor(const RenderFrame& frame);
			GLuint				_ImageTexture(const ImageDraw& draw);

private:
			BGLView*			fView;
			std::string			fName;

			Program				fCellProgram;
			Program				fSolidProgram;
			Program				fImageProgram;
			GLint				fCellMode;
			GLint				fCellSkipDefault;
			GLint				fCellCellSize;
			GLint				fCellGrid;
			GLint				fCellSlots;
			GLint				fCellPageSize;
			GLint				fCellMetrics;
			GLint				fSolidColor;
			Program*			fCurrentProgram;

			GLuint				fFramebuffer;
			GLuint				fTarget;
			int32				fWidth;
			int32				fHeight;

			GLuint				fCells;
			int32				fCellColumns;
			int32				fCellRows;
			std::vector<uint32>	fCellData;

			std::vector<GLuint>	fAtlasTextures;
			std::vector<GLuint>	fColorTextures;	// 0 for pages without
			int32				fAtlasPageSize;

			std::map<const ImageFrame*, ImageTexture> fImages;

			PFNGLGETGRAPHICSRESETSTATUSPROC fGetResetStatus;
			bool				fBroken;
};


GLRenderer::GLRenderer()
	:
	fView(NULL),
	fCurrentProgram(NULL),
	fFramebuffer(0),
	fTarget(0),
	fWidth(0),
	fHeight(0),
	fCells(0),
	fCellColumns(0),
	fCellRows(0),
	fAtlasPageSize(0),
	fGetResetStatus(NULL),
	fBroken(false)
{
	memset(&fCellProgram, 0, sizeof(fCellProgram));
	memset(&fSolidProgram, 0, sizeof(fSolidProgram));
	memset(&fImageProgram, 0, sizeof(fImageProgram));
}


GLRenderer::~GLRenderer()
{
	if (fView != NULL) {
		fView->LockGL();
		glDeleteTextures(1, &fTarget);
		glDeleteTextures(1, &fCells);
		if (!fAtlasTextures.empty()) {
			glDeleteTextures(fAtlasTextures.size(), fAtlasTextures.data());
		}
		for (size_t i = 0; i < fColorTextures.size(); i++) {
			if (fColorTextures[i] != 0)
				glDeleteTextures(1, &fColorTextures[i]);
		}
		for (std::map<const ImageFrame*, ImageTexture>::iterator it
				= fImages.begin(); it != fImages.end(); it++) {
			glDeleteTextures(1, &it->second.texture);
		}
		if (fFramebuffer != 0)
			p_glDeleteFramebuffers(1, &fFramebuffer);
		fView->UnlockGL();
		delete fView;
	}
}


status_t
GLRenderer::Init(const char** error)
{
	fView = new(std::nothrow) BGLView(BRect(0, 0, 15, 15), "terminal gl",
		B_FOLLOW_NONE, 0, BGL_RGB | BGL_DOUBLE);
	if (fView == NULL) {
		*error = "no memory";
		return B_NO_MEMORY;
	}

	fView->LockGL();
	const char* renderer = (const char*)glGetString(GL_RENDERER);
	const char* version = (const char*)glGetString(GL_VERSION);
	if (renderer == NULL || version == NULL) {
		fView->UnlockGL();
		*error = "no OpenGL context";
		return B_ERROR;
	}
	fName = std::string("OpenGL: ") + renderer;

	int major = atoi(version);
	if (major < 3) {
		fView->UnlockGL();
		*error = "OpenGL 3.0 is needed";
		return B_NOT_SUPPORTED;
	}

#define LOAD(type, name) \
	p_##name = (type)fView->GetGLProcAddress(#name); \
	if (p_##name == NULL) { \
		fView->UnlockGL(); \
		*error = "missing " #name; \
		return B_NOT_SUPPORTED; \
	}
	GL_FUNCTIONS(LOAD)
#undef LOAD

	// to notice a lost GPU (optional)
	fGetResetStatus = (PFNGLGETGRAPHICSRESETSTATUSPROC)
		fView->GetGLProcAddress("glGetGraphicsResetStatus");
	if (fGetResetStatus == NULL) {
		fGetResetStatus = (PFNGLGETGRAPHICSRESETSTATUSPROC)
			fView->GetGLProcAddress("glGetGraphicsResetStatusARB");
	}

	if (!_Compile(fCellProgram, kCellShader, error)
		|| !_Compile(fSolidProgram, kSolidShader, error)
		|| !_Compile(fImageProgram, kImageShader, error)) {
		fView->UnlockGL();
		return B_ERROR;
	}

	GLuint program = fCellProgram.program;
	p_glUseProgram(program);
	p_glUniform1i(p_glGetUniformLocation(program, "uCells"), 0);
	p_glUniform1i(p_glGetUniformLocation(program, "uAtlas0"), 1);
	p_glUniform1i(p_glGetUniformLocation(program, "uAtlas1"), 2);
	p_glUniform1i(p_glGetUniformLocation(program, "uAtlas2"), 3);
	p_glUniform1i(p_glGetUniformLocation(program, "uAtlas3"), 4);
	for (int32 i = 0; i < 4; i++) {
		char name[16];
		snprintf(name, sizeof(name), "uColor%d", (int)i);
		p_glUniform1i(p_glGetUniformLocation(program, name), 6 + i);
	}
	fCellMode = p_glGetUniformLocation(program, "uMode");
	fCellSkipDefault = p_glGetUniformLocation(program, "uSkipDefault");
	fCellCellSize = p_glGetUniformLocation(program, "uCellSize");
	fCellGrid = p_glGetUniformLocation(program, "uGrid");
	fCellSlots = p_glGetUniformLocation(program, "uSlots");
	fCellPageSize = p_glGetUniformLocation(program, "uPageSize");
	fCellMetrics = p_glGetUniformLocation(program, "uMetrics");
	fSolidColor = p_glGetUniformLocation(fSolidProgram.program, "uColor");
	p_glUseProgram(fImageProgram.program);
	p_glUniform1i(p_glGetUniformLocation(fImageProgram.program, "uImage"),
		5);
	p_glUseProgram(0);

	// every draw is a rectangle: four corners, placed by the shader
	GLuint vertexArray;
	p_glGenVertexArrays(1, &vertexArray);
	p_glBindVertexArray(vertexArray);
	static const float kCorners[] = { 0, 0, 1, 0, 0, 1, 1, 1 };
	GLuint corners;
	p_glGenBuffers(1, &corners);
	p_glBindBuffer(GL_ARRAY_BUFFER, corners);
	p_glBufferData(GL_ARRAY_BUFFER, sizeof(kCorners), kCorners,
		GL_STATIC_DRAW);
	p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
	p_glEnableVertexAttribArray(0);

	p_glGenFramebuffers(1, &fFramebuffer);
	glGenTextures(1, &fTarget);
	glGenTextures(1, &fCells);

	glDisable(GL_DEPTH_TEST);
	glDisable(GL_DITHER);
	fView->UnlockGL();
	return B_OK;
}


bool
GLRenderer::_Compile(Program& program, const char* fragmentSource,
	const char** error)
{
	static char log[1024];
	GLuint shaders[2];
	const char* sources[2] = { kVertexShader, fragmentSource };
	GLenum types[2] = { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER };
	for (int i = 0; i < 2; i++) {
		shaders[i] = p_glCreateShader(types[i]);
		p_glShaderSource(shaders[i], 1, &sources[i], NULL);
		p_glCompileShader(shaders[i]);
		GLint status = 0;
		p_glGetShaderiv(shaders[i], GL_COMPILE_STATUS, &status);
		if (!status) {
			p_glGetShaderInfoLog(shaders[i], sizeof(log), NULL, log);
			*error = log;
			return false;
		}
	}

	program.program = p_glCreateProgram();
	p_glAttachShader(program.program, shaders[0]);
	p_glAttachShader(program.program, shaders[1]);
	p_glBindAttribLocation(program.program, 0, "aCorner");
	p_glLinkProgram(program.program);
	GLint status = 0;
	p_glGetProgramiv(program.program, GL_LINK_STATUS, &status);
	if (!status) {
		p_glGetProgramInfoLog(program.program, sizeof(log), NULL, log);
		*error = log;
		return false;
	}

	program.rect = p_glGetUniformLocation(program.program, "uRect");
	program.viewport = p_glGetUniformLocation(program.program, "uViewport");
	program.textureRect = p_glGetUniformLocation(program.program,
		"uTextureRect");
	return true;
}


void
GLRenderer::_UseProgram(Program& program)
{
	if (fCurrentProgram == &program)
		return;
	p_glUseProgram(program.program);
	p_glUniform2f(program.viewport, fWidth, fHeight);
	fCurrentProgram = &program;
}


void
GLRenderer::_SetRect(Program& program, float x, float y, float width,
	float height)
{
	p_glUniform4f(program.rect, x, y, width, height);
}


bool
GLRenderer::_EnsureTarget(int32 width, int32 height)
{
	if (width == fWidth && height == fHeight)
		return true;

	glBindTexture(GL_TEXTURE_2D, fTarget);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA,
		GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	p_glBindFramebuffer(GL_FRAMEBUFFER, fFramebuffer);
	p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		GL_TEXTURE_2D, fTarget, 0);
	if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER)
			!= GL_FRAMEBUFFER_COMPLETE) {
		return false;
	}
	fWidth = width;
	fHeight = height;
	fCurrentProgram = NULL;
	return true;
}


void
GLRenderer::_UploadAtlas(const GlyphAtlasInfo& atlas)
{
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

	bool recreate = atlas.pageSize != fAtlasPageSize;
	if (recreate && !fAtlasTextures.empty()) {
		glDeleteTextures(fAtlasTextures.size(), fAtlasTextures.data());
		fAtlasTextures.clear();
		for (size_t i = 0; i < fColorTextures.size(); i++) {
			if (fColorTextures[i] != 0)
				glDeleteTextures(1, &fColorTextures[i]);
		}
		fColorTextures.clear();
	}
	fAtlasPageSize = atlas.pageSize;

	for (int32 page = 0; page < atlas.pageCount && page < 4; page++) {
		bool created = false;
		if (page >= (int32)fAtlasTextures.size()) {
			GLuint texture;
			glGenTextures(1, &texture);
			glBindTexture(GL_TEXTURE_2D, texture);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, atlas.pageSize,
				atlas.pageSize, 0, GL_RED, GL_UNSIGNED_BYTE, atlas.pages[page]);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
				GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
				GL_CLAMP_TO_EDGE);
			fAtlasTextures.push_back(texture);
			created = true;
		}

		GlyphAtlasInfo::Dirty& dirty = atlas.dirty[page];
		if (!created && dirty.top < dirty.bottom) {
			glBindTexture(GL_TEXTURE_2D, fAtlasTextures[page]);
			glTexSubImage2D(GL_TEXTURE_2D, 0, 0, dirty.top, atlas.pageSize,
				dirty.bottom - dirty.top, GL_RED, GL_UNSIGNED_BYTE,
				atlas.pages[page] + (size_t)dirty.top * atlas.pageSize);
		}

		// the color glyphs of the page, B, G, R, A as the frame buffer
		if (fColorTextures.size() <= (size_t)page)
			fColorTextures.resize(page + 1, 0);
		const uint32* colorPage = atlas.colorPages != NULL
			? atlas.colorPages[page] : NULL;
		if (colorPage != NULL && fColorTextures[page] == 0) {
			glGenTextures(1, &fColorTextures[page]);
			glBindTexture(GL_TEXTURE_2D, fColorTextures[page]);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, atlas.pageSize,
				atlas.pageSize, 0, GL_RGBA, GL_UNSIGNED_BYTE, colorPage);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
				GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
				GL_CLAMP_TO_EDGE);
		} else if (colorPage != NULL && dirty.top < dirty.bottom) {
			glBindTexture(GL_TEXTURE_2D, fColorTextures[page]);
			glTexSubImage2D(GL_TEXTURE_2D, 0, 0, dirty.top, atlas.pageSize,
				dirty.bottom - dirty.top, GL_RGBA, GL_UNSIGNED_BYTE,
				colorPage + (size_t)dirty.top * atlas.pageSize);
		}
		dirty.top = dirty.bottom = 0;
	}

	for (size_t i = 0; i < fAtlasTextures.size() && i < 4; i++) {
		p_glActiveTexture(GL_TEXTURE1 + i);
		glBindTexture(GL_TEXTURE_2D, fAtlasTextures[i]);
	}
	for (size_t i = 0; i < 4; i++) {
		// pages without color glyphs have none
		GLuint texture = i < fColorTextures.size() ? fColorTextures[i] : 0;
		if (texture == 0 && !fAtlasTextures.empty())
			texture = fAtlasTextures[0];
		p_glActiveTexture(GL_TEXTURE6 + i);
		glBindTexture(GL_TEXTURE_2D, texture);
	}
	p_glActiveTexture(GL_TEXTURE0);
}


void
GLRenderer::_UploadCells(const RenderFrame& frame, int32 firstRow,
	int32 lastRow)
{
	glBindTexture(GL_TEXTURE_2D, fCells);
	bool recreate = frame.columns != fCellColumns || frame.rows != fCellRows;
	if (recreate) {
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32UI, frame.columns,
			frame.rows, 0, GL_RGBA_INTEGER, GL_UNSIGNED_INT, NULL);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		fCellColumns = frame.columns;
		fCellRows = frame.rows;
		firstRow = 0;
		lastRow = frame.rows - 1;
	}

	int32 count = lastRow - firstRow + 1;
	if (count <= 0)
		return;
	fCellData.resize((size_t)frame.columns * count * 4);
	uint32* data = fCellData.data();
	for (int32 row = firstRow; row <= lastRow; row++) {
		for (int32 column = 0; column < frame.columns; column++) {
			const RenderCell& cell = frame.Cell(row, column);
			*data++ = cell.glyph;
			*data++ = cell.foreground | ((uint32)cell.foregroundAlpha << 24);
			*data++ = cell.background | ((uint32)(cell.flags & 0xff) << 24);
			*data++ = cell.decoration | ((uint32)(cell.flags >> 8) << 24);
		}
	}
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, firstRow, frame.columns, count,
		GL_RGBA_INTEGER, GL_UNSIGNED_INT, fCellData.data());
}


void
GLRenderer::_FillRect(float x, float y, float width, float height,
	uint32 color)
{
	_UseProgram(fSolidProgram);
	p_glUniform4f(fSolidColor, ((color >> 16) & 0xff) / 255.0f,
		((color >> 8) & 0xff) / 255.0f, (color & 0xff) / 255.0f, 1.0f);
	_SetRect(fSolidProgram, x, y, width, height);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}


void
GLRenderer::_DrawCells(const RenderFrame& frame, const GlyphAtlasInfo& atlas,
	int32 mode, bool skipDefault, int32 top, int32 bottom)
{
	_UseProgram(fCellProgram);
	const RenderMetrics& metrics = frame.metrics;
	p_glUniform1i(fCellMode, mode);
	p_glUniform1i(fCellSkipDefault, skipDefault ? 1 : 0);
	p_glUniform2i(fCellCellSize, metrics.cellWidth, metrics.cellHeight);
	p_glUniform2i(fCellGrid, frame.columns, frame.rows);
	p_glUniform2i(fCellSlots, atlas.slotsPerRow, atlas.slotsPerPage);
	p_glUniform1f(fCellPageSize, atlas.pageSize);
	p_glUniform4i(fCellMetrics, metrics.underlinePosition,
		metrics.underlineThickness, metrics.strikePosition,
		metrics.lineThickness);
	_SetRect(fCellProgram, 0, top,
		std::min(frame.width, frame.columns * metrics.cellWidth),
		bottom - top);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}


GLuint
GLRenderer::_ImageTexture(const ImageDraw& draw)
{
	const ImageFrame* key = draw.frame.get();
	std::map<const ImageFrame*, ImageTexture>::iterator found
		= fImages.find(key);
	if (found != fImages.end() && found->second.serial == draw.frame->serial) {
		found->second.unusedFrames = 0;
		return found->second.texture;
	}

	GLuint texture;
	if (found != fImages.end())
		texture = found->second.texture;
	else
		glGenTextures(1, &texture);

	p_glActiveTexture(GL_TEXTURE5);
	glBindTexture(GL_TEXTURE_2D, texture);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, draw.imageWidth,
		draw.imageHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE,
		draw.frame->pixels.data());
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	p_glActiveTexture(GL_TEXTURE0);

	ImageTexture& entry = fImages[key];
	entry.texture = texture;
	entry.serial = draw.frame->serial;
	entry.unusedFrames = 0;
	return texture;
}


void
GLRenderer::_DrawImages(const RenderFrame& frame, int32 top, int32 bottom,
	int32 minimumZ, int32 maximumZ)
{
	std::vector<const ImageDraw*> draws;
	for (size_t i = 0; i < frame.images.size(); i++) {
		const ImageDraw& draw = frame.images[i];
		if (draw.z < minimumZ || draw.z > maximumZ || draw.frame == NULL
			|| draw.y >= bottom || draw.y + draw.height <= top
			|| draw.imageWidth <= 0 || draw.imageHeight <= 0) {
			continue;
		}
		draws.push_back(&draw);
	}
	if (draws.empty())
		return;
	std::stable_sort(draws.begin(), draws.end(),
		[](const ImageDraw* a, const ImageDraw* b) {
			return a->z < b->z || (a->z == b->z && a->imageId < b->imageId);
		});

	_UseProgram(fImageProgram);
	for (size_t i = 0; i < draws.size(); i++) {
		const ImageDraw& draw = *draws[i];
		GLuint texture = _ImageTexture(draw);
		p_glActiveTexture(GL_TEXTURE5);
		glBindTexture(GL_TEXTURE_2D, texture);
		p_glActiveTexture(GL_TEXTURE0);
		p_glUniform4f(fImageProgram.textureRect,
			draw.sourceX / draw.imageWidth, draw.sourceY / draw.imageHeight,
			(draw.sourceX + draw.sourceWidth) / draw.imageWidth,
			(draw.sourceY + draw.sourceHeight) / draw.imageHeight);
		_SetRect(fImageProgram, draw.x, draw.y, draw.width, draw.height);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	}
}


void
GLRenderer::_DrawCursor(const RenderFrame& frame)
{
	if (frame.cursorShape == RENDER_CURSOR_NONE
		|| frame.cursorShape == RENDER_CURSOR_BLOCK) {
		return;
	}

	const RenderMetrics& metrics = frame.metrics;
	float left = frame.cursorX * metrics.cellWidth;
	float top = frame.cursorY * metrics.cellHeight;
	float width = metrics.cellWidth * frame.cursorWidth;
	float height = metrics.cellHeight;
	float thickness = std::max((int32)2, metrics.lineThickness * 2);
	float line = metrics.lineThickness;
	uint32 color = frame.cursorColor;

	switch (frame.cursorShape) {
		case RENDER_CURSOR_UNDERLINE:
			_FillRect(left, top + height - thickness, width, thickness,
				color);
			break;
		case RENDER_CURSOR_BEAM:
			_FillRect(left, top, thickness, height, color);
			break;
		case RENDER_CURSOR_HOLLOW:
			_FillRect(left, top, width, line, color);
			_FillRect(left, top + height - line, width, line, color);
			_FillRect(left, top, line, height, color);
			_FillRect(left + width - line, top, line, height, color);
			break;
	}
}


bool
GLRenderer::Render(const RenderFrame& frame, const GlyphAtlasInfo& atlas,
	uint8* bits, int32 bytesPerRow, int32& _firstRow, int32& _lastRow)
{
	// the rows to draw, in bands of consecutive rows
	std::vector<std::pair<int32, int32> > bands;
	for (int32 row = 0; row < frame.rows; row++) {
		if (!frame.allDirty && !frame.dirtyRows[row])
			continue;
		if (!bands.empty() && bands.back().second == row - 1)
			bands.back().second = row;
		else
			bands.push_back(std::make_pair(row, row));
	}
	if (bands.empty() && !frame.allDirty)
		return false;
	if (fBroken)
		return false;

	bigtime_t start = system_time();
	fView->LockGL();
	if (!_EnsureTarget(frame.width, frame.height)) {
		fView->UnlockGL();
		return false;
	}
	p_glBindFramebuffer(GL_FRAMEBUFFER, fFramebuffer);
	glViewport(0, 0, fWidth, fHeight);
	fCurrentProgram = NULL;

	_UploadAtlas(atlas);
	if (!bands.empty())
		_UploadCells(frame, bands.front().first, bands.back().second);
	p_glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, fCells);

	const RenderMetrics& metrics = frame.metrics;
	int32 cellHeight = metrics.cellHeight;
	int32 gridWidth = std::min(frame.width, frame.columns * metrics.cellWidth);
	int32 gridBottom = std::min(frame.height, frame.rows * cellHeight);

	glEnable(GL_SCISSOR_TEST);
	glDisable(GL_BLEND);

	if (frame.allDirty) {
		// the margins right of and below the cells
		glScissor(0, 0, fWidth, fHeight);
		_FillRect(gridWidth, 0, frame.width - gridWidth, frame.height,
			frame.defaultBackground);
		_FillRect(0, gridBottom, gridWidth, frame.height - gridBottom,
			frame.defaultBackground);
	}

	for (size_t i = 0; i < bands.size(); i++) {
		int32 top = bands[i].first * cellHeight;
		int32 bottom = std::min(frame.height, (bands[i].second + 1)
			* cellHeight);
		if (top >= bottom)
			continue;
		glScissor(0, top, fWidth, bottom - top);

		bool images = false;
		for (size_t j = 0; j < frame.images.size(); j++) {
			const ImageDraw& draw = frame.images[j];
			if (draw.y < bottom && draw.y + draw.height > top) {
				images = true;
				break;
			}
		}

		glDisable(GL_BLEND);
		if (images) {
			_FillRect(0, top, gridWidth, bottom - top,
				frame.defaultBackground);
			glEnable(GL_BLEND);
			glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
			_DrawImages(frame, top, bottom, INT32_MIN, kBelowBackgroundsZ - 1);
			glDisable(GL_BLEND);
			_DrawCells(frame, atlas, 0, true, top, bottom);

			glEnable(GL_BLEND);
			_DrawImages(frame, top, bottom, kBelowBackgroundsZ, -1);
			_DrawCells(frame, atlas, 1, false, top, bottom);
			_DrawImages(frame, top, bottom, 0, INT32_MAX);
			glDisable(GL_BLEND);
		} else {
			// backgrounds and text in one pass
			_DrawCells(frame, atlas, 2, false, top, bottom);
		}

		if (frame.cursorY >= bands[i].first
			&& frame.cursorY <= bands[i].second) {
			_DrawCursor(frame);
		}
	}
	glDisable(GL_SCISSOR_TEST);

	// read back what was drawn
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	glPixelStorei(GL_PACK_ROW_LENGTH, bytesPerRow / 4);
	if (frame.allDirty) {
		glReadPixels(0, 0, fWidth, fHeight, GL_RGBA, GL_UNSIGNED_BYTE, bits);
	} else {
		for (size_t i = 0; i < bands.size(); i++) {
			int32 top = bands[i].first * cellHeight;
			int32 bottom = std::min(frame.height, (bands[i].second + 1)
				* cellHeight);
			if (top >= bottom)
				continue;
			glReadPixels(0, top, fWidth, bottom - top, GL_RGBA,
				GL_UNSIGNED_BYTE, bits + (size_t)top * bytesPerRow);
		}
	}
	glPixelStorei(GL_PACK_ROW_LENGTH, 0);

	// forget the textures of images no longer drawn
	for (std::map<const ImageFrame*, ImageTexture>::iterator it
			= fImages.begin(); it != fImages.end();) {
		if (++it->second.unusedFrames > 120) {
			glDeleteTextures(1, &it->second.texture);
			fImages.erase(it++);
		} else
			it++;
	}

	p_glBindFramebuffer(GL_FRAMEBUFFER, 0);

	// A lost GPU (a reset, or a driver that gave up on a channel) cannot
	// draw again, and neither can one that takes seconds a frame.
	GLenum error = glGetError();
	if ((fGetResetStatus != NULL && fGetResetStatus() != GL_NO_ERROR)
		|| error == GL_OUT_OF_MEMORY || error == 0x0507 /* CONTEXT_LOST */
		|| system_time() - start > 2000000) {
		fprintf(stderr, "Terminal: the OpenGL renderer stops (error 0x%x, "
			"%" B_PRId64 " ms)\n", error, (system_time() - start) / 1000);
		fBroken = true;
	}
	fView->UnlockGL();
	if (fBroken)
		return false;

	if (bands.empty()) {
		_firstRow = 0;
		_lastRow = std::max((int32)0, frame.rows - 1);
	} else {
		_firstRow = frame.allDirty ? 0 : bands.front().first;
		_lastRow = frame.allDirty ? frame.rows - 1 : bands.back().second;
	}
	return true;
}


// #pragma mark - entry


extern "C" __attribute__((visibility("default"))) TermRenderer*
create_terminal_renderer(const char** error)
{
	GLRenderer* renderer = new(std::nothrow) GLRenderer;
	if (renderer == NULL) {
		*error = "no memory";
		return NULL;
	}
	if (renderer->Init(error) != B_OK) {
		delete renderer;
		return NULL;
	}
	return renderer;
}
