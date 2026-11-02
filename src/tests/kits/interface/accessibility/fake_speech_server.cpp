/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	Stands in for the speech_server of haiku#52 to test the screen reader's
	speech_server path: prints and logs what it is asked to say.
*/


#include <stdio.h>
#include <syslog.h>

#include <Application.h>

#include <AccessibilityProtocol.h>


class FakeSpeechServer : public BApplication {
public:
	FakeSpeechServer()
		:
		BApplication(B_SPEECH_SERVER_SIGNATURE)
	{
	}

	virtual void MessageReceived(BMessage* message)
	{
		switch (message->what) {
			case B_SPEECH_SPEAK:
				syslog(LOG_INFO, "fake_speech_server: speak%s: %s",
					message->GetBool("interrupt", false) ? " (interrupt)" : "",
					message->GetString("text", ""));
				printf("speak: %s\n", message->GetString("text", ""));
				fflush(stdout);
				break;
			case B_SPEECH_STOP:
				syslog(LOG_INFO, "fake_speech_server: stop");
				break;
			default:
				BApplication::MessageReceived(message);
				break;
		}
	}
};


int
main()
{
	FakeSpeechServer server;
	server.Run();
	return 0;
}
