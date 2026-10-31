/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	v3d_present_check: Mesa's summit_haiku_present_framebuffer() on the V3D
	(haiku#49), checked pixel by pixel.

	A framebuffer object with a known pattern is presented
	  1. into a stand-in for app_server's copy of the screen (an area of this
	     team, written by the CPU first, so its cache lines are dirty), with
	     rectangles at odd places and sizes, one partly outside the image:
	     every pixel inside is the pattern's, B, G, R, A, every pixel
	     outside still the sentinel;
	  2. into a 64x48 rectangle of the real frame buffer (rpi_display's),
	     whose 4 pixel border must not change;
	and then timed: a whole width x height frame into the stand-in.

	v3d_present_check [width height]   (the timed frame, 3840x2160)

	The frame buffer keeps what step 2 wrote until app_server draws there.

	Build against the GL sysroot of airos-ci's deps/build-gl.sh:
	  aarch64-unknown-haiku-g++ --sysroot=$GLROOT -O2 v3d_present_check.cpp \
	      -lEGL -lGLESv2 -o v3d_present_check */


#include <OS.h>
#include <Drivers.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <vector>


struct summit_haiku_present {
	uint32_t version;
	int32_t origin_x, origin_y;
	uint32_t rect_count;
	const int32_t* rects;
	uint32_t front_bytes_per_row;
	void* back_bits;
	uint32_t back_bytes_per_row;
	uint32_t back_width, back_height;
	uint32_t frames_in_flight;
	void (*completed)(void* cookie);
	void* cookie;
};

typedef int (*present_function)(unsigned, const summit_haiku_present*);
typedef void (*wait_function)(void);

static present_function sPresent;
static wait_function sWaitIdle;
static wait_function sRelease;
	// lets go of the imports: deleting an area still imported would wait
static int32 sCompleted;


static void
completed(void*)
{
	atomic_add(&sCompleted, 1);
}


static uint32
pattern(int x, int y)
{
	// R, G, B, A as the texture holds them
	uint8 r = (x * 7 + y * 3) & 0xff;
	uint8 g = (x * 5 + y) & 0xff;
	uint8 b = (y * 11 + x) & 0xff;
	uint8 a = 0xff - ((x + y) & 0x3f);
	return r | g << 8 | b << 16 | (uint32)a << 24;
}


static uint32
as_screen_pixel(uint32 rgba)
{
	// B, G, R, A in memory
	return ((rgba >> 16) & 0xff) | (rgba & 0xff00) | ((rgba & 0xff) << 16)
		| (rgba & 0xff000000);
}


static GLuint
make_framebuffer(int width, int height)
{
	std::vector<uint32> pixels((size_t)width * height);
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++)
			pixels[(size_t)y * width + x] = pattern(x, y);
	}
	GLuint texture, framebuffer;
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA,
		GL_UNSIGNED_BYTE, pixels.data());
	glGenFramebuffers(1, &framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
		texture, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		fprintf(stderr, "framebuffer incomplete\n");
		exit(1);
	}
	return framebuffer;
}


static bool
inside(const std::vector<int32>& rects, int x, int y, int originX, int originY,
	int imageWidth, int imageHeight)
{
	if (x < originX || y < originY || x >= originX + imageWidth
		|| y >= originY + imageHeight)
		return false;
	for (size_t i = 0; i < rects.size(); i += 4) {
		if (x >= rects[i] && x <= rects[i + 2] && y >= rects[i + 1]
			&& y <= rects[i + 3])
			return true;
	}
	return false;
}


