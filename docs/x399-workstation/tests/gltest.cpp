/* OpenGL test and benchmark for Haiku's BGLView.
 *
 * Prints which renderer the OpenGL kit loaded and what it reports, then draws
 * a rotating lit object for a while and reports the frame rate, and the CPU
 * time the drawing thread and the whole program used per frame (what
 * presenting costs the program; app_server's share is not in it).
 *
 * The window is a direct window, so that the renderer can put frames into the
 * screen's own frame buffer instead of sending them through the window system.
 * GLTEST_NO_DIRECT=1 uses an ordinary window instead, for comparison.
 * GLTEST_VSYNC=1 calls SwapBuffers(true), which waits for the display.
 *
 * To see what switching workspaces does to it, run switchws alongside. Do not
 * have the program switch its own: asking the window system to take your
 * window off the screen from inside your drawing loop deadlocks against the
 * window system waiting for you to say you have stopped drawing, and no real
 * program does it.
 *
 * usage: gltest [seconds] [mode]     (0 seconds only prints the strings)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <Application.h>
#include <DirectWindow.h>
#include <GLView.h>
#include <Window.h>
#include <OS.h>

#include <GL/gl.h>

// The window size, overridable so the cost of presenting can be seen to grow
// with it: GLTEST_SIZE="1920x1080".
static int kWidth = 800;
static int kHeight = 600;


class TestView : public BGLView {
public:
	TestView(BRect frame)
		:
		BGLView(frame, "gl", B_FOLLOW_ALL_SIDES, 0,
			BGL_RGB | BGL_DOUBLE | BGL_DEPTH)
	{
	}

	// A window that is resized gives its OpenGL view a bigger frame buffer, and
	// the program has to say what part of it to draw into. Without this the
	// picture stays the size the window started at, whatever the window does.
	void FrameResized(float width, float height) override
	{
		BGLView::FrameResized(width, height);

		LockGL();
		kWidth = (int)width;
		kHeight = (int)height;
		glViewport(0, 0, kWidth, kHeight);
		glMatrixMode(GL_PROJECTION);
		glLoadIdentity();
		const double kNear = 1.0, kFar = 100.0;
		double top = kNear * tan(45.0 * M_PI / 360.0);
		double right = top * kWidth / kHeight;
		glFrustum(-right, right, -top, top, kNear, kFar);
		glMatrixMode(GL_MODELVIEW);
		UnlockGL();
	}

	void PrintInfo()
	{
		LockGL();
		printf("vendor:     %s\n", glGetString(GL_VENDOR));
		printf("renderer:   %s\n", glGetString(GL_RENDERER));
		printf("version:    %s\n", glGetString(GL_VERSION));
		printf("shading:    %s\n", glGetString(GL_SHADING_LANGUAGE_VERSION));
		UnlockGL();
	}

	// A sphere as a stack of triangle strips: slices * stacks * 2 triangles,
	// with a normal per vertex so the lighting has something to do.
	static void DrawSphere(float radius, int slices, int stacks)
	{
		for (int i = 0; i < stacks; i++) {
			float phi0 = M_PI * i / stacks;
			float phi1 = M_PI * (i + 1) / stacks;
			glBegin(GL_TRIANGLE_STRIP);
			for (int j = 0; j <= slices; j++) {
				float theta = 2 * M_PI * j / slices;
				for (int k = 0; k < 2; k++) {
					float phi = k == 0 ? phi0 : phi1;
					float x = sinf(phi) * cosf(theta);
					float y = cosf(phi);
					float z = sinf(phi) * sinf(theta);
					glNormal3f(x, y, z);
					glVertex3f(x * radius, y * radius, z * radius);
				}
			}
			glEnd();
		}
	}

	// Draw a rotating sphere with a few thousand triangles, lit and depth
	// tested, for `seconds`, and return the frame rate.
	// mode 0: clear only, 1: clear plus a triangle, 2: the lit sphere
	int fMode = 2;
	// CPU used before the run, for the cost per frame
	bigtime_t fThreadCpu0 = 0, fTeamCpu0 = 0;

	// Moving the window while the GPU is drawing into the screen: the frame is
	// copied to where the window was when the clipping was last read, so a
	// window that moves in between is the case to watch.
	bool fMove = getenv("GLTEST_MOVE") != NULL;
	// Resizing changes the frame buffer, the texture behind it and the window's
	// clipping all at once, which is the other thing nobody had tried.
	bool fResize = getenv("GLTEST_RESIZE") != NULL;
	// Hiding and showing takes the window away from the screen entirely, which
	// is the other way the window system tells a direct window to stop.
	bool fHide = getenv("GLTEST_HIDE") != NULL;
	bool fVSync = getenv("GLTEST_VSYNC") != NULL;

	double Run(double seconds)
	{
		LockGL();

		if (fMode > 0) {
			glEnable(GL_DEPTH_TEST);
			glEnable(GL_LIGHTING);
			glEnable(GL_LIGHT0);
		}
		GLfloat position[4] = { 2.0f, 2.0f, 4.0f, 1.0f };
		glLightfv(GL_LIGHT0, GL_POSITION, position);
		glEnable(GL_COLOR_MATERIAL);
		glViewport(0, 0, kWidth, kHeight);
		glMatrixMode(GL_PROJECTION);
		glLoadIdentity();
		const double kNear = 1.0, kFar = 100.0;
		double top = kNear * tan(45.0 * M_PI / 360.0);
		double right = top * kWidth / kHeight;
		glFrustum(-right, right, -top, top, kNear, kFar);
		glMatrixMode(GL_MODELVIEW);

		UnlockGL();

		bigtime_t start = system_time();
		bigtime_t end = start + (bigtime_t)(seconds * 1000000);
		{
			thread_info ti;
			get_thread_info(find_thread(NULL), &ti);
			fThreadCpu0 = ti.user_time + ti.kernel_time;
			team_usage_info tu;
			get_team_usage_info(B_CURRENT_TEAM, B_TEAM_USAGE_SELF, &tu);
			fTeamCpu0 = tu.user_time + tu.kernel_time;
		}
		bigtime_t report = start + 1000000;
		int64 frames = 0;
		float angle = 0;
		while (system_time() < end) {
			LockGL();
			glClearColor(0.1f, 0.1f, 0.2f, 1.0f);
			glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
			if (fMode > 0) {
				glLoadIdentity();
				glTranslatef(0, 0, -6);
				glRotatef(angle, 0.3f, 1.0f, 0.1f);
				glColor3f(0.4f, 0.7f, 1.0f);
				if (fMode == 1) {
					glBegin(GL_TRIANGLES);
					glNormal3f(0, 0, 1);
					glVertex3f(-1, -1, 0);
					glVertex3f(1, -1, 0);
					glVertex3f(0, 1, 0);
					glEnd();
				} else {
					DrawSphere(1.8f, 64, 64);
				}
			}
			SwapBuffers(fVSync);
			UnlockGL();
			angle += 1.0f;
			frames++;

			if (fHide && (frames % 60) == 0) {
				BWindow *window = Window();
				if (window != NULL && window->LockWithTimeout(100000) == B_OK) {
					if (window->IsHidden())
						window->Show();
					else
						window->Hide();
					window->Unlock();
				}
			}

			if (fResize && (frames % 40) == 0) {
				BWindow *window = Window();
				if (window != NULL && window->LockWithTimeout(100000) == B_OK) {
					int step = (int)(frames / 40) % 6;
					window->ResizeTo(400 + step * 120, 300 + step * 90);
					window->Unlock();
				}
			}

			if (fMove && (frames % 40) == 0) {
				BWindow *window = Window();
				if (window != NULL && window->LockWithTimeout(100000) == B_OK) {
					int step = (int)(frames / 40) % 8;
					window->MoveTo(120 + step * 70, 90 + step * 55);
					window->Unlock();
				}
			}
			if (system_time() > report) {
				printf("  %" B_PRId64 " frames so far\n", frames);
				report += 1000000;
			}
		}
		double elapsed = (system_time() - start) / 1000000.0;
		{
			thread_info ti;
			get_thread_info(find_thread(NULL), &ti);
			team_usage_info tu;
			get_team_usage_info(B_CURRENT_TEAM, B_TEAM_USAGE_SELF, &tu);
			bigtime_t threadCpu = ti.user_time + ti.kernel_time - fThreadCpu0;
			bigtime_t teamCpu = tu.user_time + tu.kernel_time - fTeamCpu0;
			printf("CPU: drawing thread %.2f s (%.0f us/frame), whole team %.2f s (%.0f us/frame) over %.1f s\n",
				threadCpu / 1e6, (double)threadCpu / (frames ? frames : 1), teamCpu / 1e6,
				(double)teamCpu / (frames ? frames : 1), elapsed);
		}

		printf("%" B_PRId64 " frames in %.1f s: %.1f fps at %dx%d\n",
			frames, elapsed, frames / elapsed, kWidth, kHeight);
		return frames / elapsed;
	}
};


// A direct window hands the view the screen's frame buffer and the list of
// rectangles the window can be seen through; the renderer draws there itself.
class TestWindow : public BDirectWindow {
public:
	TestWindow(BRect frame)
		:
		BDirectWindow(frame, "gltest", B_TITLED_WINDOW,
			getenv("GLTEST_RESIZE") != NULL ? 0 : (B_NOT_RESIZABLE | B_NOT_ZOOMABLE))
	{
	}

	void DirectConnected(direct_buffer_info *info) override
	{
		if (getenv("GLTEST_TRACE") != NULL) {
			printf("window DirectConnected: state %#x, %" B_PRIu32 " clips,"
				" window %d,%d-%d,%d, first clip %d,%d-%d,%d\n",
				info->buffer_state, info->clip_list_count,
				(int)info->window_bounds.left, (int)info->window_bounds.top,
				(int)info->window_bounds.right, (int)info->window_bounds.bottom,
				info->clip_list_count > 0 ? info->clip_list[0].left : -1,
				info->clip_list_count > 0 ? info->clip_list[0].top : -1,
				info->clip_list_count > 0 ? info->clip_list[0].right : -1,
				info->clip_list_count > 0 ? info->clip_list[0].bottom : -1);
		}
		if (fView == NULL)
			return;
		fView->DirectConnected(info);
		fView->EnableDirectMode(true);
	}

	TestView *fView = NULL;
};


int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	double seconds = argc > 1 ? atof(argv[1]) : 5.0;
	int mode = argc > 2 ? atoi(argv[2]) : 2;

	const char *size = getenv("GLTEST_SIZE");
	if (size != NULL)
		sscanf(size, "%dx%d", &kWidth, &kHeight);
	int left = 80, top = 80;
	const char *pos = getenv("GLTEST_POS");
	if (pos != NULL)
		sscanf(pos, "%d,%d", &left, &top);

	BApplication app("application/x-vnd.x399-gltest");

	BRect frame(left, top, left + kWidth, top + kHeight);
	// GLTEST_FORCE_DIRECT=1 makes a direct window even where the display
	// mode does not say B_PARALLEL_ACCESS (amdgpu_kms before haiku#109).
	const bool direct = getenv("GLTEST_NO_DIRECT") == NULL
		&& (getenv("GLTEST_FORCE_DIRECT") != NULL
			|| BDirectWindow::SupportsWindowMode());
	BWindow *window;
	TestView *view;
	if (direct) {
		TestWindow *directWindow = new TestWindow(frame);
		view = new TestView(directWindow->Bounds());
		directWindow->fView = view;
		directWindow->AddChild(view);
		window = directWindow;
	} else {
		window = new BWindow(frame, "gltest", B_TITLED_WINDOW,
			getenv("GLTEST_RESIZE") != NULL ? 0 : (B_NOT_RESIZABLE | B_NOT_ZOOMABLE));
		view = new TestView(window->Bounds());
		window->AddChild(view);
	}
	printf("window: %s\n", direct ? "direct" : "ordinary");
	window->Show();
	snooze(500000);

	view->PrintInfo();
	view->fMode = mode;
	if (seconds > 0)
		view->Run(seconds);

	// Sit there without drawing, so that uncovering the window has to be
	// repaired by the renderer rather than by the next frame.
	const char *idle = getenv("GLTEST_IDLE");
	if (idle != NULL) {
		printf("idle for %s s\n", idle);
		snooze((bigtime_t)(atof(idle) * 1000000));
	}

	if (window->Lock())
		window->Quit();
	return 0;
}
