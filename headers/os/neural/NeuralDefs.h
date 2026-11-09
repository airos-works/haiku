/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _NEURAL_DEFS_H
#define _NEURAL_DEFS_H


/*!	Neural Kit: neural-network inference on the processors a machine has
	for it: neural processing units (NPUs), graphics processors and the
	CPU. A BNeuralModel is a network read from a file, a BNeuralSession is
	that network loaded onto a BNeuralDevice and ready to run, a BTensor
	holds the numbers that go in and come out. The devices are found by
	back-end add-ons (add-ons/neural), one per kind of hardware; whatever a
	device cannot do itself runs on the CPU, so a session always gives the
	same answers, only faster or slower.
*/


#include <Errors.h>
#include <SupportDefs.h>


//! Most dimensions a tensor can have.
#define B_NEURAL_MAX_RANK			8


//! How one element of a tensor is stored.
enum neural_data_type {
	B_NEURAL_NO_TYPE = 0,
	B_NEURAL_FLOAT32 = 1,
	B_NEURAL_FLOAT16 = 2,
	B_NEURAL_INT8 = 3,
	B_NEURAL_UINT8 = 4,
	B_NEURAL_INT16 = 5,
	B_NEURAL_INT32 = 6,
	B_NEURAL_INT64 = 7,
	B_NEURAL_BOOL = 8
};


//! What kind of processor a device is.
enum neural_device_kind {
	B_NEURAL_DEVICE_CPU = 0,
	B_NEURAL_DEVICE_GPU = 1,
	B_NEURAL_DEVICE_NPU = 2
};


//! BNeuralDevice::Flags()
enum {
	//! runs networks of 8 bit quantized numbers
	B_NEURAL_DEVICE_QUANTIZED		= 0x0001,
	//! runs networks of floating point numbers
	B_NEURAL_DEVICE_FLOAT			= 0x0002,
	//! the device is not there all the time (USB, hot plug)
	B_NEURAL_DEVICE_REMOVABLE		= 0x0004,
	//! the kit's own CPU device: runs every operator the kit knows
	B_NEURAL_DEVICE_REFERENCE		= 0x0008
};


//! BNeuralRoster::GetDefaultDevice() and BNeuralSession::SetTo()
enum {
	//! the fastest device for the model (the default)
	B_NEURAL_PREFER_SPEED			= 0x0000,
	//! the device that takes the least power to run it
	B_NEURAL_PREFER_LOW_POWER		= 0x0001,
	//! fail instead of running operators on the CPU that the device
	//! cannot run
	B_NEURAL_NO_FALLBACK			= 0x0002,
	//! use the CPU only, whatever else the machine has
	B_NEURAL_CPU_ONLY				= 0x0004,
	//! keep per-operator times, see BNeuralSession::GetOperatorInfo()
	B_NEURAL_PROFILE				= 0x0008
};


//! Messages sent to the targets of BNeuralRoster::StartWatching().
enum {
	//! "device" (string): the identifier of the device, "name", "kind"
	B_NEURAL_DEVICE_ADDED			= 'NRda',
	B_NEURAL_DEVICE_REMOVED			= 'NRdr'
};

/*!	Sent by BNeuralSession::RunAsync() when the run is over, unless the
	caller gave a message of its own. Fields added to either one:
	"neural:status" (int32), "neural:token" (int32), "neural:session"
	(pointer: the BNeuralSession, to tell several apart; it may be gone by
	the time the message is read), "neural:duration" (int64,
	microseconds).
*/
#define B_NEURAL_RUN_COMPLETED		'NRrc'


#define B_NEURAL_ERROR_BASE			(B_GENERAL_ERROR_BASE + 0xb000)

enum {
	//! the file is not a model, or is damaged
	B_NEURAL_BAD_MODEL = B_NEURAL_ERROR_BASE,
	//! the model is of a format or version no loader knows
	B_NEURAL_UNKNOWN_FORMAT,
	//! the model has an operator the kit does not implement
	B_NEURAL_UNSUPPORTED_OPERATOR,
	//! a tensor has another type than the model asks for
	B_NEURAL_TYPE_MISMATCH,
	//! a tensor has another shape than the model asks for
	B_NEURAL_SHAPE_MISMATCH,
	//! no device of the kind asked for
	B_NEURAL_NO_DEVICE,
	//! the device went away or stopped answering; the session may be
	//! set up again on another one
	B_NEURAL_DEVICE_LOST,
	//! B_NEURAL_NO_FALLBACK was given and the device cannot run all of
	//! the model
	B_NEURAL_NOT_ACCELERATED
};


#ifdef __cplusplus
extern "C" {
#endif

//! Size of one element in bytes, 0 for an unknown type.
size_t		neural_data_type_size(neural_data_type type);
//! "float32", "uint8", ...
const char*	neural_data_type_name(neural_data_type type);
//! "CPU", "GPU", "NPU"
const char*	neural_device_kind_name(neural_device_kind kind);
/*!	What an error means, in words, for the kit's own errors
	(B_NEURAL_...) and, through strerror(), all others. */
const char*	neural_error_string(status_t error);

#ifdef __cplusplus
}
#endif


#endif	// _NEURAL_DEFS_H
