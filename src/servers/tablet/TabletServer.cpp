/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	tablet_server: what the desktop does as a 2-in-1 laptop is folded.

	It follows the hinge (/dev/sensor/hinge/0, see <accelerometer.h>) and,
	as the Screen preferences say (<TabletModeSettings.h>):

	- turns the built-in panel's picture the right way up: folded into a tent
	  the panel is upside down, and a tablet is the right way up only when it
	  is held with the hinge at the bottom. In the stand, tent and tablet
	  modes the accelerometer behind the screen says which way is up; while
	  it cannot tell (the machine lies flat), a tent or a tablet is turned
	  and a stand is not. As a laptop the picture is always as it is.
	- starts air/OS Mobile (MobileShell, the touch shell) in place of the
	  desktop while the keyboard is out of reach, and quits it when the
	  machine is a laptop again, which puts the desktop back as it was.
	- with the desktop kept instead, has the touch screen scroll it as the
	  shell has it scroll its applications: while folded, it gives the
	  mobile_touch input_server filter the whole screen (a long press there
	  is the secondary click).

	On a machine without a hinge sensor it quits at once.
*/


#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

#include <Application.h>
#include <Entry.h>
#include <Message.h>
#include <Messenger.h>
#include <Roster.h>
#include <Screen.h>
#include <String.h>

#include <accelerometer.h>
#include <InterfacePrivate.h>
#include <TabletModeSettings.h>

#include "Protocol.h"


static const uint32 kMsgState = 'TMst';
	// "mode": uint32 hinge mode, "up": int32 (1 the screen's top is up,
	// -1 it is down, 0 cannot tell)

// While the machine is folded, how often the screen's accelerometer is
// read; as a laptop only the hinge is waited for.
static const bigtime_t kOrientationInterval = 300000;
// The screen's top must point this far up or down (mg, of 1000 for
// upright), and further than to the side, to count; and twice in a row.
static const int32 kOrientationThreshold = 550;
// Relaunches of a shell that died while it was wanted, per folding.
static const int32 kMaxShellRestarts = 3;


static const char* const kModeNames[] = {
	"unknown", "laptop", "stand", "tent", "tablet"
};


static const char*
mode_name(uint32 mode)
{
	return mode <= HINGE_MODE_TABLET ? kModeNames[mode] : "?";
}


class TabletServer : public BApplication {
public:
								TabletServer(const char* hingeFile,
									const char* accelerometerFile);
	virtual						~TabletServer();

	virtual	void				ReadyToRun();
	virtual	bool				QuitRequested();
	virtual	void				MessageReceived(BMessage* message);

private:
	static	status_t			_WatchThread(void* self);
			void				_Watch();
			bool				_ReadHinge(uint32& mode);
			bool				_ReadOrientation(int32& up);
			void				_FindDisplayAccelerometer();

			void				_Update();
			void				_SetRotation(int32 degrees);
			void				_StartShell();
			void				_QuitShell();
			void				_RestoreDesktop();
			bool				_ShellRunning() const;
			void				_SetDesktopTouch(bool on);

			BString				fHingeFile;		// for tests: text files
			BString				fAccelerometerFile;	// instead of devices
			int					fHinge;
			int					fAccelerometer;
			thread_id			fWatchThread;
	volatile bool				fQuitting;

			TabletModeSettings	fSettings;
			uint32				fMode;
			int32				fUp;
			int32				fRotation;		// as last set, -1 unknown
			bool				fRotationUnsupported;
			bool				fShellWanted;
			bool				fShellQuitAsked;
			team_id				fShellTeam;		// the shell started here
			int32				fShellRestarts;
			bool				fDesktopTouch;	// told the touch filter
};


TabletServer::TabletServer(const char* hingeFile, const char* accelerometerFile)
	:
	BApplication(TABLET_SERVER_SIGNATURE),
	fHingeFile(hingeFile),
	fAccelerometerFile(accelerometerFile),
	fHinge(-1),
	fAccelerometer(-1),
	fWatchThread(-1),
	fQuitting(false),
	fMode(HINGE_MODE_UNKNOWN),
	fUp(0),
	fRotation(-1),
	fRotationUnsupported(false),
	fShellWanted(false),
	fShellQuitAsked(false),
	fShellTeam(-1),
	fShellRestarts(0),
	fDesktopTouch(false)
{
}


TabletServer::~TabletServer()
{
	if (fHinge >= 0)
		close(fHinge);
	if (fAccelerometer >= 0)
		close(fAccelerometer);
}


