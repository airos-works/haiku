/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Shared parts of bt_firmware: the USB HCI transport used while a controller
 * has no operational firmware yet, firmware file lookup, and the vendor
 * setup entry points.
 */
#ifndef BLUETOOTH_FIRMWARE_H
#define BLUETOOTH_FIRMWARE_H


#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <string>
#include <vector>


extern bool gVerbose;
// set when a controller was reset after a failed download; it comes back as
// a new USB device, ready to be tried again
extern bool gControllerReset;
static const int kExitRetryAfterReset = 4;


#define LOG(...) \
	do { \
		printf(__VA_ARGS__); \
		fflush(stdout); \
	} while (0)
#define TRACE(...) \
	do { \
		if (gVerbose) \
			LOG(__VA_ARGS__); \
	} while (0)
#define ERROR(...) \
	do { \
		fprintf(stderr, "bt_firmware: " __VA_ARGS__); \
		fflush(stderr); \
	} while (0)


static const unsigned kCommandTimeout = 2000;
static const unsigned kInitTimeout = 10000;
static const unsigned kDownloadTimeout = 5000;
static const unsigned kBootTimeout = 5000;
// Bounds each outgoing transfer; see Controller.
static const unsigned kTransferWatchdog = 10000;


struct Endpoint {
	int32_t	index;
	size_t	maxPacket;
};


// usb_raw keeps one lock per device across the wait of every transfer, so
// only one transfer can be outstanding at a time and a pending read cannot
// be abandoned: it only ends on data or a deadly signal. Reads therefore
// follow the controller's protocol exactly -- Intel Secure Send completions
// come on bulk IN, everything else on the interrupt endpoint -- and each wait
// is bounded by alarm(), whose default action ends this per-controller
// process.
class Controller {
public:
	Controller(int fd, const char* path);
	~Controller();

	bool		Start();

	bool		Command(uint16_t opcode, const void* parameters, size_t length,
					uint8_t* reply = NULL, size_t* replyLength = NULL,
					unsigned timeout = kCommandTimeout);
	bool		CommandExpecting(uint16_t opcode, const void* parameters,
					size_t length, uint8_t eventCode,
					std::vector<uint8_t>& event, unsigned timeout);
	// Sends a command and does not wait for any answer.
	bool		Post(uint16_t opcode, const void* parameters, size_t length);
	bool		SecureSend(uint8_t type, const uint8_t* data, size_t length);

	void		ClearVendorState();
	bool		WaitDownloadResult(unsigned timeout, uint8_t& result);
	bool		WaitBootup(unsigned timeout);

	const char*	Path() const { return fPath.c_str(); }

	// Intel bootloader mode: Secure Send goes over bulk OUT and is answered
	// on bulk IN.
	bool		bootloader;

private:
	bool		_FindEndpoints();
	bool		_Send(uint16_t opcode, const void* parameters, size_t length);
	bool		_ReadEvent(bool bulk, std::vector<uint8_t>& event,
					unsigned timeout);

	int			fFd;
	std::string	fPath;
	Endpoint	fInterruptIn;
	Endpoint	fBulkOut;
	Endpoint	fBulkIn;
	std::vector<uint8_t> fPending[2];

	bool		fDownloadDone;
	uint8_t		fDownloadResult;
	bool		fBooted;
};


// Firmware files live in <data directory>/firmware/<subdirectory>/<name>;
// directories given with --firmware-dir are searched first.
void InitFirmwareDirectories(const std::vector<std::string>& extra);
bool FindFirmware(const char* subdirectory, const char* name,
	std::string& path);
bool LoadFirmware(const char* subdirectory, const char* name,
	std::vector<uint8_t>& data, bool required);


namespace IntelBluetooth {
	struct UsbDevice;
}

bool SetupIntel(Controller& controller,
	const IntelBluetooth::UsbDevice* device, bool infoOnly);
bool SetupRealtek(Controller& controller, bool infoOnly);


#endif	// BLUETOOTH_FIRMWARE_H
