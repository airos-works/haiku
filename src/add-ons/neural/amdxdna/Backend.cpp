/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	The Neural Kit's back end for the NPU of AMD's Ryzen AI processors
	("XDNA"), on top of the amdxdna driver (Linux's, see the linux-drm
	repository).

	The NPU is an array of small processors and the wiring between them;
	it runs designs compiled for that array on a Linux host, not operators.
	So this back end takes from a network exactly the stretches it has a
	design bundle for (Bundle.h, README), and loads the bundle's design
	when a program is made.

	The node is not opened to find the device or to say what it supports:
	opening it loads firmware and powers the NPU up. It is opened for the
	first program and closed after the last (Device.h). Only root can:
	without an IOMMU driver a client of the NPU reaches all memory, and
	the driver lets nobody else in. For everybody else program creation
	fails and the kit computes on the CPU.

	NEURAL_AMDXDNA_BUNDLES=<directory> in the environment is looked into
	before the data directories; NEURAL_AMDXDNA_DEBUG=1 prints the key of
	every graph asked about and what became of it, which is how the key of
	a network is found out when a bundle is to be made for it.
*/


#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Autolock.h>
#include <Locker.h>
#include <Message.h>
#include <String.h>
#include <add-ons/neural/NeuralBackend.h>

#include "Bundle.h"
#include "Device.h"
#include "Program.h"


/*!	What was found out about a graph: asked again and again (to list
	devices, to pick one, to set a session up), answered once. */
struct GraphAnswer {
	const BNeuralGraph*	graph;
	BString				key;
	//! where bundles were looked for when the answer was made
	BString				override;
	//! the constants the digest was made of
	const void*			firstConstant;
	bool				supported;
	int32				first;
	int32				count;
};

static const int32 kAnswerCount = 8;


class XdnaBackend : public BNeuralBackend {
public:
								XdnaBackend(image_id image);
	virtual						~XdnaBackend();

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
			bool				_Lookup(const BNeuralGraph& graph,
									const char* key, int32* _first,
									int32* _count);

			BLocker				fLock;
			bool				fFound;
			Device				fDevice;
			GraphAnswer			fAnswers[kAnswerCount];
			int32				fNextAnswer;
};


/*!	The first constant a stretch reads: with the graph's address and its
	key, what an answer is remembered by. */
static const void*
first_constant(const BNeuralGraph& graph, int32 first, int32 count)
{
	for (int32 i = first; i < first + count; i++) {
		const neural_graph_operator* op = graph.OperatorAt(i);
		for (int32 j = 0; op != NULL && j < op->input_count; j++) {
			if (op->inputs[j] < 0)
				continue;
			const neural_graph_tensor* tensor = graph.TensorAt(op->inputs[j]);
			if (tensor != NULL && tensor->data != NULL)
				return tensor->data;
		}
	}
	return NULL;
}


XdnaBackend::XdnaBackend(image_id image)
	:
	BNeuralBackend(image),
	fLock("amdxdna back end"),
	fFound(false),
	fNextAnswer(0)
{
	for (int32 i = 0; i < kAnswerCount; i++)
		fAnswers[i].graph = NULL;
}


XdnaBackend::~XdnaBackend()
{
}


const char*
XdnaBackend::Name() const
{
	return "amdxdna";
}


int32
XdnaBackend::CountDevices()
{
	BAutolock locker(fLock);
	// The roster asks again when things may have changed: bundles too.
	for (int32 i = 0; i < kAnswerCount; i++)
		fAnswers[i].graph = NULL;

	fFound = Device::Exists();
	return fFound ? 1 : 0;
}


status_t
XdnaBackend::GetDeviceInfo(int32 device, neural_device_info* info,
	BMessage* details)
{
	if (device != 0 || !fFound)
		return B_BAD_INDEX;

	if (info != NULL) {
		memset(info, 0, sizeof(*info));
		strlcpy(info->id, "0", sizeof(info->id));
		strlcpy(info->name, "AMD XDNA NPU", sizeof(info->name));
		strlcpy(info->vendor, "AMD", sizeof(info->vendor));
		info->kind = B_NEURAL_DEVICE_NPU;
		info->flags = B_NEURAL_DEVICE_QUANTIZED;
		info->priority = 100;
	}

	// Firmware and array are known once a program had the node opened;
	// nothing is opened to tell.
	if (details != NULL)
		fDevice.GetDetails(details);
	return B_OK;
}


/*!	Whether there is a bundle for the graph that applies, and which
	operators it covers. */