void
TabletServer::ReadyToRun()
{
	if (fHingeFile.IsEmpty()) {
		fHinge = open(HINGE_DEVICE_PATH, O_RDONLY);
		if (fHinge < 0) {
			// not a 2-in-1 (or not one we know): nothing to do
			Quit();
			return;
		}
		_FindDisplayAccelerometer();
	}

	fSettings.Load();
	be_roster->StartWatching(be_app_messenger,
		B_REQUEST_LAUNCHED | B_REQUEST_QUIT);

	// A shell that did not get to put the desktop back (it crashed, the
	// power went) left what to restore: as a laptop, do it now.
	uint32 mode;
	if ((!_ReadHinge(mode) || !is_tablet_mode(mode)) && !_ShellRunning())
		_RestoreDesktop();

	fWatchThread = spawn_thread(_WatchThread, "hinge watcher",
		B_LOW_PRIORITY, this);
	if (fWatchThread < 0) {
		Quit();
		return;
	}
	resume_thread(fWatchThread);
}


bool
TabletServer::QuitRequested()
{
	fQuitting = true;
	if (fWatchThread >= 0) {
		// it waits for the hinge at most a few seconds
		status_t result;
		wait_for_thread(fWatchThread, &result);
		fWatchThread = -1;
	}
	be_roster->StopWatching(be_app_messenger);
	_SetDesktopTouch(false);
	return true;
}


void
TabletServer::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgState:
		{
			uint32 mode = message->GetUInt32("mode", HINGE_MODE_UNKNOWN);
			int32 up = message->GetInt32("up", 0);
			if (mode != fMode) {
				syslog(LOG_INFO, "tablet_server: %s\n", mode_name(mode));
				if (is_tablet_mode(mode) != is_tablet_mode(fMode))
					fShellRestarts = 0;
				// what way up it is when it cannot be told: a stand is
				// upright, a tent upside down, and a tablet held as one is
				// usually held, with the hinge at the top
				if (up == 0 && mode != fMode)
					fUp = mode == HINGE_MODE_STAND ? 1 : -1;
			}
			fMode = mode;
			if (up != 0)
				fUp = up;
			_Update();
			break;
		}

		case BPrivate::kMsgTabletSettingsChanged:
			fSettings.Load();
			_Update();
			break;

		case B_SOME_APP_LAUNCHED:
		case B_SOME_APP_QUIT:
		{
			const char* signature;
			if (message->FindString("be:signature", &signature) != B_OK
				|| strcasecmp(signature, MOBILE_SHELL_SIGNATURE) != 0)
				break;
			team_id team = message->GetInt32("be:team", -1);
			// only the shell started here, not a "MobileShell --restore" run
			// (nor one started by hand)
			if (message->what == B_SOME_APP_LAUNCHED || team != fShellTeam)
				break;
			fShellTeam = -1;
			bool asked = fShellQuitAsked;
			fShellQuitAsked = false;
			if (!asked && fShellWanted
				&& fShellRestarts++ < kMaxShellRestarts) {
				// it died: a new one carries on from what it saved
				syslog(LOG_WARNING, "tablet_server: MobileShell quit, "
					"starting it again\n");
				_StartShell();
				break;
			}
			if (!asked && fShellWanted) {
				syslog(LOG_ERR, "tablet_server: MobileShell keeps quitting, "
					"the desktop stays\n");
			}
			// Asked to quit, it put the desktop back itself; if it died,
			// what it saved does it.
			_RestoreDesktop();
			break;
		}

		default:
			BApplication::MessageReceived(message);
	}
}


status_t
TabletServer::_WatchThread(void* self)
{
	((TabletServer*)self)->_Watch();
	return B_OK;
}


