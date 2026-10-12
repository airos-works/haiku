/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "HIDHost.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <Autolock.h>
#include <ByteOrder.h>
#include <DataIO.h>
#include <File.h>
#include <FindDirectory.h>
#include <Path.h>

#include <bluetooth/L2CAP/btL2CAP.h>
#include <bluetooth/SDPBuffer.h>
#include <bluetooth/SDPHIDRecord.h>
#include <bluetooth/sdp.h>
#include <bluetooth_hid.h>
#include <LELog.h>


using Bluetooth::LELog;
using Bluetooth::LEAddressString;
using Bluetooth::LE_LOG_DEBUG;
using Bluetooth::LE_LOG_ERROR;
using Bluetooth::LE_LOG_INFO;

#define LOG(level, format, args...) LELog(level, "hid", format, ##args)


static const uint16 kPSMControl = 0x0011;
static const uint16 kPSMInterrupt = 0x0013;

// How long a channel read waits before it looks whether the session ends:
// closing a channel does not wake a reader.
static const bigtime_t kReceiveTimeout = 1000000;
// How long a new session waits for the link to be encrypted before it
// trusts the device's reports.
static const bigtime_t kEncryptionTimeout = 10000000;

// The HIDP header (HID Profile 1.1, 3.1.1): the transaction type in the high
// nibble, its parameter in the low one.
enum {
	HIDP_HANDSHAKE			= 0x00,
	HIDP_HID_CONTROL		= 0x10,
	HIDP_SET_REPORT			= 0x50,
	HIDP_DATA				= 0xa0,
	HIDP_TYPE_MASK			= 0xf0,
	HIDP_PARAMETER_MASK		= 0x0f,

	HIDP_REPORT_INPUT		= 0x01,
	HIDP_REPORT_OUTPUT		= 0x02,
	HIDP_REPORT_FEATURE		= 0x03,

	HIDP_VIRTUAL_CABLE_UNPLUG = 0x05
};

// The boot protocol keyboard (HID 1.11 Appendix B.1) with the report ID 1 the
// Bluetooth HID Profile gives it (1.1, 3.3.2): what a keyboard sends after
// SET_PROTOCOL(Boot).
static const uint8 kBootKeyboardDescriptor[] = {
	0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x85, 0x01,
	0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00,
	0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
	0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
	0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01,
	0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03,
	0x91, 0x01,
	0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65,
	0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
	0xc0
};
static const uint8 kHIDPSetProtocolBoot = 0x70;

// SDP attributes (Bluetooth Assigned Numbers, SDP; HID Profile 1.1, 5.3)
static const char* kAttributeServiceName = "0x0100";
static const char* kAttributeVendorID = "0x0201";
static const char* kAttributeProductID = "0x0202";
static const char* kAttributeHIDReconnect = "0x0205";
static const char* kAttributeHIDDescriptors = "0x0206";
static const uint16 kServiceClassPnPInformation = 0x1200;


static void
format_address(const bdaddr_t& address, char buffer[18])
{
	LEAddressString(address.b, buffer);
}


struct HIDHost::ListenJob {
	HIDHost*	host;
	uint16		psm;
	int			index;
};


struct HIDHost::ConnectJob {
	HIDHost*	host;
	bdaddr_t	address;
};


// #pragma mark - Session


/*!	One connected HID device: its two channels and its bluetooth_hid device.
	It runs on its own threads and deletes itself when it ends.
*/
class HIDHost::Session {
public:
								Session(HIDHost* host,
									const bdaddr_t& address, int control,
									int interrupt);

			status_t			Start();
			void				Stop()
									{ atomic_set(&fStopping, 1); }
			thread_id			Thread() const { return fThread; }

private:
			bool				_Stopping() const
									{ return atomic_get(
										(int32*)&fStopping) != 0; }
			bool				_Receive(int socket, uint8* buffer,
									size_t size, ssize_t& _length);

	static	status_t			_Run(void* data);
			void				_Main();
			bool				_CreateDevice();
	static	status_t			_ControlThread(void* data);
			void				_ReadControl();
	static	status_t			_OutputThread(void* data);
			void				_WriteOutput();

private:
			HIDHost*			fHost;
			bdaddr_t			fAddress;
			char				fAddressString[18];
			BString				fName;
			int					fControl;
			int					fInterrupt;
			int					fKernel;
			thread_id			fThread;
			thread_id			fControlThread;
			thread_id			fOutputThread;
			int32				fStopping;
};