bool
XdnaBackend::_Lookup(const BNeuralGraph& graph, const char* key,
	int32* _first, int32* _count)
{
	const char* override = getenv(XDNA_BUNDLE_ENVIRONMENT);
	if (override == NULL)
		override = "";

	BAutolock locker(fLock);
	for (int32 i = 0; i < kAnswerCount; i++) {
		GraphAnswer& answer = fAnswers[i];
		if (answer.graph != &graph || answer.key != key
			|| answer.override != override) {
			continue;
		}
		if (answer.supported && answer.firstConstant
				!= first_constant(graph, answer.first, answer.count)) {
			continue;
		}
		*_first = answer.first;
		*_count = answer.count;
		return answer.supported;
	}
	locker.Unlock();

	// The manifest, then the weights the bundle's stretch uses: only
	// those are read, and only when there is a bundle for the graph.
	Bundle bundle;
	const char* verdict = "no bundle";
	status_t status = bundle.Find(key);
	bool supported = false;
	if (status == B_OK) {
		char digest[65];
		if (bundle.Check(graph) != B_OK)
			verdict = "the bundle does not fit the network";
		else if (xdna_weights_digest(graph, bundle.FirstOperator(),
				bundle.CountOperators(), digest) != B_OK
			|| strcmp(digest, bundle.WeightsDigest()) != 0) {
			verdict = "the bundle is for other weights";
		} else {
			verdict = "bundle applies";
			supported = true;
		}
	} else if (status != B_ENTRY_NOT_FOUND)
		verdict = "the bundle's manifest is not understood";
	if (getenv("NEURAL_AMDXDNA_DEBUG") != NULL) {
		fprintf(stderr, "amdxdna.neural: graph %s: %s%s%s\n", key, verdict,
			status != B_ENTRY_NOT_FOUND ? " in " : "",
			status != B_ENTRY_NOT_FOUND ? bundle.Path() : "");
	}

	locker.Lock();
	GraphAnswer& answer = fAnswers[fNextAnswer];
	fNextAnswer = (fNextAnswer + 1) % kAnswerCount;
	answer.graph = &graph;
	answer.key = key;
	answer.override = override;
	answer.supported = supported;
	answer.first = supported ? bundle.FirstOperator() : 0;
	answer.count = supported ? bundle.CountOperators() : 0;
	answer.firstConstant = supported
		? first_constant(graph, answer.first, answer.count) : NULL;

	*_first = answer.first;
	*_count = answer.count;
	return supported;
}


status_t
XdnaBackend::GetSupportedOperators(int32 device, const BNeuralGraph& graph,
	bool* supported)
{
	if (device != 0)
		return B_BAD_INDEX;

	int32 opCount = graph.CountOperators();
	for (int32 i = 0; i < opCount; i++)
		supported[i] = false;

	// Most networks on most machines: no bundles anywhere, nothing to
	// compute.
	if (!xdna_have_bundle_directories())
		return B_OK;

	// What the network is made of, without its weights: quick for any
	// size of model.
	char key[33];
	if (xdna_graph_key(graph, key) != B_OK)
		return B_OK;

	int32 first;
	int32 count;
	if (_Lookup(graph, key, &first, &count)) {
		for (int32 i = first; i < first + count && i < opCount; i++)
			supported[i] = true;
	}
	return B_OK;
}


status_t
XdnaBackend::CreateProgram(int32 device, const BNeuralGraph& graph,
	const neural_partition& partition, uint32 flags,
	BNeuralProgram** _program)
{
	if (device != 0)
		return B_BAD_INDEX;

	char key[33];
	status_t status = xdna_graph_key(graph, key);
	if (status != B_OK)
		return status;

	// read anew: what the program is made of is checked in full
	Bundle* bundle = new(std::nothrow) Bundle;
	Program* program = new(std::nothrow) Program(&fDevice);
	if (bundle == NULL || program == NULL) {
		delete bundle;
		delete program;
		return B_NO_MEMORY;
	}
	status = bundle->Find(key);
	if (status != B_OK) {
		delete bundle;
		delete program;
		return status;
	}

	status = program->Init(graph, partition, bundle);
	if (status != B_OK) {
		// B_NO_MEMORY would end the session; the CPU can still do it
		delete program;
		return status == B_NO_MEMORY ? B_DEVICE_FULL : status;
	}

	*_program = program;
	return B_OK;
}


extern "C" BNeuralBackend*
instantiate_neural_backend(image_id image)
{
	return new(std::nothrow) XdnaBackend(image);
}
