/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef SPEECH_OUTPUT_H
#define SPEECH_OUTPUT_H


#include <Locker.h>
#include <Messenger.h>
#include <OS.h>
#include <String.h>
#include <StringList.h>


/*!	Speaks through whatever text to speech the system has (haiku#52):
	 1. the speech_server (B_SPEECH_SERVER_SIGNATURE), if it runs or
	    launch_daemon can start it on demand;
	 2. an engine command: "speak", "espeak-ng", "espeak" or "flite" in PATH,
	    or the one named in the settings;
	 3. nothing: the screen reader then shows captions only.
*/
class SpeechOutput {
public:
								SpeechOutput(const char* engine,
									int32 rate);
								~SpeechOutput();

			void				Speak(const char* text, bool interrupt);
			void				Stop();
			void				WaitUntilDone(bigtime_t timeout);

			bool				HasEngine() const
									{ return fEngine != kEngineNone; }
			const BString&		EngineName() const { return fEngineName; }

private:
	enum {
		kEngineNone,
		kEngineServer,
		kEngineCommand
	};

			void				_Detect(const char* engine);
			bool				_FindCommand(const char* name,
									BString& path);
			team_id				_Launch(const BString& text);

	static	status_t			_ThreadEntry(void* data);
			void				_Thread();

			int					fEngine;
			BString				fEngineName;
			BString				fCommand;
			bool				fFlite;
			BMessenger			fServer;
			int32				fRate;

			BLocker				fLock;
			BStringList			fQueue;
			sem_id				fSem;
			thread_id			fThread;
			team_id				fCurrent;
			bool				fQuit;
};


#endif	// SPEECH_OUTPUT_H