HIDHost::Session::Session(HIDHost* host, const bdaddr_t& address,
	int control, int interrupt)
	:
	fHost(host),
	fAddress(address),
	fControl(control),
	fInterrupt(interrupt),
	fKernel(-1),
	fThread(-1),
	fControlThread(-1),
	fOutputThread(-1),
	fStopping(0)
{
	format_address(address, fAddressString);

	timeval timeout;
	timeout.tv_sec = kReceiveTimeout / 1000000;
	timeout.tv_usec = kReceiveTimeout % 1000000;
	setsockopt(fControl, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	setsockopt(fInterrupt, SOL_SOCKET, SO_RCVTIMEO, &timeout,
		sizeof(timeout));
}


status_t
HIDHost::Session::Start()
{
	// Input reports wait for this thread only: keep it ahead of busy
	// applications, as the kernel's USB HID handling is.
	fThread = spawn_thread(&_Run, "bluetooth HID input",
		B_REAL_TIME_DISPLAY_PRIORITY, this);
	if (fThread < 0)
		return fThread;
	return resume_thread(fThread);
}


bool
HIDHost::Session::_Receive(int socket, uint8* buffer, size_t size,
	ssize_t& _length)
{
	while (!_Stopping()) {
		ssize_t length = recv(socket, buffer, size, 0);
		if (length > 0) {
			_length = length;
			return true;
		}
		if (length == 0)
			return false;
		if (errno == B_WOULD_BLOCK || errno == B_TIMED_OUT
			|| errno == B_INTERRUPTED)
			continue;
		LOG(LE_LOG_DEBUG, "%s: channel read failed: %s", fAddressString,
			strerror(errno));
		return false;
	}
	return false;
}


/*static*/ status_t
HIDHost::Session::_Run(void* data)
{
	Session* session = (Session*)data;
	session->_Main();
	return B_OK;
}


void
HIDHost::Session::_Main()
{
	// The reports are only trusted on an encrypted link: a device that only
	// pretends to have the address of a paired keyboard does not get there.
	bigtime_t deadline = system_time() + kEncryptionTimeout;
	bool encrypted = false;
	while (!_Stopping() && system_time() < deadline) {
		if (fHost->_IsEncrypted(fAddress)) {
			encrypted = true;
			break;
		}
		snooze(50000);
	}

	uint32 reports = 0;
	if (!encrypted) {
		if (!_Stopping()) {
			LOG(LE_LOG_ERROR, "%s: the link is not encrypted, not using its "
				"HID channels", fAddressString);
		}
	} else if (_CreateDevice()) {
		fControlThread = spawn_thread(&_ControlThread,
			"bluetooth HID control", B_NORMAL_PRIORITY, this);
		if (fControlThread >= 0)
			resume_thread(fControlThread);
		fOutputThread = spawn_thread(&_OutputThread, "bluetooth HID output",
			B_URGENT_DISPLAY_PRIORITY, this);
		if (fOutputThread >= 0)
			resume_thread(fOutputThread);

		LOG(LE_LOG_INFO, "%s: %s connected", fAddressString, fName.String());

		uint8 buffer[1 + BLUETOOTH_HID_MAX_REPORT];
		ssize_t length;
		bool reported = false;
		uint32 recent = 0;
		bigtime_t recentSince = system_time();
		while (_Receive(fInterrupt, buffer, sizeof(buffer), length)) {
			// Show that the device is in use, at most every 30 s.
			recent++;
			if (system_time() - recentSince >= 30000000) {
				LOG(LE_LOG_INFO, "%s: %" B_PRIu32 " reports in the last %"
					B_PRId64 " s", fAddressString, recent,
					(system_time() - recentSince) / 1000000);
				recent = 0;
				recentSince = system_time();
			} else if (recent == 1 && reports == 0) {
				LOG(LE_LOG_INFO, "%s: first input report (%zd bytes, id %u)",
					fAddressString, length - 1, length > 1 ? buffer[1] : 0);
			}
			if (buffer[0] != (HIDP_DATA | HIDP_REPORT_INPUT)) {
				LOG(LE_LOG_DEBUG, "%s: ignoring interrupt channel header "
					"%#x", fAddressString, buffer[0]);
				continue;
			}
			if (length < 2)
				continue;
			if (write(fKernel, buffer + 1, length - 1) < 0 && !reported) {
				LOG(LE_LOG_ERROR, "%s: report %u (%zd bytes) refused: %s",
					fAddressString, buffer[1], length - 1, strerror(errno));
				reported = true;
			}
			reports++;
		}
	}

	Stop();

	// Closing the kernel device wakes the output thread, and removes the
	// keyboard and mouse from the input server.
	if (fKernel >= 0)
		close(fKernel);
	status_t result;
	if (fOutputThread >= 0)
		wait_for_thread(fOutputThread, &result);
	if (fControlThread >= 0)
		wait_for_thread(fControlThread, &result);

	shutdown(fInterrupt, SHUT_RDWR);
	shutdown(fControl, SHUT_RDWR);
	close(fInterrupt);
	close(fControl);

	LOG(LE_LOG_INFO, "%s: disconnected after %" B_PRIu32 " reports",
		fAddressString, reports);

	fHost->_SessionEnded(this);
	delete this;
}


bool
HIDHost::Session::_CreateDevice()
{
	BMessage device;
	if (!fHost->_DeviceInfo(fAddress, device)) {
		LOG(LE_LOG_ERROR, "%s: not a known HID device", fAddressString);
		return false;
	}

	const void* descriptor;
	ssize_t descriptorSize;
	if (device.FindData("descriptor", B_RAW_TYPE, &descriptor,
			&descriptorSize) != B_OK || descriptorSize <= 0) {
		LOG(LE_LOG_ERROR, "%s: no report descriptor stored", fAddressString);
		return false;
	}
	fName = device.GetString("name", "Bluetooth HID device");

	fKernel = open(BLUETOOTH_HID_CONTROL_PATH, O_RDWR);
	if (fKernel < 0) {
		LOG(LE_LOG_ERROR, "%s: cannot open %s: %s", fAddressString,
			BLUETOOTH_HID_CONTROL_PATH, strerror(errno));
		return false;
	}

	bluetooth_hid_create create;
	memset(&create, 0, sizeof(create));
	create.version = BLUETOOTH_HID_VERSION;
	memcpy(create.address, fAddress.b, sizeof(create.address));
	create.vendor_id = device.GetUInt16("vendor", 0);
	create.product_id = device.GetUInt16("product", 0);
	create.descriptor_length = descriptorSize;
	create.descriptor = descriptor;
	strlcpy(create.name, fName.String(), sizeof(create.name));

	if (ioctl(fKernel, BLUETOOTH_HID_CREATE, &create, sizeof(create)) != 0) {
		LOG(LE_LOG_ERROR, "%s: the HID driver refused the device: %s",
			fAddressString, strerror(errno));
		return false;
	}

	if (device.GetBool("boot", false)) {
		// The device answers with a HANDSHAKE on the control channel.
		uint8 setProtocol = kHIDPSetProtocolBoot;
		if (send(fControl, &setProtocol, 1, 0) < 0) {
			LOG(LE_LOG_ERROR, "%s: SET_PROTOCOL(Boot) failed: %s",
				fAddressString, strerror(errno));
		}
	}
	return true;
}


/*static*/ status_t
HIDHost::Session::_ControlThread(void* data)
{
	((Session*)data)->_ReadControl();
	return B_OK;
}


void
HIDHost::Session::_ReadControl()
{
	uint8 buffer[64];
	ssize_t length;
	while (_Receive(fControl, buffer, sizeof(buffer), length)) {
		uint8 type = buffer[0] & HIDP_TYPE_MASK;
		uint8 parameter = buffer[0] & HIDP_PARAMETER_MASK;
		if (type == HIDP_HID_CONTROL
			&& parameter == HIDP_VIRTUAL_CABLE_UNPLUG) {
			LOG(LE_LOG_INFO, "%s: the device unplugged its virtual cable",
				fAddressString);
			break;
		}
		LOG(type == HIDP_HANDSHAKE ? LE_LOG_INFO : LE_LOG_DEBUG,
			"%s: control channel header %#x (%zd bytes)", fAddressString,
			buffer[0], length);
	}

	// Without its control channel the device is gone.
	Stop();
}


/*static*/ status_t
HIDHost::Session::_OutputThread(void* data)
{
	((Session*)data)->_WriteOutput();
	return B_OK;
}


void
HIDHost::Session::_WriteOutput()
{
	uint8 buffer[sizeof(bluetooth_hid_output) + BLUETOOTH_HID_MAX_REPORT];
	uint8 packet[1 + BLUETOOTH_HID_MAX_REPORT];
	while (!_Stopping()) {
		ssize_t length = read(fKernel, buffer, sizeof(buffer));
		if (length < (ssize_t)sizeof(bluetooth_hid_output)) {
			if (length < 0 && errno == B_INTERRUPTED)
				continue;
			break;
		}

		bluetooth_hid_output* header = (bluetooth_hid_output*)buffer;
		size_t reportLength = header->length;
		if (reportLength > length - sizeof(bluetooth_hid_output))
			continue;
		memcpy(packet + 1, buffer + sizeof(bluetooth_hid_output),
			reportLength);

		// Output reports (the LEDs) go on the interrupt channel like input
		// ones; feature reports are set over the control channel.
		int socket = fInterrupt;
		packet[0] = HIDP_DATA | HIDP_REPORT_OUTPUT;
		if (header->type == BLUETOOTH_HID_REPORT_FEATURE) {
			socket = fControl;
			packet[0] = HIDP_SET_REPORT | HIDP_REPORT_FEATURE;
		}
		if (send(socket, packet, 1 + reportLength, 0) < 0) {
			LOG(LE_LOG_DEBUG, "%s: sending a report failed: %s",
				fAddressString, strerror(errno));
		}
	}
}


// #pragma mark - HIDHost


HIDHost::HIDHost()
	:
	fLock("bluetooth HID host"),
	fQuitting(false)
{
	for (int i = 0; i < 2; i++) {
		fListenSockets[i] = -1;
		fListenThreads[i] = -1;
	}
}


HIDHost::~HIDHost()
{
	Stop();
}


status_t
HIDHost::Start()
{
	_Load();

	static const uint16 kPSMs[2] = { kPSMControl, kPSMInterrupt };
	for (int i = 0; i < 2; i++) {
		ListenJob* job = new(std::nothrow) ListenJob;
		if (job == NULL)
			return B_NO_MEMORY;
		job->host = this;
		job->psm = kPSMs[i];
		job->index = i;
		fListenThreads[i] = spawn_thread(&_ListenThread,
			i == 0 ? "bluetooth HID control listener"
				: "bluetooth HID interrupt listener",
			B_NORMAL_PRIORITY, job);
		if (fListenThreads[i] < 0) {
			delete job;
			return fListenThreads[i];
		}
		resume_thread(fListenThreads[i]);
	}
	return B_OK;
}


void
HIDHost::Stop()
{
	BAutolock locker(fLock);
	if (fQuitting)
		return;
	fQuitting = true;

	for (int i = 0; i < 2; i++) {
		if (fListenSockets[i] >= 0) {
			// Shutting a listening socket down wakes its accept().
			shutdown(fListenSockets[i], SHUT_RDWR);
		}
	}

	std::map<uint64, Pending>::iterator pending = fPending.begin();
	for (; pending != fPending.end(); pending++) {
		if (pending->second.control >= 0)
			close(pending->second.control);
		if (pending->second.interrupt >= 0)
			close(pending->second.interrupt);
	}
	fPending.clear();

	thread_id threads[16];
	int32 count = 0;
	std::map<uint64, Session*>::iterator session = fSessions.begin();
	for (; session != fSessions.end(); session++) {
		session->second->Stop();
		if (count < 16)
			threads[count++] = session->second->Thread();
	}
	locker.Unlock();

	status_t result;
	for (int32 i = 0; i < count; i++)
		wait_for_thread(threads[i], &result);
	for (int i = 0; i < 2; i++) {
		if (fListenThreads[i] >= 0)
			wait_for_thread(fListenThreads[i], &result);
	}
}


bool
HIDHost::IsHIDDevice(const bdaddr_t& address)
{
	BMessage device;
	return _DeviceInfo(address, device);
}


bool
HIDHost::HasDevices()
{
	BAutolock locker(fLock);
	return fDevices.HasMessage("device");
}


bool
HIDHost::AddDevice(const bdaddr_t& address, const BMessage& services,
	const char* name)
{
	char addressString[18];
	format_address(address, addressString);

	BMessage device;
	bool found = false;
	uint16 vendor = 0;
	uint16 product = 0;
	BMessage record;
	for (int32 i = 0; services.FindMessage("service", i, &record) == B_OK;
			i++) {
		if (Bluetooth::SDPHasServiceClass(record, kServiceClassPnPInformation)) {
			vendor = record.GetUInt16(kAttributeVendorID, 0);
			product = record.GetUInt16(kAttributeProductID, 0);
			continue;
		}
		if (found || !Bluetooth::SDPHasServiceClass(record,
				SDP_SERVICE_CLASS_HUMAN_INTERFACE_DEVICE))
			continue;

		const void* data;
		ssize_t size;
		const uint8* descriptor;
		size_t descriptorLength;
		if (record.FindData(kAttributeHIDDescriptors, B_RAW_TYPE, &data,
				&size) != B_OK
			|| !Bluetooth::SDPFindHIDReportDescriptor((const uint8*)data, size, descriptor,
				descriptorLength)) {
			LOG(LE_LOG_ERROR, "%s: its HID service has no report descriptor",
				addressString);
			continue;
		}

		BString serviceName = record.GetString(kAttributeServiceName, "");
		device.AddData("address", B_RAW_TYPE, address.b, sizeof(address.b));
		device.AddData("descriptor", B_RAW_TYPE, descriptor,
			descriptorLength);
		device.AddBool("reconnect",
			record.GetBool(kAttributeHIDReconnect, true));
		device.AddString("name", name != NULL && name[0] != '\0'
			? name : serviceName.String());
		found = true;
	}

	if (!found) {
		LOG(LE_LOG_DEBUG, "%s: no HID service", addressString);
		return false;
	}
	device.AddUInt16("vendor", vendor);
	device.AddUInt16("product", product);

	BAutolock locker(fLock);
	_RemoveStored(address);
	fDevices.AddMessage("device", &device);
	_Save();

	const void* descriptor;
	ssize_t descriptorSize = 0;
	device.FindData("descriptor", B_RAW_TYPE, &descriptor, &descriptorSize);
	LOG(LE_LOG_INFO, "%s: HID device \"%s\" (vendor %#06x, product %#06x, "
		"%zd-byte report descriptor, %s)", addressString,
		device.GetString("name", ""), vendor, product, descriptorSize,
		device.GetBool("reconnect", true)
			? "reconnects by itself" : "we reconnect it");
	return true;
}


bool
HIDHost::AddBootKeyboard(const bdaddr_t& address, const char* name)
{
	BMessage device;
	device.AddData("address", B_RAW_TYPE, address.b, sizeof(address.b));
	device.AddData("descriptor", B_RAW_TYPE, kBootKeyboardDescriptor,
		sizeof(kBootKeyboardDescriptor));
	device.AddBool("reconnect", true);
	device.AddBool("boot", true);
	device.AddString("name", name != NULL && name[0] != '\0'
		? name : "Bluetooth keyboard");
	device.AddUInt16("vendor", 0);
	device.AddUInt16("product", 0);

	BAutolock locker(fLock);
	_RemoveStored(address);
	fDevices.AddMessage("device", &device);
	_Save();

	char addressString[18];
	format_address(address, addressString);
	LOG(LE_LOG_INFO, "%s: keyboard \"%s\" without SDP records: boot protocol",
		addressString, device.GetString("name", ""));
	return true;
}


void
HIDHost::RemoveDevice(const bdaddr_t& address)
{
	BAutolock locker(fLock);
	if (_RemoveStored(address)) {
		_Save();
		char addressString[18];
		format_address(address, addressString);
		LOG(LE_LOG_INFO, "%s: HID device forgotten", addressString);
	}

	std::map<uint64, Session*>::iterator session
		= fSessions.find(_Key(address));
	if (session != fSessions.end())
		session->second->Stop();
}


void
HIDHost::Connect(const bdaddr_t& address)
{
	ConnectJob* job = new(std::nothrow) ConnectJob;
	if (job == NULL)
		return;
	job->host = this;
	job->address = address;

	thread_id thread = spawn_thread(&_ConnectThread, "bluetooth HID connect",
		B_NORMAL_PRIORITY, job);
	if (thread < 0 || resume_thread(thread) != B_OK)
		delete job;
}


void
HIDHost::LinkEncrypted(const bdaddr_t& address, bool encrypted,
	bool deviceInitiated)
{
	BAutolock locker(fLock);
	fEncrypted[_Key(address)] = encrypted;
	if (!encrypted || !deviceInitiated || fQuitting
		|| _FindStored(address, NULL) < 0)
		return;
	locker.Unlock();

	// A paired device that connected by itself should open its channels
	// now; a Keychron K4 does not (it waits and gives up after a minute).
	// If nothing came after a moment, open them from here.
	ConnectJob* job = new(std::nothrow) ConnectJob;
	if (job == NULL)
		return;
	job->host = this;
	job->address = address;
	thread_id thread = spawn_thread(&_ReconnectCheckThread,
		"bluetooth HID reconnect", B_NORMAL_PRIORITY, job);
	if (thread < 0 || resume_thread(thread) != B_OK)
		delete job;
}


/*static*/ status_t
HIDHost::_ReconnectCheckThread(void* data)
{
	ConnectJob* job = (ConnectJob*)data;
	HIDHost* host = job->host;
	bdaddr_t address = job->address;
	delete job;

	snooze(3000000);

	BAutolock locker(host->fLock);
	uint64 key = _Key(address);
	if (host->fQuitting
		|| host->fSessions.find(key) != host->fSessions.end()
		|| host->fPending.find(key) != host->fPending.end()
		|| host->fEncrypted.find(key) == host->fEncrypted.end())
		return B_OK;
	locker.Unlock();

	char addressString[18];
	format_address(address, addressString);
	LOG(LE_LOG_INFO, "%s: reconnected without opening its HID channels; "
		"opening them", addressString);
	host->_ConnectChannels(address);
	return B_OK;
}


void
HIDHost::LinkDown(const bdaddr_t& address)
{
	BAutolock locker(fLock);
	uint64 key = _Key(address);
	fEncrypted.erase(key);

	std::map<uint64, Pending>::iterator pending = fPending.find(key);
	if (pending != fPending.end()) {
		if (pending->second.control >= 0)
			close(pending->second.control);
		if (pending->second.interrupt >= 0)
			close(pending->second.interrupt);
		fPending.erase(pending);
	}

	std::map<uint64, Session*>::iterator session = fSessions.find(key);
	if (session != fSessions.end())
		session->second->Stop();
}


/*static*/ uint64
HIDHost::_Key(const bdaddr_t& address)
{
	uint64 key = 0;
	for (int i = 0; i < 6; i++)
		key |= (uint64)address.b[i] << (i * 8);
	return key;
}


/*static*/ status_t
HIDHost::_ListenThread(void* data)
{
	ListenJob* job = (ListenJob*)data;
	job->host->_Listen(job->psm, job->index);
	delete job;
	return B_OK;
}


void
HIDHost::_Listen(uint16 psm, int index)
{
	int listener = socket(PF_BLUETOOTH, SOCK_SEQPACKET, BLUETOOTH_PROTO_L2CAP);
	if (listener < 0) {
		LOG(LE_LOG_ERROR, "no L2CAP socket for PSM %#x: %s", psm,
			strerror(errno));
		return;
	}

	sockaddr_l2cap local;
	memset(&local, 0, sizeof(local));
	local.l2cap_len = sizeof(local);
	local.l2cap_family = AF_BLUETOOTH;
	local.l2cap_bdaddr = BDADDR_ANY;
	local.l2cap_psm = B_HOST_TO_LENDIAN_INT16(psm);
	if (bind(listener, (sockaddr*)&local, sizeof(local)) != 0
		|| listen(listener, 4) != 0) {
		LOG(LE_LOG_ERROR, "cannot listen on PSM %#x: %s", psm,
			strerror(errno));
		close(listener);
		return;
	}

	{
		BAutolock locker(fLock);
		if (fQuitting) {
			close(listener);
			return;
		}
		fListenSockets[index] = listener;
	}

	while (true) {
		sockaddr_l2cap peer;
		socklen_t peerLength = sizeof(peer);
		memset(&peer, 0, sizeof(peer));
		int channel = accept(listener, (sockaddr*)&peer, &peerLength);

		BAutolock locker(fLock);
		if (fQuitting) {
			if (channel >= 0)
				close(channel);
			break;
		}
		locker.Unlock();

		if (channel < 0) {
			snooze(100000);
			continue;
		}
		_ChannelOpened(psm, channel, peer.l2cap_bdaddr);
	}

	BAutolock locker(fLock);
	fListenSockets[index] = -1;
	close(listener);
}


/*static*/ status_t
HIDHost::_ConnectThread(void* data)
{
	ConnectJob* job = (ConnectJob*)data;
	job->host->_ConnectChannels(job->address);
	delete job;
	return B_OK;
}


void
HIDHost::_ConnectChannels(const bdaddr_t& address)
{
	char addressString[18];
	format_address(address, addressString);
	{
		BAutolock locker(fLock);
		if (fSessions.find(_Key(address)) != fSessions.end()
			|| fPending.find(_Key(address)) != fPending.end())
			return;
	}

	// The control channel first (HID Profile 1.1, 7.4.1).
	int control = _OpenChannel(address, kPSMControl);
	if (control < 0)
		return;
	int interrupt = _OpenChannel(address, kPSMInterrupt);
	if (interrupt < 0) {
		shutdown(control, SHUT_RDWR);
		close(control);
		return;
	}
	_StartSession(address, control, interrupt);
}


int
HIDHost::_OpenChannel(const bdaddr_t& address, uint16 psm)
{
	char addressString[18];
	format_address(address, addressString);

	int channel = socket(PF_BLUETOOTH, SOCK_SEQPACKET, BLUETOOTH_PROTO_L2CAP);
	if (channel < 0) {
		LOG(LE_LOG_ERROR, "%s: no L2CAP socket: %s", addressString,
			strerror(errno));
		return -1;
	}

	sockaddr_l2cap remote;
	memset(&remote, 0, sizeof(remote));
	remote.l2cap_len = sizeof(remote);
	remote.l2cap_family = AF_BLUETOOTH;
	remote.l2cap_bdaddr = address;
	remote.l2cap_psm = B_HOST_TO_LENDIAN_INT16(psm);
	if (connect(channel, (sockaddr*)&remote, sizeof(remote)) != 0) {
		LOG(LE_LOG_ERROR, "%s: cannot open the HID %s channel: %s",
			addressString, psm == kPSMControl ? "control" : "interrupt",
			strerror(errno));
		close(channel);
		return -1;
	}
	LOG(LE_LOG_DEBUG, "%s: HID %s channel open", addressString,
		psm == kPSMControl ? "control" : "interrupt");
	return channel;
}


void
HIDHost::_ChannelOpened(uint16 psm, int channel, const bdaddr_t& address)
{
	char addressString[18];
	format_address(address, addressString);

	BAutolock locker(fLock);
	BMessage device;
	if (_FindStored(address, &device) < 0) {
		LOG(LE_LOG_INFO, "%s: refusing its HID %s channel: not a paired HID "
			"device", addressString,
			psm == kPSMControl ? "control" : "interrupt");
		locker.Unlock();
		close(channel);
		return;
	}

	uint64 key = _Key(address);
	std::map<uint64, Pending>::iterator found = fPending.find(key);
	if (found == fPending.end()) {
		Pending pending;
		pending.control = -1;
		pending.interrupt = -1;
		found = fPending.insert(std::make_pair(key, pending)).first;
	}

	int& slot = psm == kPSMControl
		? found->second.control : found->second.interrupt;
	if (slot >= 0)
		close(slot);
	slot = channel;
	LOG(LE_LOG_DEBUG, "%s: the device opened its HID %s channel",
		addressString, psm == kPSMControl ? "control" : "interrupt");

	if (found->second.control < 0 || found->second.interrupt < 0)
		return;

	int control = found->second.control;
	int interrupt = found->second.interrupt;
	fPending.erase(found);
	locker.Unlock();

	_StartSession(address, control, interrupt);
}


void
HIDHost::_StartSession(const bdaddr_t& address, int control, int interrupt)
{
	BAutolock locker(fLock);
	if (fQuitting) {
		close(control);
		close(interrupt);
		return;
	}

	uint64 key = _Key(address);
	std::map<uint64, Session*>::iterator old = fSessions.find(key);
	if (old != fSessions.end()) {
		// The device came back before we noticed it was gone: the old
		// channels are dead.
		old->second->Stop();
		fSessions.erase(old);
	}

	Session* session = new(std::nothrow) Session(this, address, control,
		interrupt);
	if (session == NULL) {
		close(control);
		close(interrupt);
		return;
	}
	fSessions[key] = session;
	if (session->Start() != B_OK) {
		fSessions.erase(key);
		delete session;
		close(control);
		close(interrupt);
	}
}


void
HIDHost::_SessionEnded(Session* session)
{
	BAutolock locker(fLock);
	std::map<uint64, Session*>::iterator found = fSessions.begin();
	for (; found != fSessions.end(); found++) {
		if (found->second == session) {
			fSessions.erase(found);
			break;
		}
	}
}


bool
HIDHost::_IsEncrypted(const bdaddr_t& address)
{
	BAutolock locker(fLock);
	std::map<uint64, bool>::iterator found = fEncrypted.find(_Key(address));
	return found != fEncrypted.end() && found->second;
}


bool
HIDHost::_DeviceInfo(const bdaddr_t& address, BMessage& device)
{
	BAutolock locker(fLock);
	return _FindStored(address, &device) >= 0;
}


int32
HIDHost::_FindStored(const bdaddr_t& address, BMessage* _device)
{
	BMessage device;
	for (int32 i = 0; fDevices.FindMessage("device", i, &device) == B_OK;
			i++) {
		const void* data;
		ssize_t size;
		if (device.FindData("address", B_RAW_TYPE, &data, &size) == B_OK
			&& size == sizeof(address.b)
			&& memcmp(data, address.b, size) == 0) {
			if (_device != NULL)
				*_device = device;
			return i;
		}
	}
	return -1;
}


bool
HIDHost::_RemoveStored(const bdaddr_t& address)
{
	int32 index = _FindStored(address, NULL);
	if (index < 0)
		return false;
	fDevices.RemoveData("device", index);
	return true;
}


static status_t
settings_path(BPath& path)
{
	status_t status = find_directory(B_USER_SETTINGS_DIRECTORY, &path);
	if (status == B_OK)
		status = path.Append("bluetooth/hid_devices");
	return status;
}


status_t
HIDHost::_Load()
{
	BPath path;
	status_t status = settings_path(path);
	if (status != B_OK)
		return status;

	BFile file(path.Path(), B_READ_ONLY);
	BMessage devices;
	status = file.InitCheck();
	if (status == B_OK)
		status = devices.Unflatten(&file);
	if (status != B_OK)
		return status;

	BAutolock locker(fLock);
	fDevices = devices;
	type_code type;
	int32 count = 0;
	fDevices.GetInfo("device", &type, &count);
	LOG(LE_LOG_INFO, "%" B_PRId32 " paired HID device%s", count,
		count == 1 ? "" : "s");
	return B_OK;
}


status_t
HIDHost::_Save()
{
	// fLock is held.
	BPath path;
	status_t status = settings_path(path);
	if (status != B_OK)
		return status;

	BString temporary(path.Path());
	temporary << ".new";
	BFile file(temporary.String(), B_WRITE_ONLY | B_CREATE_FILE
		| B_ERASE_FILE);
	status = file.InitCheck();
	if (status == B_OK)
		status = fDevices.Flatten(&file);
	if (status == B_OK)
		status = file.Sync();
	if (status == B_OK && rename(temporary.String(), path.Path()) != 0)
		status = errno;
	if (status != B_OK) {
		LOG(LE_LOG_ERROR, "cannot save %s: %s", path.Path(),
			strerror(status));
	}
	return status;
}
