/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef JOB_PROCESS_H
#define JOB_PROCESS_H


#include <Locker.h>
#include <OS.h>

#include "Transcoder.h"


/*!	One conversion in a process of its own - this executable again, with
	--worker - as HandBrake runs its encodes: a graphics card that hangs
	(Mesa ends the process when the GPU is reset) or a decoder that crashes
	on a damaged file costs the job, not the window and its queue. The
	worker gets the job on its standard input and reports on its standard
	output, a line at a time; its input closing stops it, so it never
	outlives the window.

	Run() blocks like Transcoder::Run(); Cancel() may be called from any
	thread. */
class JobProcess {
public:
								JobProcess(const TranscodeJob& job);
								~JobProcess();

			status_t			Run(TranscodeListener* listener,
									BString& error);
			void				Cancel();

	const	TranscodeReport&	Report() const { return fReport; }
			// the worker ended without finishing: it crashed or was killed
			bool				Crashed() const { return fCrashed; }
			// the GPU's worker died and the CPU did the job
			bool				RetriedOnCpu() const { return fRetried; }

	// in the worker
	static	int					WorkerMain();

private:
			status_t			_RunOnce(TranscodeListener* listener,
									BString& error);

			TranscodeJob		fJob;
			TranscodeReport		fReport;
			bool				fCrashed;
			bool				fRetried;
			BLocker				fLock;
			pid_t				fChild;
			int					fInput;		// the worker's standard input
			bool				fCancelled;
			bigtime_t			fKillAt;	// after a cancel, if still there
};


#endif	// JOB_PROCESS_H