/*!	Waits for the hinge, and while the machine is folded reads which way up
	the screen is. Tells the application thread what changed.
*/
void
TabletServer::_Watch()
{
	uint32 lastMode = ~(uint32)0;
	int32 lastUp = 0;
	int32 candidate = 0;
	int32 seen = 0;
	while (!fQuitting) {
		uint32 mode;
		if (!_ReadHinge(mode)) {
			snooze(1000000);
			continue;
		}

		int32 up = 0;
		if (is_tablet_mode(mode) && _ReadOrientation(up)) {
			// the same twice in a row
			if (up != candidate) {
				candidate = up;
				seen = 1;
			} else
				seen++;
			// Twice in a row; a mode that just changed takes the first
			// reading (the hinge has been still for most of a second),
			// so that what it does is not undone a moment later.
			up = seen >= 2 || mode != lastMode ? candidate : 0;
		} else
			candidate = seen = 0;

		if (mode != lastMode || (up != 0 && up != lastUp)) {
			BMessage state(kMsgState);
			state.AddUInt32("mode", mode);
			state.AddInt32("up", up);
			PostMessage(&state);
			lastMode = mode;
			if (up != 0)
				lastUp = up;
		}
		if (!is_tablet_mode(mode))
			lastUp = 0;

		// As a laptop nothing changes until the hinge does; folded, the
		// screen is looked at a few times a second.
		bigtime_t timeout = is_tablet_mode(mode) ? kOrientationInterval
			: 2000000;
		if (fHinge >= 0) {
			fd_set readable;
			FD_ZERO(&readable);
			FD_SET(fHinge, &readable);
			struct timeval wait;
			wait.tv_sec = timeout / 1000000;
			wait.tv_usec = timeout % 1000000;
			select(fHinge + 1, &readable, NULL, NULL, &wait);
		} else
			snooze(min_c(timeout, kOrientationInterval));
	}
}


bool
TabletServer::_ReadHinge(uint32& mode)
{
	if (fHinge >= 0) {
		hinge_state state;
		memset(&state, 0, sizeof(state));
		state.version = ACCELEROMETER_API_VERSION;
		if (ioctl(fHinge, HINGE_GET_STATE, &state, sizeof(state)) != 0)
			return false;
		mode = state.mode;
		return true;
	}

	// a test: the file has what the device reads as, "tablet 357"
	FILE* file = fopen(fHingeFile.String(), "r");
	if (file == NULL)
		return false;
	char name[32] = {};
	bool read = fscanf(file, "%31s", name) == 1;
	fclose(file);
	if (!read)
		return false;
	for (uint32 i = 0; i <= HINGE_MODE_TABLET; i++) {
		if (strcmp(name, kModeNames[i]) == 0) {
			mode = i;
			return true;
		}
	}
	return false;
}


bool
TabletServer::_ReadOrientation(int32& up)
{
	int32 x, y;
	if (fAccelerometer >= 0) {
		accelerometer_sample sample;
		memset(&sample, 0, sizeof(sample));
		if (ioctl(fAccelerometer, ACCELEROMETER_GET_SAMPLE, &sample,
				sizeof(sample)) != 0)
			return false;
		x = sample.x;
		y = sample.y;
	} else if (!fAccelerometerFile.IsEmpty()) {
		FILE* file = fopen(fAccelerometerFile.String(), "r");
		if (file == NULL)
			return false;
		int32 z;
		bool read = fscanf(file, "%" B_SCNd32 " %" B_SCNd32 " %" B_SCNd32,
			&x, &y, &z) == 3;
		fclose(file);
		if (!read)
			return false;
	} else
		return false;

	if (y > kOrientationThreshold && y > abs(x))
		up = 1;
	else if (y < -kOrientationThreshold && -y > abs(x))
		up = -1;
	else
		up = 0;
	return up != 0;
}


void
TabletServer::_FindDisplayAccelerometer()
{
	DIR* directory = opendir(ACCELEROMETER_DEVICE_DIRECTORY);
	if (directory == NULL)
		return;
	while (struct dirent* entry = readdir(directory)) {
		if (entry->d_name[0] == '.')
			continue;
		BString path;
		path << ACCELEROMETER_DEVICE_DIRECTORY "/" << entry->d_name;
		int fd = open(path.String(), O_RDONLY);
		if (fd < 0)
			continue;
		accelerometer_info info;
		memset(&info, 0, sizeof(info));
		info.version = ACCELEROMETER_API_VERSION;
		if (ioctl(fd, ACCELEROMETER_GET_INFO, &info, sizeof(info)) == 0
			&& info.location == ACCELEROMETER_LOCATION_DISPLAY) {
			fAccelerometer = fd;
			break;
		}
		close(fd);
	}
	closedir(directory);
}


/*!	Brings the picture and the shell in line with the mode and the
	settings.
*/
void
TabletServer::_Update()
{
	bool folded = is_tablet_mode(fMode);

	int32 rotation = 0;
	if (folded && fSettings.uprightRotation)
		rotation = fUp < 0 ? 180 : 0;
	_SetRotation(rotation);

	bool wanted = folded && fSettings.view == BPrivate::TABLET_VIEW_MOBILE;
	if (wanted && !fShellWanted)
		fShellRestarts = 0;
	fShellWanted = wanted;
	// the shell looks after the touch screen itself
	_SetDesktopTouch(folded && !wanted);
	if (wanted && !_ShellRunning())
		_StartShell();
	else if (!wanted && _ShellRunning())
		_QuitShell();
}