static int
check_back(void)
{
	const int imageWidth = 301, imageHeight = 203;
	GLuint framebuffer = make_framebuffer(imageWidth, imageHeight);
	(void)framebuffer;

	const int width = 1000, height = 600;
	const uint32 bytesPerRow = 4004;
		// not a whole number of cache lines
	size_t size = (bytesPerRow * height + B_PAGE_SIZE - 1) & ~(B_PAGE_SIZE - 1);
	void* bits;
	area_id area = create_area("v3d_present_check back", &bits, B_ANY_ADDRESS,
		size, B_NO_LOCK, B_READ_AREA | B_WRITE_AREA);
	if (area < 0)
		return 1;
	const uint32 sentinel = 0x5aa5c33c;
	for (size_t i = 0; i < size / 4; i++)
		((uint32*)bits)[i] = sentinel;

	const int originX = 17, originY = 9;
	std::vector<int32> rects = {
		20, 11, 20 + 57 - 1, 11 + 33 - 1,		// odd place and size
		101, 50, 300, 199,
		250, 150, 400, 260,						// partly outside the image
		0, 0, 16, 300,							// left of the image
	};

	summit_haiku_present present = {};
	present.version = 1;
	present.origin_x = originX;
	present.origin_y = originY;
	present.rect_count = rects.size() / 4;
	present.rects = rects.data();
	present.back_bits = bits;
	present.back_bytes_per_row = bytesPerRow;
	present.back_width = width;
	present.back_height = height;
	present.completed = completed;
	int32 before = sCompleted;
	int result = sPresent(framebuffer, &present);
	sWaitIdle();
	printf("back: present %d, completed %d\n", result,
		(int)(sCompleted - before));
	if (result != 0)
		return 1;

	int wrong = 0, copied = 0;
	for (int y = 0; y < height; y++) {
		const uint32* row = (const uint32*)((uint8*)bits + y * bytesPerRow);
		for (int x = 0; x < width; x++) {
			uint32 expected = sentinel;
			if (inside(rects, x, y, originX, originY, imageWidth,
					imageHeight)) {
				expected = as_screen_pixel(pattern(x - originX, y - originY));
				copied++;
			}
			if (row[x] != expected) {
				if (wrong++ < 8) {
					printf("  %d,%d: %#08" B_PRIx32 ", expected %#08" B_PRIx32
						"\n", x, y, row[x], expected);
				}
			}
		}
	}
	printf("back: %d pixels copied, %d wrong\n", copied, wrong);
	sRelease();
	delete_area(area);
	return wrong != 0;
}


// rpi_display (headers/private/graphics/rpi_display)
#define RPI_DISPLAY_CLONE_FRAME_BUFFER (B_DEVICE_OP_CODES_END + 3)
#define RPI_DISPLAY_GET_STATE (B_DEVICE_OP_CODES_END + 4)
struct rpi_display_output {
	uint32 id, flags;
	uint16 native_width, native_height;
	int32 x, y;
	uint16 width, height, mode_width, mode_height, scale, render_scale;
	uint32 edid_length;
	uint8 edid[256];
};
struct rpi_display_shared_info {
	uint32 version, output_count;
	rpi_display_output outputs[2];
	uint32 width, height, bytes_per_row, generation;
	uint64 physical_address;
};


static int
check_front(void)
{
	int display = open("/dev/graphics/rpi_display/0", O_RDWR);
	rpi_display_shared_info info;
	area_info clone;
	if (display < 0 || ioctl(display, RPI_DISPLAY_GET_STATE, &info,
			sizeof(info)) != 0
		|| ioctl(display, RPI_DISPLAY_CLONE_FRAME_BUFFER, &clone,
			sizeof(clone)) != 0) {
		printf("front: no rpi_display frame buffer\n");
		return 1;
	}
	printf("front: %" B_PRIu32 "x%" B_PRIu32 ", %" B_PRIu32 " bytes a row\n",
		info.width, info.height, info.bytes_per_row);

	const int imageWidth = 64, imageHeight = 48;
	GLuint framebuffer = make_framebuffer(imageWidth, imageHeight);
	const int originX = info.width - 300 + 3, originY = info.height - 200 + 1;
	std::vector<int32> rects = {originX, originY, originX + imageWidth - 1,
		originY + imageHeight - 1};

	auto pixel = [&](int x, int y) {
		return ((volatile uint32*)((uint8*)clone.address
			+ (size_t)y * info.bytes_per_row))[x];
	};
	// the border, before
	std::vector<uint32> border;
	for (int y = originY - 4; y < originY + imageHeight + 4; y++) {
		for (int x = originX - 4; x < originX + imageWidth + 4; x++) {
			if (!inside(rects, x, y, originX, originY, imageWidth, imageHeight))
				border.push_back(pixel(x, y));
		}
	}

	summit_haiku_present present = {};
	present.version = 1;
	present.origin_x = originX;
	present.origin_y = originY;
	present.rect_count = 1;
	present.rects = rects.data();
	present.front_bytes_per_row = info.bytes_per_row;
	present.completed = completed;
	int result = sPresent(framebuffer, &present);
	sWaitIdle();
	printf("front: present %d\n", result);
	if (result != 0)
		return 1;

	int wrong = 0, changed = 0;
	size_t index = 0;
	for (int y = originY - 4; y < originY + imageHeight + 4; y++) {
		for (int x = originX - 4; x < originX + imageWidth + 4; x++) {
			if (!inside(rects, x, y, originX, originY, imageWidth,
					imageHeight)) {
				changed += pixel(x, y) != border[index++];
				continue;
			}
			uint32 expected = as_screen_pixel(pattern(x - originX,
				y - originY));
			if (pixel(x, y) != expected && wrong++ < 8) {
				printf("  %d,%d: %#08" B_PRIx32 ", expected %#08" B_PRIx32
					"\n", x, y, pixel(x, y), expected);
			}
		}
	}
	printf("front: %d wrong inside, %d border pixels changed\n", wrong,
		changed);
	delete_area(clone.area);
	close(display);
	return wrong != 0 || changed != 0;
}


