/*
 * Copyright 2026, air/OS.
 * Copyright 2026 Renku Mobile contributors.
 * Distributed under the terms of the MIT License.
 */

/*	The mobile shell's keyboard: an input_server device add-on that types
	what the shell's on-screen keyboard writes to its port, as if a real
	keyboard had.

	It also tells the shell when a text field gets or loses the focus, so
	that the on-screen keyboard can come and go with it. Windows tell
	input_server so for its input methods (IS_FOCUS_IM_AWARE_VIEW and
	IS_UNFOCUS_IM_AWARE_VIEW), which passes that on to no one; the add-on
	runs inside input_server, so it watches for these messages with a filter.

	Without a shell that said hello it types nothing and tells no one:
	keys are taken from the team that said hello only, and that team has to
	be the mobile shell.

	It registers no device with input_server: there is nothing about it to
	set up, and the Input preferences would list it on every machine.
*/


#include "Protocol.h"

#include <Application.h>
#include <Autolock.h>
#include <InputServerDevice.h>
#include <InterfaceDefs.h>
#include <Locker.h>
#include <Message.h>
#include <MessageFilter.h>
#include <Messenger.h>
#include <OS.h>
#include <Roster.h>

#include <input/InputServerTypes.h>

#include <stdlib.h>
#include <string.h>


class MobileKeyboardDevice : public BInputServerDevice {
public:
								MobileKeyboardDevice();
	virtual						~MobileKeyboardDevice();

	virtual	status_t			InitCheck();

			void				FocusChanged(BMessage* message);

private:
	static	status_t			_Reader(void* data);
			void				_Hello(team_id sender, const char* buffer,
									ssize_t size);
			void				_Forget();
			void				_Modifiers(uint32 from, uint32 to);
			void				_Type(const mobile_key& key);

			port_id				fPort;
			thread_id			fThread;
			BMessageFilter*		fFocusFilter;

			BLocker				fLock;
			team_id				fShell;			// -1: none said hello
			BMessenger			fShellMessenger;
};


static MobileKeyboardDevice* sDevice;


static filter_result
focus_changed(BMessage* message, BHandler** target, BMessageFilter* filter)
{
	if ((message->what == IS_FOCUS_IM_AWARE_VIEW
			|| message->what == IS_UNFOCUS_IM_AWARE_VIEW) && sDevice != NULL) {
		sDevice->FocusChanged(message);
	}
	return B_DISPATCH_MESSAGE;
}


MobileKeyboardDevice::MobileKeyboardDevice()
	:
	fPort(-1),
	fThread(-1),
	fFocusFilter(NULL),
	fLock("mobile keyboard"),
	fShell(-1)
{
	sDevice = this;
	if (be_app->Lock()) {
		fFocusFilter = new BMessageFilter(B_ANY_DELIVERY, B_ANY_SOURCE,
			focus_changed);
		be_app->AddCommonFilter(fFocusFilter);
		be_app->Unlock();
	}
}


MobileKeyboardDevice::~MobileKeyboardDevice()
{
	if (fPort >= 0) {
		// Deleting the port ends the reader.
		delete_port(fPort);
		status_t result;
		wait_for_thread(fThread, &result);
	}
	if (fFocusFilter != NULL && be_app->Lock()) {
		be_app->RemoveCommonFilter(fFocusFilter);
		be_app->Unlock();
		delete fFocusFilter;
	}
	sDevice = NULL;
}


status_t
MobileKeyboardDevice::InitCheck()
{
	fPort = create_port(32, MOBILE_KEYBOARD_PORT);
	if (fPort < 0)
		return fPort;

	fThread = spawn_thread(_Reader, "mobile keyboard",
		B_URGENT_DISPLAY_PRIORITY, this);
	if (fThread < 0) {
		delete_port(fPort);
		fPort = -1;
		return fThread;
	}
	return resume_thread(fThread);
}


