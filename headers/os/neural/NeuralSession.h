/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _NEURAL_SESSION_H
#define _NEURAL_SESSION_H


#include <Messenger.h>
#include <neural/NeuralDefs.h>
#include <neural/NeuralModel.h>
#include <neural/NeuralRoster.h>
#include <neural/Tensor.h>

namespace BPrivate {
	class NeuralSessionData;
}


//! BNeuralSession::GetRunInfo()
struct neural_run_info {
	//! runs since the session was set up
	int64				runs;
	//! time the last run took, in microseconds
	bigtime_t			last_duration;
	//! of that, time spent in the device's parts of the model
	bigtime_t			last_device_duration;
	//! mean over all runs
	bigtime_t			average_duration;
	//! operators the device runs itself
	int32				device_operators;
	//! operators the CPU runs
	int32				cpu_operators;
	//! stretches of operators handed to the device at once
	int32				device_partitions;
	/*!	Runs of a device partition that failed and were done by the CPU
		instead. A device that fails three times in a row, or says that
		it is gone, loses its partition to the CPU for good; the counts
		above then change. */
	int32				device_failures;
	uint32				_reserved[7];
};


//! BNeuralSession::GetOperatorInfo()
struct neural_operator_info {
	neural_operator_code code;
	//! true: runs on the session's device; false: on the CPU
	bool				on_device;
	//! the device partition the operator is part of, or -1
	int32				partition;
	/*!	Time of the last run in microseconds, kept with
		B_NEURAL_PROFILE: for an operator on the CPU its own time, for
		one on the device the time of its whole partition. */
	bigtime_t			last_duration;
	uint32				_reserved[4];
};


/*!	A model loaded onto a device and ready to run. Setting a session up
	is the slow part (the device's back end translates the network and
	uploads its weights); running it is fast and may be repeated as often
	as wanted.

		BNeuralModel model("/boot/home/models/mobilenet.tflite");
		BNeuralSession session(model);
		// fill session.InputAt(0) ...
		if (session.Run() == B_OK)
			int64 best = session.OutputAt(0)->IndexOfMaximum();

	The input and output tensors belong to the session and keep their
	memory between runs. A session runs one thing at a time; several
	sessions, also of the same model, may run at once and share a device.
*/
class BNeuralSession {
public:
								BNeuralSession();
								BNeuralSession(const BNeuralModel& model,
									uint32 flags = 0);
								BNeuralSession(const BNeuralModel& model,
									const BNeuralDevice& device,
									uint32 flags = 0);
	virtual						~BNeuralSession();

	//! On the device BNeuralRoster::GetDefaultDevice() picks.
			status_t			SetTo(const BNeuralModel& model,
									uint32 flags = 0);
			status_t			SetTo(const BNeuralModel& model,
									const BNeuralDevice& device,
									uint32 flags = 0);
			void				Unset();
			status_t			InitCheck() const;

			const BNeuralModel&	Model() const;
			const BNeuralDevice& Device() const;

			int32				CountInputs() const;
			BTensor*			InputAt(int32 index);
			BTensor*			FindInput(const char* name);

			int32				CountOutputs() const;
			const BTensor*		OutputAt(int32 index) const;
			const BTensor*		FindOutput(const char* name) const;

	/*!	Runs the network on what the input tensors hold and returns when
		the outputs are filled in. */
			status_t			Run();

	/*!	Starts a run and returns at once. When it is over, the message
		(B_NEURAL_RUN_COMPLETED when none is given) is sent to the target
		with the result in "neural:status". The inputs must be left alone
		and the outputs not read until then. B_BUSY while a run is going
		on. */
			status_t			RunAsync(const BMessenger& target,
									const BMessage* message = NULL,
									int32* _token = NULL);
	//! Waits for a run started by RunAsync() and returns its result.
			status_t			WaitForRun(
									bigtime_t timeout = B_INFINITE_TIMEOUT);
			bool				IsRunning() const;

			status_t			GetRunInfo(neural_run_info* info) const;
			status_t			GetOperatorInfo(int32 index,
									neural_operator_info* info) const;

private:
								BNeuralSession(const BNeuralSession&);
			BNeuralSession&		operator=(const BNeuralSession&);

	virtual	void				_ReservedNeuralSession1();
	virtual	void				_ReservedNeuralSession2();
	virtual	void				_ReservedNeuralSession3();
	virtual	void				_ReservedNeuralSession4();

			BPrivate::NeuralSessionData* fData;
			status_t			fStatus;
			uint32				_reserved[6];
};


#endif	// _NEURAL_SESSION_H