/*!	Has the touch screen scroll the desktop ( on), or not any more. */
void
TabletServer::_SetDesktopTouch(bool on)
{
	if (on == fDesktopTouch)
		return;

	port_id port = find_port(MOBILE_TOUCH_PORT);
	if (port < 0)
		return;

	mobile_hold_area area = {};
	if (on) {
		BRect frame = BScreen().Frame();
		area.enabled = 1;
		area.left = frame.left;
		area.top = frame.top;
		area.right = frame.right;
		area.bottom = frame.bottom;
		area.flags = kTouchScroll;
	}
	if (write_port_etc(port, kMobileHoldArea, &area, sizeof(area),
			B_RELATIVE_TIMEOUT, 100000) == B_OK) {
		fDesktopTouch = on;
	}
}


void
TabletServer::_SetRotation(int32 degrees)
{
	if (fRotationUnsupported && degrees != 0)
		return;

	// app_server may have been started again since: ask it how it is
	BMessage layout;
	if (BPrivate::get_display_layout(layout) == B_OK) {
		BMessage display;
		for (int32 i = 0; layout.FindMessage("display", i, &display) == B_OK;
				i++) {
			if (display.GetBool("internal", false)
				&& display.GetBool("connected", false)) {
				fRotation = display.GetInt32("rotation", 0);
				if (degrees != 0 && !display.GetBool("can rotate", false)) {
					fRotationUnsupported = true;
					syslog(LOG_INFO, "tablet_server: the built-in display "
						"cannot be turned upside down\n");
					return;
				}
				break;
			}
		}
	}
	if (fRotation == degrees)
		return;

	BMessage request;
	request.AddInt32("rotation", degrees);
	status_t status = BPrivate::set_display_layout(request);
	if (status == B_OK)
		fRotation = degrees;
	else {
		if (status == B_UNSUPPORTED || status == B_ENTRY_NOT_FOUND)
			fRotationUnsupported = true;
		syslog(LOG_WARNING, "tablet_server: could not turn the display to %"
			B_PRId32 " degrees: %s\n", degrees, strerror(status));
	}
}


void
TabletServer::_StartShell()
{
	team_id team = -1;
	status_t status = be_roster->Launch(MOBILE_SHELL_SIGNATURE,
		(BMessage*)NULL, &team);
	if (status == B_OK || status == B_ALREADY_RUNNING)
		fShellTeam = team;
	if (status != B_OK && status != B_ALREADY_RUNNING) {
		syslog(LOG_ERR, "tablet_server: could not start MobileShell: %s\n",
			strerror(status));
	}
}


void
TabletServer::_QuitShell()
{
	fShellQuitAsked = true;
	BMessenger(MOBILE_SHELL_SIGNATURE).SendMessage(B_QUIT_REQUESTED);
}


/*!	Has MobileShell put back what it changed of the desktop, from what it
	saved: when it did itself, there is nothing left to do.
*/
void
TabletServer::_RestoreDesktop()
{
	if (access(MOBILE_SHELL_PATH, X_OK) != 0)
		return;
	const char* args[] = { MOBILE_SHELL_PATH, "--restore", NULL };
	// not waited for: it has the windows to go through, and does nothing
	// when a shell runs
	thread_id thread = load_image(2, args, (const char**)environ);
	if (thread >= 0)
		resume_thread(thread);
}


bool
TabletServer::_ShellRunning() const
{
	return be_roster->IsRunning(MOBILE_SHELL_SIGNATURE);
}


// #pragma mark -


int
main(int argc, char** argv)
{
	const char* hingeFile = NULL;
	const char* accelerometerFile = NULL;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--hinge") == 0 && i + 1 < argc)
			hingeFile = argv[++i];
		else if (strcmp(argv[i], "--display-accelerometer") == 0
			&& i + 1 < argc)
			accelerometerFile = argv[++i];
		else {
			fprintf(stderr, "Usage: %s [--hinge <file> "
				"[--display-accelerometer <file>]]\n"
				"Follows the hinge of a 2-in-1 laptop. For tests, the files "
				"stand in for\nthe devices: \"tablet 357\" for the hinge, "
				"\"x y z\" for the screen's\naccelerometer.\n", argv[0]);
			return 1;
		}
	}

	TabletServer server(hingeFile, accelerometerFile);
	server.Run();
	return 0;
}
