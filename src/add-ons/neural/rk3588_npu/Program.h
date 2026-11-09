/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef RK3588_NPU_PROGRAM_H
#define RK3588_NPU_PROGRAM_H


#include <pthread.h>

#include <add-ons/neural/NeuralBackend.h>

#include "Compiler.h"
#include "Device.h"


/*!	A stretch of a network on the processor: its weights, biases and
	command lists in one buffer that is written once, its tensors in
	another, and the list of tasks that a run hands to the driver as one
	job. Nothing of this is shared with other programs but the open
	device. */
class Program : public BNeuralProgram {
public:
								Program();
	virtual						~Program();

			status_t			Init(const BNeuralGraph& graph,
									const neural_partition& partition);

	virtual	status_t			Run(void* const* tensors);
	virtual	bigtime_t			LastDeviceTime() const;

private:
			struct Port {
				int32			tensor;
				int32			slot;
			};

			status_t			_Lower(const BNeuralGraph& graph,
									const neural_partition& partition);
			int32				_SlotFor(const BNeuralGraph& graph,
									int32 tensor);
			status_t			_Layout(uint32& constantsSize,
									uint32& tensorsSize);
			status_t			_Fill(const BNeuralGraph& graph);
			status_t			_MakePorts(const neural_partition& partition);

			void				_Store(const TensorSlot& slot,
									const uint8* source);
			void				_Load(const TensorSlot& slot, uint8* target);

			Device*				fDevice;
			DeviceBuffer		fConstants;
			DeviceBuffer		fTensors;

			Operation*			fOperations;
			int32				fOperationCount;
			TensorSlot*			fSlots;
			int32				fSlotCount;

			rk3588_npu_task*	fTasks;
			uint32				fTaskCount;

			Port*				fInputs;
			int32				fInputCount;
			Port*				fOutputs;
			int32				fOutputCount;

			pthread_mutex_t		fLock;
			bigtime_t			fDeviceTime;
};


#endif	// RK3588_NPU_PROGRAM_H
