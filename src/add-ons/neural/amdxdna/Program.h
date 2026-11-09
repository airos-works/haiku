/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef XDNA_PROGRAM_H
#define XDNA_PROGRAM_H


#include <Locker.h>

#include <add-ons/neural/NeuralBackend.h>

#include "Bundle.h"
#include "Device.h"


/*!	A stretch of a network as a design on the NPU: a hardware context
	with the bundle's configuration, its instruction sequence and the
	buffers the design reads and writes. */
class Program : public BNeuralProgram {
public:
								Program(Device* device);
	virtual						~Program();

	//! Takes over \a bundle, also when it fails.
			status_t			Init(const BNeuralGraph& graph,
									const neural_partition& partition,
									Bundle* bundle);

	virtual	status_t			Run(void* const* tensors);
	virtual	bigtime_t			LastDeviceTime() const;

private:
			status_t			_CheckPartition(
									const neural_partition& partition) const;
			status_t			_Lost(const char* what, status_t error);

			BLocker				fLock;
			Device*				fDevice;
			bool				fAcquired;
			Bundle*				fBundle;
			const BNeuralGraph*	fGraph;

			uint32				fContext;
			bool				fHaveContext;
			Buffer				fConfiguration;
			Buffer				fInstructions;
			size_t				fInstructionSize;
			Buffer				fCommand;
			Buffer				fBuffers[XDNA_MAX_BUFFERS];
			int32				fTensors[XDNA_MAX_BUFFERS];

			bool				fLost;
			bigtime_t			fLastDeviceTime;
};


#endif	// XDNA_PROGRAM_H