/*!	A window told input_server which kind of view has its focus now. Runs in
	input_server's main thread, with the window waiting for the reply: the
	shell is told without waiting for it.
*/
void
MobileKeyboardDevice::FocusChanged(BMessage* message)
{
	BAutolock _(fLock);
	if (fShell < 0)
		return;
	if (!fShellMessenger.IsValid()) {
		// the shell is gone without a word
		fShell = -1;
		fShellMessenger = BMessenger();
		return;
	}

	BMessage notice(kMsgTextFocus);
	notice.AddBool("text", message->what == IS_FOCUS_IM_AWARE_VIEW);
	BMessenger view;
	if (message->FindMessenger("view", &view) == B_OK)
		notice.AddInt32("team", view.Team());
	fShellMessenger.SendMessage(&notice, (BHandler*)NULL, 0);
}


status_t
MobileKeyboardDevice::_Reader(void* data)
{
	MobileKeyboardDevice* self = (MobileKeyboardDevice*)data;
	char buffer[1024];
	while (true) {
		port_message_info info;
		status_t status = get_port_message_info_etc(self->fPort, &info, 0, 0);
		if (status == B_INTERRUPTED)
			continue;
		if (status != B_OK)
			break;

		int32 code;
		ssize_t size = read_port_etc(self->fPort, &code, buffer,
			sizeof(buffer), B_RELATIVE_TIMEOUT, 0);
		if (size == B_INTERRUPTED || size == B_WOULD_BLOCK
			|| size == B_TIMED_OUT) {
			continue;
		}
		if (size < 0)
			break;

		switch (code) {
			case kMobileHello:
				self->_Hello(info.sender_team, buffer, size);
				break;

			case kMobileBye:
				if (info.sender_team == self->fShell)
					self->_Forget();
				break;

			case kMobileKey:
			{
				// whole, terminated keys from the shell only
				mobile_key key;
				if (info.sender_team != self->fShell
					|| size != (ssize_t)sizeof(key)) {
					break;
				}
				memcpy(&key, buffer, sizeof(key));
				if (memchr(key.bytes, '\0', sizeof(key.bytes)) != NULL)
					self->_Type(key);
				break;
			}
		}
	}
	return B_OK;
}


void
MobileKeyboardDevice::_Hello(team_id sender, const char* buffer, ssize_t size)
{
	// The shell says hello again now and then, in case input_server was
	// restarted under it.
	if (sender == fShell)
		return;

	app_info info;
	if (be_roster->GetRunningAppInfo(sender, &info) != B_OK
		|| strcmp(info.signature, MOBILE_SHELL_SIGNATURE) != 0) {
		return;
	}

	BMessage hello;
	BMessenger shell;
	if (hello.Unflatten(buffer) != B_OK
		|| hello.FindMessenger("shell", &shell) != B_OK
		|| shell.Team() != sender) {
		return;
	}

	BAutolock _(fLock);
	fShell = sender;
	fShellMessenger = shell;
}


void
MobileKeyboardDevice::_Forget()
{
	BAutolock _(fLock);
	fShell = -1;
	fShellMessenger = BMessenger();
}


void
MobileKeyboardDevice::_Modifiers(uint32 from, uint32 to)
{
	BMessage* message = new BMessage(B_MODIFIERS_CHANGED);
	message->AddInt64("when", system_time());
	message->AddInt32("be:old_modifiers", from);
	message->AddInt32("modifiers", to);
	if (EnqueueMessage(message) != B_OK)
		delete message;
}


void
MobileKeyboardDevice::_Type(const mobile_key& key)
{
	// input_server takes the modifiers every application sees from the last
	// key event, so they are pressed and let go around the key, as on a real
	// keyboard: otherwise they would stay down until the next key.
	if (key.modifiers != 0)
		_Modifiers(0, key.modifiers);

	for (int down = 1; down >= 0; down--) {
		BMessage* message = new BMessage(down ? B_KEY_DOWN : B_KEY_UP);
		message->AddInt64("when", system_time());
		message->AddInt32("key", key.key);
		message->AddInt32("modifiers", key.modifiers);
		int32 length = strlen(key.bytes);
		for (int32 i = 0; i < length; i++)
			message->AddInt8("byte", (int8)key.bytes[i]);
		message->AddData("bytes", B_STRING_TYPE, key.bytes, length + 1);
		message->AddInt32("raw_char", (uint8)key.raw & 0x7f);
		if (EnqueueMessage(message) != B_OK)
			delete message;
	}

	if (key.modifiers != 0)
		_Modifiers(key.modifiers, 0);
}


extern "C" BInputServerDevice*
instantiate_input_device()
{
	return new MobileKeyboardDevice();
}