static int
time_back(int width, int height)
{
	GLuint framebuffer = make_framebuffer(width, height);
	const uint32 bytesPerRow = width * 4;
	size_t size = ((size_t)bytesPerRow * height + B_PAGE_SIZE - 1)
		& ~(size_t)(B_PAGE_SIZE - 1);
	void* bits;
	area_id area = create_area("v3d_present_check timing", &bits,
		B_ANY_ADDRESS, size, B_NO_LOCK, B_READ_AREA | B_WRITE_AREA);
	if (area < 0)
		return 1;
	memset(bits, 0, size);

	std::vector<int32> rects = {0, 0, width - 1, height - 1};
	summit_haiku_present present = {};
	present.version = 1;
	present.rect_count = 1;
	present.rects = rects.data();
	present.back_bits = bits;
	present.back_bytes_per_row = bytesPerRow;
	present.back_width = width;
	present.back_height = height;
	present.frames_in_flight = 2;

	// the first imports the memory
	sPresent(framebuffer, &present);
	sWaitIdle();
	const int count = 20;
	bigtime_t start = system_time();
	for (int i = 0; i < count; i++) {
		if (sPresent(framebuffer, &present) != 0)
			return 1;
	}
	bigtime_t submitted = system_time();
	sWaitIdle();
	bigtime_t end = system_time();
	printf("timed: %dx%d, %.2f ms a frame (%.2f ms to submit), %.0f MB/s\n",
		width, height, (end - start) / 1000.0 / count,
		(submitted - start) / 1000.0 / count,
		(double)bytesPerRow * height * count / (end - start));

	uint32 first = ((uint32*)bits)[0];
	uint32 last = ((uint32*)((uint8*)bits + (size_t)(height - 1) * bytesPerRow))
		[width - 1];
	bool right = first == as_screen_pixel(pattern(0, 0))
		&& last == as_screen_pixel(pattern(width - 1, height - 1));
	printf("timed: corner pixels %s\n", right ? "right" : "WRONG");
	sRelease();
	delete_area(area);
	return right ? 0 : 1;
}


int
main(int argc, char** argv)
{
	// a step that takes the machine down is still on record
	setvbuf(stdout, NULL, _IONBF, 0);
	int width = argc > 2 ? atoi(argv[1]) : 3840;
	int height = argc > 2 ? atoi(argv[2]) : 2160;

	EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (!eglInitialize(display, NULL, NULL))
		return 1;
	eglBindAPI(EGL_OPENGL_ES_API);
	const EGLint configAttributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8, EGL_NONE};
	EGLConfig config;
	EGLint count = 0;
	if (!eglChooseConfig(display, configAttributes, &config, 1, &count)
		|| count == 0)
		return 1;
	const EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE};
	EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT,
		contextAttributes);
	const EGLint surfaceAttributes[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16,
		EGL_NONE};
	EGLSurface surface = eglCreatePbufferSurface(display, config,
		surfaceAttributes);
	if (!eglMakeCurrent(display, surface, surface, context))
		return 1;
	printf("renderer: %s\n", glGetString(GL_RENDERER));

	void* mesa = dlopen("libEGL_mesa.so.0", RTLD_NOW | RTLD_NOLOAD);
	if (mesa == NULL)
		mesa = dlopen("libEGL_mesa.so.0", RTLD_NOW);
	sPresent = mesa ? (present_function)dlsym(mesa,
		"summit_haiku_present_framebuffer") : NULL;
	sWaitIdle = mesa ? (wait_function)dlsym(mesa,
		"summit_haiku_present_wait_idle") : NULL;
	sRelease = mesa ? (wait_function)dlsym(mesa,
		"summit_haiku_present_release") : NULL;
	if (sPresent == NULL || sWaitIdle == NULL || sRelease == NULL) {
		printf("libEGL_mesa has no summit_haiku_present_*\n");
		return 1;
	}

	int failed = check_back();
	failed |= check_front();
	failed |= time_back(width, height);
	printf("%s\n", failed ? "FAILED" : "passed");

	sRelease();
	eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglTerminate(display);
	return failed;
}
