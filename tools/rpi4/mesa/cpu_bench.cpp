/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	CPU cost of the GL driver on an EGL pbuffer, to compare Mesa builds that
	differ only in code generation (docs/rpi4/CPU-TUNING.md). Three parts,
	each the median of the rounds in milliseconds:
	  compile   link 24 programs that differ in a constant (run it with
	            MESA_SHADER_CACHE_DISABLE=true: the cache would hide them)
	  draw      100 frames of 400 one-triangle draws, a uniform change each
	  upload    16 uploads of a 1024x1024 RGBA texture (v3d tiles on the CPU)
	A final read back checks that the draws rendered.
	  rpi4_mesa_cpu_bench [rounds] */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <vector>

#include <OS.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>


static const int kSize = 256;


static GLuint
compile(GLenum type, const char* source)
{
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);
	GLint status = 0;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
	if (status == 0) {
		char log[512];
		glGetShaderInfoLog(shader, sizeof(log), NULL, log);
		fprintf(stderr, "shader: %s\n", log);
		exit(1);
	}
	return shader;
}


static GLuint
program(int variant)
{
	char vertex[512], fragment[1024];
	snprintf(vertex, sizeof(vertex),
		"attribute vec2 position;\n"
		"varying vec2 uv;\n"
		"void main() { uv = position * %d.5 + 0.5;\n"
		"  gl_Position = vec4(position, 0.0, 1.0); }\n", variant);
	snprintf(fragment, sizeof(fragment),
		"precision mediump float;\n"
		"uniform vec4 color;\n"
		"varying vec2 uv;\n"
		"void main() {\n"
		"  vec3 c = color.rgb;\n"
		"  for (int i = 0; i < 4; i++)\n"
		"    c = abs(sin(c * %d.25 + vec3(uv, float(i))));\n"
		"  gl_FragColor = vec4(mix(color.rgb, c, 0.%d), 1.0); }\n",
		variant + 1, variant % 10);
	GLuint p = glCreateProgram();
	glAttachShader(p, compile(GL_VERTEX_SHADER, vertex));
	glAttachShader(p, compile(GL_FRAGMENT_SHADER, fragment));
	glBindAttribLocation(p, 0, "position");
	glLinkProgram(p);
	GLint status = 0;
	glGetProgramiv(p, GL_LINK_STATUS, &status);
	if (status == 0) {
		fprintf(stderr, "link failed\n");
		exit(1);
	}
	return p;
}


static double
median(std::vector<double> values)
{
	std::sort(values.begin(), values.end());
	return values[values.size() / 2];
}


int
main(int argc, char** argv)
{
	const int rounds = argc > 1 ? atoi(argv[1]) : 5;

	EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	eglInitialize(display, NULL, NULL);
	const EGLint configAttributes[] = {
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
		EGL_NONE
	};
	EGLConfig config;
	EGLint count = 0;
	if (!eglChooseConfig(display, configAttributes, &config, 1, &count)
		|| count != 1) {
		fprintf(stderr, "eglChooseConfig failed\n");
		return 1;
	}
	const EGLint surfaceAttributes[] = {EGL_WIDTH, kSize, EGL_HEIGHT, kSize,
		EGL_NONE};
	EGLSurface surface = eglCreatePbufferSurface(display, config,
		surfaceAttributes);
	eglBindAPI(EGL_OPENGL_ES_API);
	const EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE};
	EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT,
		contextAttributes);
	if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT
		|| !eglMakeCurrent(display, surface, surface, context)) {
		fprintf(stderr, "no pbuffer context\n");
		return 1;
	}
	printf("renderer: %s\n", glGetString(GL_RENDERER));
	glViewport(0, 0, kSize, kSize);

	const GLfloat triangle[] = {-1, -1, 3, -1, -1, 3};
	GLuint buffer;
	glGenBuffers(1, &buffer);
	glBindBuffer(GL_ARRAY_BUFFER, buffer);
	glBufferData(GL_ARRAY_BUFFER, sizeof(triangle), triangle, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);

	std::vector<unsigned char> texels(1024 * 1024 * 4);
	for (size_t i = 0; i < texels.size(); i++)
		texels[i] = (unsigned char)(i * 7 + (i >> 12));
	GLuint texture;
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);

	std::vector<double> compileMs, drawMs, uploadMs;
	GLuint drawProgram = 0;
	for (int round = 0; round < rounds; round++) {
		bigtime_t start = system_time();
		GLuint programs[24];
		for (int i = 0; i < 24; i++)
			programs[i] = program(round * 24 + i);
		glFinish();
		compileMs.push_back((system_time() - start) / 1000.0);

		if (drawProgram == 0)
			drawProgram = programs[0];
		else {
			for (int i = 0; i < 24; i++)
				glDeleteProgram(programs[i]);
		}
		glUseProgram(drawProgram);
		GLint color = glGetUniformLocation(drawProgram, "color");
		start = system_time();
		for (int frame = 0; frame < 100; frame++) {
			for (int draw = 0; draw < 400; draw++) {
				glUniform4f(color, (draw & 7) / 8.0f, (frame & 3) / 4.0f,
					(draw & 1) ? 0.25f : 0.75f, 1.0f);
				glDrawArrays(GL_TRIANGLES, 0, 3);
			}
			glFlush();
		}
		glFinish();
		drawMs.push_back((system_time() - start) / 1000.0);

		start = system_time();
		for (int i = 0; i < 16; i++) {
			texels[i] = (unsigned char)(round + i);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1024, 1024, 0, GL_RGBA,
				GL_UNSIGNED_BYTE, texels.data());
		}
		glFinish();
		uploadMs.push_back((system_time() - start) / 1000.0);
	}

	unsigned char pixel[4] = {};
	glReadPixels(kSize / 2, kSize / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
	bool rendered = pixel[3] == 255 && (pixel[0] | pixel[1] | pixel[2]) != 0;
	printf("MESA_CPU_BENCH compile_ms=%.2f draw_ms=%.2f upload_ms=%.2f "
		"rounds=%d pixel=%u,%u,%u,%u %s\n", median(compileMs), median(drawMs),
		median(uploadMs), rounds, pixel[0], pixel[1], pixel[2], pixel[3],
		rendered ? "PASS" : "FAIL");
	return rendered && glGetError() == GL_NO_ERROR ? 0 : 1;
}
