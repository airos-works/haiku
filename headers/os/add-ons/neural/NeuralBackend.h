/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _NEURAL_BACKEND_H
#define _NEURAL_BACKEND_H


/*!	Neural Kit back ends: add-ons in add-ons/neural that make a kind of
	hardware usable for inference. A back end finds its devices (usually
	by opening the kernel driver's nodes under /dev/npu), says which
	operators of a network a device can run, and turns runs of such
	operators into a program for the device. The kit does the rest: it
	reads models, splits them between the device and the CPU, owns the
	tensors and runs the parts in order.

	A back end is one BNeuralBackend made by instantiate_neural_backend();
	it lives as long as the team's roster. It must be thread safe:
	programs of different sessions run at the same time.
*/


#include <image.h>
#include <neural/NeuralDefs.h>
#include <neural/NeuralOperators.h>

class BMessage;

namespace BPrivate {
	class NeuralGraphData;
}


/*!	A tensor of a network. Constants (weights) have their numbers in
	\c data; for the others \c data is NULL and the numbers exist only
	while the network runs. */
struct neural_graph_tensor {
	const char*			name;
	neural_data_type	type;
	int32				rank;
	int32				dimensions[B_NEURAL_MAX_RANK];
	//! real = scale * (stored - zero_point), when \c quantized
	bool				quantized;
	float				scale;
	int32				zero_point;
	/*!	Weights with one scale per slice along \c channel_axis (the zero
		point is 0 then); \c channel_count is 0 otherwise. */
	const float*		channel_scales;
	int32				channel_count;
	int32				channel_axis;
	const void*			data;
	size_t				size;
	uint32				_reserved[4];
};


/*!	An operator of a network: what it does, the indices of the tensors
	it reads and writes, and its parameters (the structure
	NeuralOperators.h names for the code, or NULL). */
struct neural_graph_operator {
	neural_operator_code code;
	const int32*		inputs;
	int32				input_count;
	const int32*		outputs;
	int32				output_count;
	const void*			params;
	uint32				_reserved[4];
};


/*!	A network as back ends see it. The operators are in an order in which
	they can be run one after the other. */
class BNeuralGraph {
public:
			const char*			Name() const;

			int32				CountTensors() const;
			const neural_graph_tensor* TensorAt(int32 index) const;

			int32				CountOperators() const;
			const neural_graph_operator* OperatorAt(int32 index) const;

			int32				CountInputs() const;
			int32				InputAt(int32 index) const;
			int32				CountOutputs() const;
			int32				OutputAt(int32 index) const;

private:
	friend class BPrivate::NeuralGraphData;

								BNeuralGraph(
									BPrivate::NeuralGraphData* data);
								~BNeuralGraph();
								BNeuralGraph(const BNeuralGraph&);
			BNeuralGraph&		operator=(const BNeuralGraph&);

			BPrivate::NeuralGraphData* fData;
			uint32				_reserved[4];
};


/*!	The stretch of a network a program is made for: operators that follow
	one another, the tensors they need from outside (\c inputs: made by
	earlier operators or given by the caller; constants are not listed)
	and the tensors they make that are needed outside (\c outputs). */
struct neural_partition {
	const int32*		operators;
	int32				operator_count;
	const int32*		inputs;
	int32				input_count;
	const int32*		outputs;
	int32				output_count;
	uint32				_reserved[4];
};


struct neural_device_info {
	//! unique within the back end and stable across restarts: "0"
	char				id[32];
	char				name[96];
	char				vendor[64];
	neural_device_kind	kind;
	//! B_NEURAL_DEVICE_QUANTIZED, ...
	uint32				flags;
	/*!	Order among devices of the same kind when the kit picks one for
		a program that has no preference: the higher, the earlier. Use
		100 for a device that is the reason the machine has an NPU, 50
		for a general purpose graphics processor. */
	int32				priority;
	uint32				_reserved[8];
};


/*!	A stretch of a network made ready for a device. */
class BNeuralProgram {
public:
								BNeuralProgram();
	virtual						~BNeuralProgram();

	/*!	Runs the program once. \a tensors has one entry per tensor of the
		graph; those of the partition's inputs and outputs point to
		memory of the tensor's size, in the graph's type and layout. The
		inputs are filled in, the outputs are to be written. The call
		returns when the outputs are complete. Return
		B_NEURAL_DEVICE_LOST when the device is gone for good: the kit
		then runs the stretch on the CPU from now on. */
	virtual	status_t			Run(void* const* tensors) = 0;

	/*!	Time the device itself took for the last Run() in microseconds,
		without copying in and out; 0 if not known. */
	virtual	bigtime_t			LastDeviceTime() const;

private:
	virtual	void				_ReservedNeuralProgram1();
	virtual	void				_ReservedNeuralProgram2();
	virtual	void				_ReservedNeuralProgram3();
	virtual	void				_ReservedNeuralProgram4();

			uint32				_reserved[8];
};


class BNeuralBackend {
public:
								BNeuralBackend(image_id image);
	virtual						~BNeuralBackend();

			image_id			Image() const { return fImage; }

	//! Short and unique, used in device identifiers: "rk3588_npu"
	virtual	const char*			Name() const = 0;

	/*!	Looks for devices; called once after the add-on is loaded and
		again for BNeuralRoster::Rescan(). A back end without devices on
		this machine returns 0 and costs nothing more. */
	virtual	int32				CountDevices() = 0;
	/*!	\a details may be NULL; see BNeuralDevice::GetInfo() for what to
		put in it. */
	virtual	status_t			GetDeviceInfo(int32 device,
									neural_device_info* info,
									BMessage* details) = 0;

	/*!	Sets supported[i] for every operator i of the graph the device
		can run. This is asked before anything is loaded and should be
		quick: look at codes, types and shapes only. */
	virtual	status_t			GetSupportedOperators(int32 device,
									const BNeuralGraph& graph,
									bool* supported) = 0;

	/*!	Makes the program for a stretch of supported operators. This is
		where weights are converted and uploaded; it may take a while.
		On an error the kit runs the stretch on the CPU instead. The
		graph outlives the program. \a flags are those of the session
		(B_NEURAL_PREFER_LOW_POWER, ...). */
	virtual	status_t			CreateProgram(int32 device,
									const BNeuralGraph& graph,
									const neural_partition& partition,
									uint32 flags,
									BNeuralProgram** _program) = 0;

private:
	virtual	void				_ReservedNeuralBackend1();
	virtual	void				_ReservedNeuralBackend2();
	virtual	void				_ReservedNeuralBackend3();
	virtual	void				_ReservedNeuralBackend4();
	virtual	void				_ReservedNeuralBackend5();
	virtual	void				_ReservedNeuralBackend6();

			image_id			fImage;
			uint32				_reserved[8];
};


/*!	The one function a back-end add-on exports. Return NULL when the
	back end cannot work at all here (the wrong machine). */
extern "C" _EXPORT BNeuralBackend* instantiate_neural_backend(
	image_id image);


#endif	// _NEURAL_BACKEND_H
