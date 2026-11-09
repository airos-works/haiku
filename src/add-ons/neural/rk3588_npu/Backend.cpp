/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	The Neural Kit's back end for the neural processor of the Rockchip
	RK3588, on top of the rk3588_npu driver.

	The processor does convolutions on 8 bit quantized numbers and can add
	a tensor to the result of one; that is what this back end takes from a
	network (see Support.cpp for exactly what), the rest stays on the CPU.
	How the processor is told what to do is taken from Mesa's "rocket"
	driver; see the other files for what is whose.
*/


#include <new>
#include <stdio.h>
#include <string.h>

#include <Message.h>
#include <add-ons/neural/NeuralBackend.h>

#include "Device.h"
#include "Program.h"
#include "Support.h"


class RK3588Backend : public BNeuralBackend {
public:
								RK3588Backend(image_id image);
	virtual						~RK3588Backend();

	virtual	const char*			Name() const;

	virtual	int32				CountDevices();
	virtual	status_t			GetDeviceInfo(int32 device,
									neural_device_info* info,
									BMessage* details);

	virtual	status_t			GetSupportedOperators(int32 device,
									const BNeuralGraph& graph,
									bool* supported);
	virtual	status_t			CreateProgram(int32 device,
									const BNeuralGraph& graph,
									const neural_partition& partition,
									uint32 flags,
									BNeuralProgram** _program);

private:
			bool				fFound;
			rk3588_npu_info		fInfo;
};


RK3588Backend::RK3588Backend(image_id image)
	:
	BNeuralBackend(image),
	fFound(false)
{
	memset(&fInfo, 0, sizeof(fInfo));
}


RK3588Backend::~RK3588Backend()
{
}


const char*
RK3588Backend::Name() const
{
	return "rk3588_npu";
}


int32
RK3588Backend::CountDevices()
{
	// Opening the node is what tells: the driver powers the processor up
	// for it and reads its version. It is closed again right away, so a
	// team that only lists devices leaves the processor off.
	rk3588_npu_info info;
	fFound = Device::Probe(&info) == B_OK;
	if (fFound)
		fInfo = info;
	return fFound ? 1 : 0;
}


status_t
RK3588Backend::GetDeviceInfo(int32 device, neural_device_info* info,
	BMessage* details)
{
	if (device != 0 || !fFound)
		return B_BAD_INDEX;

	if (info != NULL) {
		memset(info, 0, sizeof(*info));
		strlcpy(info->id, "0", sizeof(info->id));
		strlcpy(info->name, "Rockchip RK3588 NPU", sizeof(info->name));
		strlcpy(info->vendor, "Rockchip", sizeof(info->vendor));
		info->kind = B_NEURAL_DEVICE_NPU;
		info->flags = B_NEURAL_DEVICE_QUANTIZED;
		info->priority = 100;
	}

	if (details != NULL) {
		char version[16];
		snprintf(version, sizeof(version), "%#" B_PRIx32,
			fInfo.hardware_version);

		details->AddString("driver", "rk3588_npu");
		details->AddString("version", version);
		details->AddInt32("cores", fInfo.core_count);
		details->AddInt32("clock", fInfo.clock_rate);
		details->AddString("path", RK3588_NPU_DEVICE_PATH);
	}

	return B_OK;
}


status_t
RK3588Backend::GetSupportedOperators(int32 device, const BNeuralGraph& graph,
	bool* supported)
{
	if (device != 0)
		return B_BAD_INDEX;

	GraphUse use;
	status_t status = use.Init(graph);
	if (status != B_OK)
		return status;

	// In the order the operators run: whether an addition can be done
	// depends on the operators before it.
	for (int32 i = 0; i < graph.CountOperators(); i++) {
		const neural_graph_operator* op = graph.OperatorAt(i);
		Operation operation;
		supported[i] = false;

		switch (op->code) {
			case B_NEURAL_OP_CONV_2D:
			case B_NEURAL_OP_DEPTHWISE_CONV_2D:
				supported[i] = describe_convolution(graph, i, operation)
					== B_OK;
				break;

			case B_NEURAL_OP_ADD:
			{
				int32 convolution;
				int32 addend;
				supported[i] = find_addition_target(graph, use, i, supported,
						&convolution, &addend) == B_OK
					&& describe_convolution(graph, convolution, operation)
						== B_OK
					&& fuse_addition(graph, i, addend, operation) == B_OK;
				break;
			}

			default:
				break;
		}
	}

	return B_OK;
}


status_t
RK3588Backend::CreateProgram(int32 device, const BNeuralGraph& graph,
	const neural_partition& partition, uint32 flags,
	BNeuralProgram** _program)
{
	if (device != 0)
		return B_BAD_INDEX;

	Program* program = new(std::nothrow) Program();
	if (program == NULL)
		return B_NO_MEMORY;

	status_t status = program->Init(graph, partition);
	if (status != B_OK) {
		delete program;
		return status;
	}

	*_program = program;
	return B_OK;
}


extern "C" BNeuralBackend*
instantiate_neural_backend(image_id image)
{
	return new(std::nothrow) RK3588Backend(image);
}
