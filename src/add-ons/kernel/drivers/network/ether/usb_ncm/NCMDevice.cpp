/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Copyright 2008, Michael Lotz <mmlr@mlotz.ch> (usb_ecm, which this follows)
 * Distributed under the terms of the MIT License.
 */


/*!	USB CDC Network Control Model (NCM 1.0) driver.

	ECM carries one Ethernet frame per USB transfer; NCM packs many into one
	Network Transfer Block (NTB). With usb_ecm, a 2.5 Gbit/s Realtek RTL8156
	topped out near 650 Mbit/s up and 525 down, held back by the cost of a
	transfer per frame (airos-works/haiku#123).

	Receiving: several bulk IN transfers of the NTB input size stay queued.
	The stack's reader takes the datagrams of a completed NTB, many per call
	(ETHER_RECEIVE_NET_BUFFERS), and the transfer goes back to the device as
	soon as its last datagram is out.

	Sending: frames are packed into the NTB being filled. It goes out at once
	while fewer than "tx_eager" transfers are in flight (low latency when the
	bus has room), when it is full (by size or datagram count), or else when a
	transfer completes. So no timer is needed: a frame waits only while
	transfers are in flight, and the next completion sends it. Under load the
	NTBs grow by themselves to what the bus takes.
*/


#include "NCMDevice.h"

#include <ByteOrder.h>
#include <driver_settings.h>
#include <net/if_media.h>
#include <stdlib.h>
#include <string.h>

#include <kernel.h>
#include <util/AutoLock.h>


// CDC NCM 1.0 class requests (table 6-2)
enum {
	NCM_SET_ETHERNET_PACKET_FILTER	= 0x43,
	NCM_GET_NTB_PARAMETERS			= 0x80,
	NCM_SET_NTB_FORMAT				= 0x84,
	NCM_SET_NTB_INPUT_SIZE			= 0x86,
	NCM_SET_CRC_MODE				= 0x8a
};

// bmNetworkCapabilities of the NCM functional descriptor (table 5-2)
enum {
	NCM_CAP_PACKET_FILTER		= 0x01,
	NCM_CAP_NET_ADDRESS			= 0x02,
	NCM_CAP_ENCAPSULATED		= 0x04,
	NCM_CAP_MAX_DATAGRAM_SIZE	= 0x08,
	NCM_CAP_CRC_MODE			= 0x10,
	NCM_CAP_NTB_INPUT_SIZE_8	= 0x20
};

// packet filter bits (ECM 1.2, 6.2.4)
enum {
	PACKET_TYPE_PROMISCUOUS		= 0x01,
	PACKET_TYPE_ALL_MULTICAST	= 0x02,
	PACKET_TYPE_DIRECTED		= 0x04,
	PACKET_TYPE_BROADCAST		= 0x08
};

// notifications
enum {
	CDC_NOTIFY_NETWORK_CONNECTION		= 0x00,
	CDC_NOTIFY_CONNECTION_SPEED_CHANGE	= 0x2a
};

static const uint8 kNCMFunctionalDescriptor = 0x1a;
static const uint8 kNTBProtocol = 0x01;
	// of the data interface

typedef struct ntb_parameters {
	uint16	length;
	uint16	formats_supported;
	uint32	in_max_size;
	uint16	in_divisor;
	uint16	in_payload_remainder;
	uint16	in_alignment;
	uint16	reserved;
	uint32	out_max_size;
	uint16	out_divisor;
	uint16	out_payload_remainder;
	uint16	out_alignment;
	uint16	out_max_datagrams;
} _PACKED ntb_parameters;

typedef struct cdc_notification {
	uint8	request_type;
	uint8	notification_code;
	uint16	value;
	uint16	index;
	uint16	data_length;
	uint8	data[0];
} _PACKED cdc_notification;

// defaults, which the driver settings file "usb_ncm" can change
static const uint32 kDefaultRxNTBSize = 32768;
static const uint32 kDefaultTxNTBSize = 32768;
static const uint32 kDefaultRxTransfers = 8;
static const uint32 kDefaultTxTransfers = 8;
static const uint32 kDefaultTxEager = 2;
static const uint32 kDefaultTxDatagrams = 32;

static const uint32 kMinNTBSize = 2048;
	// USB_CDC_NCM_NTB_MIN_IN_SIZE
static const uint32 kMaxNTBSize = 65536;
static const size_t kMinFrame = 60;


static inline bool
is_user_address(const void* address)
{
	return IS_USER_ADDRESS(address);
}


static status_t
copy_to(void* target, const void* source, size_t length)
{
	if (is_user_address(target))
		return user_memcpy(target, source, length);
	memcpy(target, source, length);
	return B_OK;
}


static status_t
copy_from(void* target, const void* source, size_t length)
{
	if (is_user_address(source))
		return user_memcpy(target, source, length);
	memcpy(target, source, length);
	return B_OK;
}


NCMDevice::NCMDevice(usb_device device)
	:
	fStatus(B_ERROR),
	fOpen(false),
	fRemoved(false),
	fNonBlocking(false),
	fPromiscuous(false),
	fInsideNotify(0),
	fDevice(device),
	fVendorID(0),
	fProductID(0),
	fKeepAwake(false),
	fConfiguration(NULL),
	fControlInterface(0),
	fDataInterface(0),
	fDataAlternate(1),
	fMACAddressIndex(0),
	fNetworkCapabilities(0),
	fMaxSegmentSize(1514),
	fNotifyEndpoint(0),
	fReadEndpoint(0),
	fWriteEndpoint(0),
	fWriteMaxPacketSize(0),
	fNTBFormats(0),
	fNTB32(false),
	fDeviceRxMax(0),
	fDeviceTxMax(0),
	fRxNTBSize(0),
	fTxNTBSize(0),
	fTxDivisor(4),
	fTxRemainder(0),
	fTxAlignment(4),
	fTxMaxDatagrams(0),
	fMaxFrame(1518),
	fRxNTBSetting(kDefaultRxNTBSize),
	fTxNTBSetting(kDefaultTxNTBSize),
	fRxTransfers(kDefaultRxTransfers),
	fTxTransfers(kDefaultTxTransfers),
	fTxEager(kDefaultTxEager),
	fTxDatagramSetting(kDefaultTxDatagrams),
	fRxBufferSize(0),
	fRxDoneHead(0),
	fRxDoneCount(0),
	fRxSem(-1),
	fRxRunning(false),
	fRxCurrent(-1),
	fRxNext(0),
	fRxCount(0),
	fTxBufferSize(0),
	fTxFill(-1),
	fTxInFlight(0),
	fTxSequence(0),
	fTxRunning(false),
	fTxStalled(0),
	fNotifyBuffer(NULL),
	fNotifyBufferLength(0),
	fNotifyStopping(0),
	fPendingTransfers(0),
	fRecoveryFlags(0),
	fRecoveryThread(-1),
	fLinkStateChangeSem(-1),
	fHasConnection(false),
	fDownstreamSpeed(0),
	fUpstreamSpeed(0)
{
	B_INITIALIZE_SPINLOCK(&fRxDoneLock);
	mutex_init(&fRxLock, DRIVER_NAME " receive");
	mutex_init(&fTxLock, DRIVER_NAME " send");
	mutex_init(&fRecoveryLock, DRIVER_NAME " recovery");
	fTxCondition.Init(this, DRIVER_NAME " send");
	memset(&fStats, 0, sizeof(fStats));

	for (int32 i = 0; i < kMaxRxTransfers; i++) {
		fRxSlots[i].device = this;
		fRxSlots[i].buffer = NULL;
		fRxSlots[i].queued = false;
	}
	for (int32 i = 0; i < kMaxTxTransfers; i++) {
		fTxSlots[i].device = this;
		fTxSlots[i].buffer = NULL;
		fTxSlots[i].state = TX_FREE;
	}

	const usb_device_descriptor* deviceDescriptor
		= gUSBModule->get_device_descriptor(device);
	if (deviceDescriptor == NULL) {
		TRACE_ALWAYS("failed to get the device descriptor\n");
		return;
	}
	fVendorID = deviceDescriptor->vendor_id;
	fProductID = deviceDescriptor->product_id;

	if (_SetupDevice() != B_OK)
		return;

	if (_ReadMACAddress(fMACAddress) != B_OK) {
		TRACE_ALWAYS("failed to read the MAC address\n");
		return;
	}

	fStatus = B_OK;
}


NCMDevice::~NCMDevice()
{
	_StopNotifications();
	if (fOpen && !fRemoved) {
		gUSBModule->cancel_queued_transfers(fNotifyEndpoint);
		gUSBModule->cancel_queued_transfers(fReadEndpoint);
		gUSBModule->cancel_queued_transfers(fWriteEndpoint);
	}
	_WaitForTransfers();
	if (fRxSem >= 0)
		delete_sem(fRxSem);

	_FreeBuffers();
	free(fNotifyBuffer);
	mutex_destroy(&fRxLock);
	mutex_destroy(&fTxLock);
	mutex_destroy(&fRecoveryLock);
}


/*!	Looks for a usable NCM function in any configuration of \a device:
	a communication interface of the NCM subclass with union and Ethernet
	functional descriptors and a notification endpoint, and a data interface
	with an alternate that has the bulk endpoints (preferably the one of the
	NTB protocol; Linux and FreeBSD take any). Makes no requests.
*/
bool
NCMDevice::FindFunction(usb_device device, ncm_function* function)
{
	const usb_device_descriptor* deviceDescriptor
		= gUSBModule->get_device_descriptor(device);
	if (deviceDescriptor == NULL)
		return false;

	for (uint32 i = 0; i < deviceDescriptor->num_configurations; i++) {
		const usb_configuration_info* config
			= gUSBModule->get_nth_configuration(device, i);
		if (config == NULL)
			continue;

		for (size_t j = 0; j < config->interface_count; j++) {
			const usb_interface_info* interface = config->interface[j].active;
			if (interface == NULL || interface->descr == NULL)
				continue;
			const usb_interface_descriptor* descriptor = interface->descr;
			if (descriptor->interface_class
					!= USB_CDC_COMMUNICATION_INTERFACE_CLASS
				|| descriptor->interface_subclass
					!= USB_CDC_COMMUNICATION_INTERFACE_NCM_SUBCLASS
				|| interface->endpoint_count == 0) {
				continue;
			}

			bool foundUnion = false;
			bool foundEthernet = false;
			ncm_function found;
			found.configuration = i;
			found.control_interface = descriptor->interface_number;
			found.data_interface = 0;
			found.data_alternate = 0;
			found.mac_address_index = 0;
			found.max_segment_size = 1514;
			found.capabilities = 0;
			for (size_t k = 0; k < interface->generic_count; k++) {
				const usb_generic_descriptor* generic
					= &interface->generic[k]->generic;
				if (generic->descriptor_type != 0x24)
					continue;
					// CS_INTERFACE

				if (generic->length >= 5
					&& generic->data[0] == USB_CDC_UNION_FUNCTIONAL_DESCRIPTOR) {
					found.data_interface = generic->data[2];
					foundUnion = true;
				} else if (generic->length
						>= sizeof(usb_cdc_ethernet_functional_descriptor)
					&& generic->data[0]
						== USB_CDC_ETHERNET_FUNCTIONAL_DESCRIPTOR) {
					const usb_cdc_ethernet_functional_descriptor* ethernet
						= (const usb_cdc_ethernet_functional_descriptor*)generic;
					found.mac_address_index = ethernet->mac_address_index;
					found.max_segment_size
						= B_LENDIAN_TO_HOST_INT16(ethernet->max_segment_size);
					foundEthernet = true;
				} else if (generic->length >= 6
					&& generic->data[0] == kNCMFunctionalDescriptor) {
					found.capabilities = generic->data[3];
				}
			}
			if (!foundUnion || !foundEthernet)
				continue;

			// The interface numbers are the indices: the stack relies on it
			// (set_alt_interface()).
			if (found.control_interface >= config->interface_count
				|| found.data_interface >= config->interface_count) {
				continue;
			}

			const usb_interface_list* data
				= &config->interface[found.data_interface];
			for (size_t a = 0; a < data->alt_count; a++) {
				const usb_interface_info* alt = &data->alt[a];
				if (alt->descr == NULL
					|| alt->descr->interface_class
						!= USB_CDC_DATA_INTERFACE_CLASS
					|| alt->descr->alternate_setting == 0
					|| alt->endpoint_count < 2) {
					continue;
				}
				if (found.data_alternate == 0
					|| alt->descr->interface_protocol == kNTBProtocol) {
					found.data_alternate = alt->descr->alternate_setting;
				}
			}
			if (found.data_alternate == 0
				|| found.data_alternate >= data->alt_count) {
				continue;
			}

			*function = found;
			return true;
		}
	}

	return false;
}


/*!	Finds the configuration with an NCM function and makes it the current
	one. A device may offer NCM next to a vendor configuration and ECM (the
	Realtek RTL8156 has all three): NCM is the one taken here.
*/
status_t
NCMDevice::_SetupDevice()
{
	ncm_function function;
	if (!FindFunction(fDevice, &function)) {
		TRACE_ALWAYS("no usable NCM function found\n");
		return B_ERROR;
	}

	const usb_configuration_info* config
		= gUSBModule->get_nth_configuration(fDevice, function.configuration);

	// Leaves a configuration in use alone: setting it again would reset the
	// endpoints of the other functions of a composite device
	// (set_configuration() does nothing for the current one).
	status_t status = gUSBModule->set_configuration(fDevice, config);
	if (status != B_OK) {
		TRACE_ALWAYS("setting configuration %" B_PRIu32 " failed: %s\n",
			function.configuration, strerror(status));
		return status;
	}

	fConfiguration = gUSBModule->get_configuration(fDevice);
	fControlInterface = function.control_interface;
	fDataInterface = function.data_interface;
	fDataAlternate = function.data_alternate;
	fMACAddressIndex = function.mac_address_index;
	fMaxSegmentSize = function.max_segment_size;
	fNetworkCapabilities = function.capabilities;

	const usb_interface_info* control
		= fConfiguration->interface[fControlInterface].active;
	fNotifyBufferLength = max_c(16,
		control->endpoint[0].descr->max_packet_size & 0x7ff);
	free(fNotifyBuffer);
	fNotifyBuffer = (uint8*)malloc(fNotifyBufferLength);
	if (fNotifyBuffer == NULL)
		return B_NO_MEMORY;

	TRACE_ALWAYS("NCM in configuration %" B_PRIu32 ", interfaces %u/%u "
		"(alternate %u), capabilities 0x%02x, max segment %u\n",
		function.configuration, fControlInterface, fDataInterface,
		fDataAlternate, fNetworkCapabilities, fMaxSegmentSize);
	return B_OK;
}


status_t
NCMDevice::_ReadMACAddress(uint8* buffer)
{
	if (fMACAddressIndex == 0)
		return B_BAD_VALUE;

	size_t actualLength = 0;
	uint8 macString[26];
	uint8 langid[4];

	status_t result = gUSBModule->get_descriptor(fDevice,
		USB_DESCRIPTOR_STRING, 0, 0, langid, sizeof(langid), &actualLength);
	if (result != B_OK)
		return result;

	result = gUSBModule->get_descriptor(fDevice, USB_DESCRIPTOR_STRING,
		fMACAddressIndex, (langid[3] << 8) | langid[2], macString,
		sizeof(macString), &actualLength);
	if (result != B_OK)
		return result;

	if (actualLength != sizeof(macString)) {
		TRACE_ALWAYS("did not retrieve the full MAC address\n");
		return B_ERROR;
	}

	char macPart[3];
	macPart[2] = 0;
	for (int32 i = 0; i < 6; i++) {
		macPart[0] = macString[2 + i * 4 + 0];
		macPart[1] = macString[2 + i * 4 + 2];
		buffer[i] = strtol(macPart, NULL, 16);
	}

	TRACE_ALWAYS("MAC address %02x:%02x:%02x:%02x:%02x:%02x\n", buffer[0],
		buffer[1], buffer[2], buffer[3], buffer[4], buffer[5]);
	return B_OK;
}


void
NCMDevice::_ReadSettings()
{
	fRxNTBSetting = kDefaultRxNTBSize;
	fTxNTBSetting = kDefaultTxNTBSize;
	fRxTransfers = kDefaultRxTransfers;
	fTxTransfers = kDefaultTxTransfers;
	fTxEager = kDefaultTxEager;
	fTxDatagramSetting = kDefaultTxDatagrams;

	void* handle = load_driver_settings(DRIVER_NAME);
	if (handle == NULL)
		return;

	struct {
		const char*	name;
		uint32*		value;
		uint32		min;
		uint32		max;
	} settings[] = {
		{ "rx_ntb_size", &fRxNTBSetting, kMinNTBSize, kMaxNTBSize },
		{ "tx_ntb_size", &fTxNTBSetting, kMinNTBSize, kMaxNTBSize },
		{ "rx_transfers", &fRxTransfers, 1, kMaxRxTransfers },
		{ "tx_transfers", &fTxTransfers, 1, kMaxTxTransfers },
		{ "tx_eager", &fTxEager, 1, kMaxTxTransfers },
		{ "tx_max_datagrams", &fTxDatagramSetting, 1, 256 },
	};
	for (size_t i = 0; i < B_COUNT_OF(settings); i++) {
		const char* text = get_driver_parameter(handle, settings[i].name,
			NULL, NULL);
		if (text == NULL)
			continue;
		uint32 value = strtoul(text, NULL, 0);
		if (value < settings[i].min)
			value = settings[i].min;
		if (value > settings[i].max)
			value = settings[i].max;
		*settings[i].value = value;
	}
	unload_driver_settings(handle);
}


status_t
NCMDevice::_ClassRequest(bool in, uint8 request, uint16 value, void* data,
	size_t length)
{
	size_t actualLength = 0;
	status_t status = gUSBModule->send_request(fDevice,
		(in ? USB_REQTYPE_INTERFACE_IN : USB_REQTYPE_INTERFACE_OUT)
			| USB_REQTYPE_CLASS,
		request, value, fControlInterface, length, data, &actualLength);
	if (status == B_OK && in && actualLength != length)
		status = B_IO_ERROR;
	return status;
}


/*!	Asks the function for its NTB parameters and sets up ours. Called with the
	data interface in its alternate setting 0: the format and the input size
	can only change there (NCM 1.0, 7.2).
*/
status_t
NCMDevice::_Negotiate()
{
	ntb_parameters parameters;
	status_t status = _ClassRequest(true, NCM_GET_NTB_PARAMETERS, 0,
		&parameters, sizeof(parameters));
	if (status != B_OK) {
		TRACE_ALWAYS("GET_NTB_PARAMETERS failed: %s\n", strerror(status));
		return status;
	}

	fNTBFormats = B_LENDIAN_TO_HOST_INT16(parameters.formats_supported);
	fDeviceRxMax = B_LENDIAN_TO_HOST_INT32(parameters.in_max_size);
	fDeviceTxMax = B_LENDIAN_TO_HOST_INT32(parameters.out_max_size);
	uint16 outDivisor = B_LENDIAN_TO_HOST_INT16(parameters.out_divisor);
	uint16 outRemainder
		= B_LENDIAN_TO_HOST_INT16(parameters.out_payload_remainder);
	uint16 outAlignment = B_LENDIAN_TO_HOST_INT16(parameters.out_alignment);
	uint16 outMaxDatagrams
		= B_LENDIAN_TO_HOST_INT16(parameters.out_max_datagrams);

	// Blocks of 16 bit format are what every function must know; the 32 bit
	// one only for a function that knows nothing else.
	if ((fNTBFormats & 0x01) != 0)
		fNTB32 = false;
	else if ((fNTBFormats & 0x02) != 0)
		fNTB32 = true;
	else {
		TRACE_ALWAYS("no NTB format supported (0x%04x)\n", fNTBFormats);
		return B_NOT_SUPPORTED;
	}
	const uint32 formatMax = fNTB32 ? 0xffffffff : 0xffff;

	if (fDeviceRxMax < kMinNTBSize || fDeviceTxMax < kMinNTBSize) {
		TRACE_ALWAYS("NTB sizes %" B_PRIu32 "/%" B_PRIu32 " too small\n",
			fDeviceRxMax, fDeviceTxMax);
		return B_NOT_SUPPORTED;
	}

	fRxNTBSize = min_c(min_c(fDeviceRxMax, fRxNTBSetting), formatMax);
	fTxNTBSize = min_c(min_c(fDeviceTxMax, fTxNTBSetting), formatMax);
	fTxMaxDatagrams = fTxDatagramSetting;
	if (outMaxDatagrams != 0 && outMaxDatagrams < fTxMaxDatagrams)
		fTxMaxDatagrams = outMaxDatagrams;

	// errors are not fatal below: the defaults stay
	if ((fNTBFormats & 0x02) != 0) {
		status = _ClassRequest(false, NCM_SET_NTB_FORMAT, fNTB32 ? 1 : 0,
			NULL, 0);
		if (status != B_OK)
			TRACE_ALWAYS("SET_NTB_FORMAT failed: %s\n", strerror(status));
	}

	if ((fNetworkCapabilities & NCM_CAP_CRC_MODE) != 0) {
		status = _ClassRequest(false, NCM_SET_CRC_MODE, 0, NULL, 0);
		if (status != B_OK)
			TRACE_ALWAYS("SET_CRC_MODE failed: %s\n", strerror(status));
	}

	uint32 inputSize[2];
	inputSize[0] = B_HOST_TO_LENDIAN_INT32(fRxNTBSize);
	inputSize[1] = 0;
		// wNtbInMaxDatagrams 0: no limit
	status = _ClassRequest(false, NCM_SET_NTB_INPUT_SIZE, 0, inputSize,
		(fNetworkCapabilities & NCM_CAP_NTB_INPUT_SIZE_8) != 0 ? 8 : 4);
	if (status != B_OK) {
		// the function keeps sending up to its own maximum
		TRACE_ALWAYS("SET_NTB_INPUT_SIZE %" B_PRIu32 " failed: %s\n",
			fRxNTBSize, strerror(status));
		fRxNTBSize = min_c(fDeviceRxMax, formatMax);
		if (fRxNTBSize > 1024 * 1024)
			return B_NOT_SUPPORTED;
	}

	if (!fTxWriter.Init(fNTB32, fTxNTBSize, outDivisor, outRemainder,
			outAlignment, fTxMaxDatagrams)
		&& !fTxWriter.Init(fNTB32, fTxNTBSize, 4, 0, 4, fTxMaxDatagrams)) {
		TRACE_ALWAYS("cannot build NTBs of %" B_PRIu32 " bytes\n",
			fTxNTBSize);
		return B_NOT_SUPPORTED;
	}
	fTxNTBSize = fTxWriter.Capacity();
	fTxDivisor = outDivisor;
	fTxRemainder = outRemainder;
	fTxAlignment = outAlignment;

	// Frames as long as the Ethernet descriptor says (with room for a VLAN
	// tag when receiving), as long as an empty NTB can carry one.
	fMaxFrame = max_c(fMaxSegmentSize, 1514);
	if (!fTxWriter.CanAdd(fMaxFrame))
		fMaxFrame = 1514;
	if (!fTxWriter.CanAdd(fMaxFrame)) {
		TRACE_ALWAYS("NTBs of %" B_PRIu32 " bytes cannot carry a frame\n",
			fTxNTBSize);
		return B_NOT_SUPPORTED;
	}

	TRACE_ALWAYS("NTB%s: in %" B_PRIu32 " (max %" B_PRIu32 "), out %"
		B_PRIu32 " (max %" B_PRIu32 ", divisor %u, remainder %u, alignment "
		"%u, %" B_PRIu32 " datagrams)\n", fNTB32 ? "32" : "16", fRxNTBSize,
		fDeviceRxMax, fTxNTBSize, fDeviceTxMax, outDivisor, outRemainder,
		outAlignment, fTxMaxDatagrams);
	return B_OK;
}


status_t
NCMDevice::_SetPacketFilter()
{
	if ((fNetworkCapabilities & NCM_CAP_PACKET_FILTER) == 0)
		return B_OK;

	uint16 filter = PACKET_TYPE_DIRECTED | PACKET_TYPE_BROADCAST
		| PACKET_TYPE_ALL_MULTICAST;
	if (fPromiscuous)
		filter |= PACKET_TYPE_PROMISCUOUS;
	return _ClassRequest(false, NCM_SET_ETHERNET_PACKET_FILTER, filter, NULL,
		0);
}


status_t
NCMDevice::_AllocateBuffers()
{
	// sizes may differ from the last open: the settings can change
	if (fRxBufferSize != fRxNTBSize || fTxBufferSize != fTxNTBSize + 1)
		_FreeBuffers();

	fRxBufferSize = fRxNTBSize;
	fTxBufferSize = fTxNTBSize + 1;
		// Finish() may add a byte

	for (uint32 i = 0; i < fRxTransfers; i++) {
		if (fRxSlots[i].buffer == NULL)
			fRxSlots[i].buffer = (uint8*)malloc(fRxBufferSize);
		if (fRxSlots[i].buffer == NULL)
			return B_NO_MEMORY;
	}
	for (uint32 i = 0; i < fTxTransfers; i++) {
		if (fTxSlots[i].buffer == NULL)
			fTxSlots[i].buffer = (uint8*)malloc(fTxBufferSize);
		if (fTxSlots[i].buffer == NULL)
			return B_NO_MEMORY;
	}
	return B_OK;
}


void
NCMDevice::_FreeBuffers()
{
	for (int32 i = 0; i < kMaxRxTransfers; i++) {
		free(fRxSlots[i].buffer);
		fRxSlots[i].buffer = NULL;
	}
	for (int32 i = 0; i < kMaxTxTransfers; i++) {
		free(fTxSlots[i].buffer);
		fTxSlots[i].buffer = NULL;
	}
	fRxBufferSize = fTxBufferSize = 0;
}


status_t
NCMDevice::Open()
{
	if (fOpen)
		return B_BUSY;
	if (fRemoved)
		return B_ERROR;

	_ReadSettings();
	_SetPHYPower(true);

	const usb_configuration_info* config
		= gUSBModule->get_configuration(fDevice);
	if (config == NULL || config != fConfiguration
		|| fDataInterface >= config->interface_count
		|| config->interface[fDataInterface].alt_count <= fDataAlternate) {
		// a device that came back behind a hub can start over in another
		// configuration
		TRACE_ALWAYS("the device is not in its NCM configuration\n");
		return B_ERROR;
	}

	// The data interface without endpoints first: the NTB parameters can
	// only change there, and the switch to the NTB alternate resets the
	// function's data path (NCM 1.0, 7.2).
	gUSBModule->set_alt_interface(fDevice,
		&config->interface[fDataInterface].alt[0]);

	status_t status = _Negotiate();
	if (status != B_OK)
		return status;

	status = _AllocateBuffers();
	if (status != B_OK)
		return status;

	config = gUSBModule->get_configuration(fDevice);
	status = gUSBModule->set_alt_interface(fDevice,
		&config->interface[fDataInterface].alt[fDataAlternate]);
	if (status != B_OK) {
		TRACE_ALWAYS("selecting the NTB alternate failed: %s\n",
			strerror(status));
		return status;
	}

	// Setting an alternate replaces the pipes of that interface: take the
	// handles from the configuration as it is now.
	config = gUSBModule->get_configuration(fDevice);
	const usb_interface_info* data = config->interface[fDataInterface].active;
	fReadEndpoint = fWriteEndpoint = 0;
	for (size_t i = 0; i < data->endpoint_count; i++) {
		const usb_endpoint_descriptor* descriptor = data->endpoint[i].descr;
		if ((descriptor->attributes & USB_ENDPOINT_ATTR_MASK)
				!= USB_ENDPOINT_ATTR_BULK) {
			continue;
		}
		if ((descriptor->endpoint_address & USB_ENDPOINT_ADDR_DIR_IN) != 0) {
			if (fReadEndpoint == 0)
				fReadEndpoint = data->endpoint[i].handle;
		} else if (fWriteEndpoint == 0) {
			fWriteEndpoint = data->endpoint[i].handle;
			fWriteMaxPacketSize = descriptor->max_packet_size & 0x7ff;
		}
	}
	if (fReadEndpoint == 0 || fWriteEndpoint == 0) {
		TRACE_ALWAYS("no bulk endpoints in the NTB alternate\n");
		return B_ERROR;
	}

	const usb_interface_info* control
		= config->interface[fControlInterface].active;
	if (control->endpoint_count == 0)
		return B_ERROR;
	fNotifyEndpoint = control->endpoint[0].handle;

	status = _SetPacketFilter();
	if (status != B_OK)
		TRACE_ALWAYS("setting the packet filter failed: %s\n", strerror(status));

	memset(&fStats, 0, sizeof(fStats));
	atomic_set(&fNotifyStopping, 0);

	// sending
	{
		MutexLocker locker(fTxLock);
		for (uint32 i = 0; i < kMaxTxTransfers; i++)
			fTxSlots[i].state = TX_FREE;
		fTxFill = -1;
		fTxInFlight = 0;
		fTxRunning = true;
		atomic_set(&fTxStalled, 0);
	}

	status = _StartReceiving();
	if (status != B_OK) {
		MutexLocker locker(fTxLock);
		fTxRunning = false;
		return status;
	}

	fHasConnection = false;
	fDownstreamSpeed = fUpstreamSpeed = 0;
	if (_QueueNotify() != B_OK) {
		TRACE_ALWAYS("cannot queue notifications, assuming a link\n");
		fHasConnection = true;
		fDownstreamSpeed = fUpstreamSpeed = 100 * 1000 * 1000;
	}

	fOpen = true;
	return B_OK;
}


status_t
NCMDevice::Close()
{
	fRxRunning = false;
	{
		MutexLocker locker(fTxLock);
		fTxRunning = false;
		fTxCondition.NotifyAll(B_INTERRUPTED);
	}

	// Neither the notification hook nor the recovery thread may queue
	// anything after the cancel below.
	_StopNotifications();

	if (!fRemoved) {
		gUSBModule->cancel_queued_transfers(fNotifyEndpoint);
		gUSBModule->cancel_queued_transfers(fReadEndpoint);
		gUSBModule->cancel_queued_transfers(fWriteEndpoint);
	}

	// whoever waits for a frame gets B_BAD_SEM_ID and returns
	sem_id rxSem = fRxSem;
	fRxSem = -1;
	if (rxSem >= 0)
		delete_sem(rxSem);

	{
		MutexLocker locker(fRxLock);
		fRxCurrent = -1;
		for (int32 i = 0; i < kMaxRxTransfers; i++)
			fRxSlots[i].queued = false;
	}

	TRACE_ALWAYS("closed: received %" B_PRIu64 " NTBs (%" B_PRIu64
		" datagrams, up to %" B_PRIu64 " in one, %" B_PRIu64 " bad), sent %"
		B_PRIu64 " NTBs (%" B_PRIu64 " datagrams, up to %" B_PRIu64
		" in one; %" B_PRIu64 " sent idle, %" B_PRIu64 " full, %" B_PRIu64
		" on completion), %" B_PRIu64 " receive and %" B_PRIu64
		" send errors\n", fStats.rx_ntbs, fStats.rx_datagrams,
		fStats.rx_max_datagrams, fStats.rx_bad_ntbs, fStats.tx_ntbs,
		fStats.tx_datagrams, fStats.tx_max_datagrams_seen,
		fStats.tx_flush_idle, fStats.tx_flush_full,
		fStats.tx_flush_completion, fStats.rx_errors, fStats.tx_errors);

	if (!fRemoved) {
		const usb_configuration_info* config
			= gUSBModule->get_configuration(fDevice);
		if (config != NULL && fDataInterface < config->interface_count) {
			gUSBModule->set_alt_interface(fDevice,
				&config->interface[fDataInterface].alt[0]);
		}

		// no link (and no link lights) while the interface is down
		_SetPHYPower(false);
	}

	fOpen = false;
	return B_OK;
}


status_t
NCMDevice::Free()
{
	// Nobody is in read(), write() or ioctl() anymore: the buffers can go,
	// once no transfer can write into them.
	if (!fOpen) {
		_WaitForTransfers();
		_FreeBuffers();
	}
	return B_OK;
}


void
NCMDevice::Removed()
{
	fRemoved = true;
	fRxRunning = false;
	fHasConnection = false;
	fDownstreamSpeed = fUpstreamSpeed = 0;

	{
		MutexLocker locker(fTxLock);
		fTxRunning = false;
		fTxCondition.NotifyAll(B_DEVICE_NOT_FOUND);
	}

	// The notification hook and the recovery thread queue transfers
	// themselves: they must be done before the pipes may no longer be used.
	_StopNotifications();

	gUSBModule->cancel_queued_transfers(fNotifyEndpoint);
	gUSBModule->cancel_queued_transfers(fReadEndpoint);
	gUSBModule->cancel_queued_transfers(fWriteEndpoint);
	if (fRxSem >= 0)
		release_sem_etc(fRxSem, 1, B_DO_NOT_RESCHEDULE);

	if (fLinkStateChangeSem >= B_OK)
		release_sem_etc(fLinkStateChangeSem, 1, B_DO_NOT_RESCHEDULE);
}


//	#pragma mark - receiving


status_t
NCMDevice::_StartReceiving()
{
	if (fRxSem >= 0)
		delete_sem(fRxSem);
	fRxSem = create_sem(0, DRIVER_NAME " receive");
	if (fRxSem < 0)
		return fRxSem;
	fRxRunning = true;

	{
		InterruptsSpinLocker locker(fRxDoneLock);
		fRxDoneHead = fRxDoneCount = 0;
	}

	MutexLocker locker(fRxLock);
	fRxCurrent = -1;
	fRxNext = fRxCount = 0;

	uint32 queued = 0;
	for (uint32 i = 0; i < fRxTransfers; i++) {
		fRxSlots[i].queued = false;
		_QueueRead(i);
		if (!fRxSlots[i].queued) {
			// the host controller takes no more on one endpoint
			break;
		}
		queued++;
	}

	if (queued == 0) {
		TRACE_ALWAYS("could not queue a read\n");
		fRxRunning = false;
		delete_sem(fRxSem);
		fRxSem = -1;
		return B_ERROR;
	}
	if (queued < fRxTransfers) {
		TRACE_ALWAYS("only %" B_PRIu32 " reads queued\n", queued);
		fRxTransfers = queued;
	}
	return B_OK;
}


void
NCMDevice::_QueueRead(uint32 index)
{
	rx_slot& slot = fRxSlots[index];
	if (fRemoved || !fRxRunning || slot.buffer == NULL)
		return;

	atomic_add(&fPendingTransfers, 1);
	status_t status = gUSBModule->queue_bulk(fReadEndpoint, slot.buffer,
		fRxBufferSize, _ReadCallback, &slot);
	slot.queued = status == B_OK;
	if (status != B_OK) {
		atomic_add(&fPendingTransfers, -1);
		// _NextDatagram() tries again
		if (fStats.rx_errors++ < 10)
			TRACE_ALWAYS("queueing a read failed: %s\n", strerror(status));
	}
}


void
NCMDevice::_ReadCallback(void* cookie, int32 status, void* data,
	size_t actualLength)
{
	rx_slot* slot = (rx_slot*)cookie;
	NCMDevice* device = slot->device;
	slot->status = status;
	slot->length = actualLength;

	{
		InterruptsSpinLocker locker(device->fRxDoneLock);
		uint32 index = slot - device->fRxSlots;
		device->fRxDone[(device->fRxDoneHead + device->fRxDoneCount)
			% kMaxRxTransfers] = index;
		device->fRxDoneCount++;
	}

	release_sem_etc(device->fRxSem, 1, B_DO_NOT_RESCHEDULE);
	atomic_add(&device->fPendingTransfers, -1);
}


/*!	Makes the next datagram available in fRxDatagrams[fRxNext], waiting for a
	completed NTB if \a block is true. Called with fRxLock held.
*/
status_t
NCMDevice::_NextDatagram(bool block)
{
	while (true) {
		if (fRxCurrent >= 0) {
			if (fRxNext < fRxCount)
				return B_OK;

			// all of it is out: back to the device with it
			_QueueRead(fRxCurrent);
			fRxCurrent = -1;
		}

		if (fRemoved)
			return B_DEVICE_NOT_FOUND;

		// Buffers that could not go back to the device (out of memory in
		// the controller's pool, say) are tried again: each one lost would
		// leave one transfer fewer queued, until receiving stops.
		bool idle = false;
		for (uint32 i = 0; i < fRxTransfers; i++) {
			if (!fRxSlots[i].queued && (int32)i != fRxCurrent) {
				_QueueRead(i);
				idle |= !fRxSlots[i].queued;
			}
		}

		status_t status;
		if (block && !fNonBlocking) {
			status = acquire_sem_etc(fRxSem, 1,
				B_CAN_INTERRUPT | (idle ? B_RELATIVE_TIMEOUT : 0),
				idle ? 100000 : 0);
			if (status == B_TIMED_OUT)
				continue;
		} else
			status = acquire_sem_etc(fRxSem, 1, B_RELATIVE_TIMEOUT, 0);
		if (status != B_OK) {
			if (fRemoved)
				return B_DEVICE_NOT_FOUND;
			return status == B_TIMED_OUT ? B_WOULD_BLOCK : status;
		}
		if (fRemoved)
			return B_DEVICE_NOT_FOUND;

		uint32 index;
		{
			InterruptsSpinLocker locker(fRxDoneLock);
			if (fRxDoneCount == 0) {
				// a wake-up from Removed()
				continue;
			}
			index = fRxDone[fRxDoneHead];
			fRxDoneHead = (fRxDoneHead + 1) % kMaxRxTransfers;
			fRxDoneCount--;
		}

		rx_slot& slot = fRxSlots[index];
		slot.queued = false;

		if (slot.status != B_OK) {
			if (slot.status == B_CANCELED) {
				if (!fRxRunning)
					return B_CANCELED;
				// not by us: the buffer goes back
				_QueueRead(index);
				continue;
			}

			fStats.rx_errors++;
			if (slot.status == B_DEV_STALLED
				&& gUSBModule->clear_feature(fReadEndpoint,
					USB_FEATURE_ENDPOINT_HALT) != B_OK) {
				TRACE_ALWAYS("clearing the stalled read pipe failed\n");
			} else if (slot.status != B_DEV_STALLED) {
				// mostly a device that is going away: no need to hurry
				TRACE_ALWAYS("read error 0x%08" B_PRIx32 "\n", slot.status);
				snooze(1000);
			}
			_QueueRead(index);
			continue;
		}

		ncm::parse_stats parseStats = {};
		uint32 count = 0;
		ncm::parse_result result = ncm::parse_ntb(slot.buffer, slot.length,
			fMaxFrame + 4, fRxDatagrams, kMaxRxDatagrams, &count,
			&parseStats);

		fStats.rx_ntbs++;
		fStats.rx_bad_datagrams += parseStats.bad_datagrams;
		fStats.rx_truncated += parseStats.truncated;
		if (result != ncm::PARSE_OK) {
			fStats.rx_bad_ntbs++;
			if (fStats.rx_bad_ntbs <= 10) {
				TRACE_ALWAYS("bad NTB (%d) of %" B_PRIuSIZE " bytes, %"
					B_PRIu32 " datagrams used\n", result, slot.length,
					count);
			}
		}
		if (count > fStats.rx_max_datagrams)
			fStats.rx_max_datagrams = count;

		if (count == 0) {
			_QueueRead(index);
			continue;
		}

		fRxCurrent = index;
		fRxNext = 0;
		fRxCount = count;
	}
}


/*!	Takes the next datagram, into a new net_buffer if \a _buffer is given,
	else into \a data (*_length bytes at most).
*/
status_t
NCMDevice::_Receive(bool block, net_buffer** _buffer, uint8* data,
	size_t* _length)
{
	status_t status = _NextDatagram(block);
	if (status != B_OK)
		return status;

	const ncm::datagram& datagram = fRxDatagrams[fRxNext++];
	const uint8* source = fRxSlots[fRxCurrent].buffer + datagram.offset;

	if (_buffer != NULL) {
		net_buffer* buffer = gBufferModule->create(0);
		if (buffer != NULL
			&& gBufferModule->append(buffer, source, datagram.length)
				!= B_OK) {
			gBufferModule->free(buffer);
			buffer = NULL;
		}
		if (buffer == NULL) {
			fStats.rx_no_buffers++;
			status = B_NO_MEMORY;
		}
		*_buffer = buffer;
	} else {
		size_t length = min_c(*_length, datagram.length);
		status = copy_to(data, source, length);
		*_length = length;
	}

	if (status == B_OK) {
		fStats.rx_datagrams++;
		fStats.rx_bytes += datagram.length;
	}

	if (fRxNext == fRxCount) {
		// the last one: the device can have the buffer back right away
		_QueueRead(fRxCurrent);
		fRxCurrent = -1;
	}
	return status;
}


status_t
NCMDevice::_ReceiveBatch(ether_receive_batch* batch)
{
	uint32 room = batch->count;
	batch->count = 0;
	if (room == 0)
		return B_BAD_VALUE;

	MutexLocker locker(fRxLock);

	status_t status;
	do {
		// at least one; then what is there already
		status = _Receive(true, &batch->buffers[0], NULL, NULL);
	} while (status == B_NO_MEMORY);
	if (status != B_OK)
		return status;

	uint32 count = 1;
	while (count < room) {
		status = _Receive(false, &batch->buffers[count], NULL, NULL);
		if (status == B_NO_MEMORY)
			continue;
		if (status != B_OK)
			break;
		count++;
	}

	batch->count = count;
	return B_OK;
}


status_t
NCMDevice::Read(uint8* buffer, size_t* numBytes)
{
	MutexLocker locker(fRxLock);
	status_t status = _Receive(true, NULL, buffer, numBytes);
	if (status != B_OK)
		*numBytes = 0;
	return status;
}


//	#pragma mark - sending


status_t
NCMDevice::_Send(net_buffer* buffer, const uint8* data, size_t length)
{
	if (fRemoved)
		return B_DEVICE_NOT_FOUND;
	if (length < ncm::kEthernetHeaderSize || length > fMaxFrame)
		return B_BAD_VALUE;

	MutexLocker locker(fTxLock);

	bigtime_t deadline = system_time() + 1000000;
	while (true) {
		if (!fTxRunning)
			return fRemoved ? B_DEVICE_NOT_FOUND : B_INTERRUPTED;

		if (fTxFill >= 0) {
			if (fTxWriter.CanAdd(length))
				break;
			if (fTxWriter.IsEmpty()) {
				// cannot happen: _Negotiate() made sure it fits
				return B_BAD_VALUE;
			}
			if (atomic_get(&fTxStalled) == 0) {
				_SubmitLocked(FLUSH_FULL);
				continue;
			}
			// full while the pipe stall is cleared: wait for that
		} else {
			int32 free = -1;
			for (uint32 i = 0; i < fTxTransfers; i++) {
				if (fTxSlots[i].state == TX_FREE) {
					free = i;
					break;
				}
			}
			if (free >= 0) {
				fTxFill = free;
				fTxSlots[free].state = TX_FILLING;
				fTxWriter.Start(fTxSlots[free].buffer);
				continue;
			}
		}

		// every buffer is on its way to the device
		fStats.tx_waits++;
		status_t status = fTxCondition.Wait(&fTxLock,
			B_ABSOLUTE_TIMEOUT | B_CAN_INTERRUPT, deadline);
		if (status == B_TIMED_OUT) {
			fStats.tx_dropped++;
			return B_TIMED_OUT;
		}
		if (status != B_OK && status != B_INTERRUPTED)
			return status;
	}

	uint8* target = fTxWriter.Add(length);
	status_t status;
	if (buffer != NULL)
		status = gBufferModule->read(buffer, 0, target, length);
	else {
		// kernel memory, see Write()
		memcpy(target, data, length);
		status = B_OK;
	}
	if (status != B_OK) {
		fTxWriter.RemoveLast();
		fStats.tx_dropped++;
		return status;
	}

	fStats.tx_datagrams++;
	fStats.tx_bytes += length;

	if (!fTxWriter.CanAdd(kMinFrame))
		_SubmitLocked(FLUSH_FULL);
	else if (fTxInFlight < fTxEager)
		_SubmitLocked(FLUSH_IDLE);

	return B_OK;
}


/*!	Sends the NTB being filled. Called with fTxLock held. */
void
NCMDevice::_SubmitLocked(flush_reason reason)
{
	// Nothing goes to a stalled pipe: the recovery thread sends the NTB
	// once the halt is cleared.
	if (fTxFill < 0 || atomic_get(&fTxStalled) != 0)
		return;

	tx_slot& slot = fTxSlots[fTxFill];
	fTxFill = -1;
	uint32 count = fTxWriter.Count();
	if (count == 0) {
		slot.state = TX_FREE;
		return;
	}

	size_t length = fTxWriter.Finish(fTxSequence++, fWriteMaxPacketSize,
		fDeviceTxMax);
	slot.datagrams = count;
	slot.state = TX_IN_FLIGHT;
	fTxInFlight++;

	atomic_add(&fPendingTransfers, 1);
	status_t status = gUSBModule->queue_bulk(fWriteEndpoint, slot.buffer,
		length, _WriteCallback, &slot);
	if (status != B_OK) {
		atomic_add(&fPendingTransfers, -1);
		slot.state = TX_FREE;
		fTxInFlight--;
		fStats.tx_errors++;
		fStats.tx_dropped += count;
		if (fStats.tx_errors <= 10)
			TRACE_ALWAYS("queueing a write failed: %s\n", strerror(status));
		return;
	}

	fStats.tx_ntbs++;
	if (count > fStats.tx_max_datagrams_seen)
		fStats.tx_max_datagrams_seen = count;
	switch (reason) {
		case FLUSH_IDLE:
			fStats.tx_flush_idle++;
			break;
		case FLUSH_FULL:
			fStats.tx_flush_full++;
			break;
		case FLUSH_COMPLETION:
			fStats.tx_flush_completion++;
			break;
	}
}


void
NCMDevice::_WriteCallback(void* cookie, int32 status, void* data,
	size_t actualLength)
{
	tx_slot* slot = (tx_slot*)cookie;
	NCMDevice* device = slot->device;

	bool stalled = false;
	{
		MutexLocker locker(device->fTxLock);
		if (slot->state == TX_IN_FLIGHT) {
			slot->state = TX_FREE;
			device->fTxInFlight--;
		}

		if (status != B_OK && status != B_CANCELED) {
			device->fStats.tx_errors++;
			device->fStats.tx_dropped += slot->datagrams;
			// The halt can only be cleared with a request, which cannot
			// complete while this thread (the controller's) waits for it.
			if (status == B_DEV_STALLED && device->fTxRunning
				&& atomic_get_and_set(&device->fTxStalled, 1) == 0) {
				stalled = true;
			}
			if (!device->fRemoved && device->fStats.tx_errors <= 10)
				TRACE_ALWAYS("write error 0x%08" B_PRIx32 "\n", status);
		}

		// what waited for this one to complete goes now
		if (device->fTxRunning && !device->fRemoved && status != B_CANCELED)
			device->_SubmitLocked(FLUSH_COMPLETION);

		device->fTxCondition.NotifyAll();
	}

	if (stalled)
		device->_ScheduleRecovery(RECOVER_WRITE);
	atomic_add(&device->fPendingTransfers, -1);
}


status_t
NCMDevice::Write(const uint8* buffer, size_t* numBytes)
{
	// Not copied from user memory under fTxLock: a page fault there could
	// need the controller's thread, which waits for the lock.
	uint8* copy = NULL;
	if (is_user_address(buffer)) {
		if (*numBytes > fMaxFrame) {
			*numBytes = 0;
			return B_BAD_VALUE;
		}
		copy = (uint8*)malloc(*numBytes);
		if (copy == NULL || user_memcpy(copy, buffer, *numBytes) != B_OK) {
			status_t status = copy == NULL ? B_NO_MEMORY : B_BAD_ADDRESS;
			free(copy);
			*numBytes = 0;
			return status;
		}
		buffer = copy;
	}

	status_t status = _Send(NULL, buffer, *numBytes);
	free(copy);
	if (status != B_OK)
		*numBytes = 0;
	return status;
}


//	#pragma mark - control


status_t
NCMDevice::GetInfo(void* buffer, size_t length)
{
	usb_ncm_info info;
	{
		MutexLocker txLocker(fTxLock);
		info = fStats;
	}
	info.version = USB_NCM_INFO_VERSION;
	info.size = sizeof(info);
	info.ntb_formats = fNTBFormats;
	info.ntb32 = fNTB32;
	info.rx_ntb_size = fRxNTBSize;
	info.tx_ntb_size = fTxNTBSize;
	info.device_rx_max = fDeviceRxMax;
	info.device_tx_max = fDeviceTxMax;
	info.tx_divisor = fTxDivisor;
	info.tx_remainder = fTxRemainder;
	info.tx_ndp_alignment = fTxAlignment;
	info.tx_max_datagrams = fTxMaxDatagrams;
	info.rx_transfers = fRxTransfers;
	info.tx_transfers = fTxTransfers;
	info.tx_eager = fTxEager;
	info.max_frame = fMaxFrame;
	info.link_up = fHasConnection;
	info.downstream_speed = fDownstreamSpeed;
	info.upstream_speed = fUpstreamSpeed;
	info.rx_queued = 0;
	for (uint32 i = 0; i < fRxTransfers; i++) {
		if (fRxSlots[i].queued)
			info.rx_queued++;
	}
	{
		InterruptsSpinLocker locker(fRxDoneLock);
		info.rx_completed = fRxDoneCount;
	}
	info.tx_in_flight = fTxInFlight;
	info.pending_transfers = atomic_get(&fPendingTransfers);

	return copy_to(buffer, &info, min_c(length, sizeof(info)));
}


status_t
NCMDevice::Control(uint32 op, void* buffer, size_t length)
{
	switch (op) {
		case ETHER_INIT:
			return B_OK;

		case ETHER_GETADDR:
			return copy_to(buffer, fMACAddress, sizeof(fMACAddress));

		case ETHER_GETFRAMESIZE:
		{
			uint32 frameSize = fMaxFrame;
			return copy_to(buffer, &frameSize, sizeof(frameSize));
		}

		case ETHER_NONBLOCK:
		{
			int32 value;
			status_t status = copy_from(&value, buffer, sizeof(value));
			if (status == B_OK)
				fNonBlocking = value != 0;
			return status;
		}

		case ETHER_SETPROMISC:
		{
			int32 value;
			status_t status = copy_from(&value, buffer, sizeof(value));
			if (status != B_OK)
				return status;
			fPromiscuous = value != 0;
			return fOpen && !fRemoved ? _SetPacketFilter() : B_OK;
		}

		case ETHER_ADDMULTI:
		case ETHER_REMMULTI:
			// every multicast frame passes the filter
			return B_OK;

		case ETHER_SET_LINK_STATE_SEM:
			return copy_from(&fLinkStateChangeSem, buffer, sizeof(sem_id));

		case ETHER_GET_LINK_STATE:
		{
			ether_link_state_t state;
			state.media = IFM_ETHER | IFM_FULL_DUPLEX
				| (fHasConnection ? IFM_ACTIVE : 0);
			state.quality = 1000;
			state.speed = fDownstreamSpeed;
			return copy_to(buffer, &state, sizeof(state));
		}

		case ETHER_SEND_NET_BUFFER:
		{
			if (buffer == NULL || length == 0)
				return B_BAD_DATA;
			if (!IS_KERNEL_ADDRESS(buffer))
				return B_BAD_ADDRESS;
			net_buffer* netBuffer = (net_buffer*)buffer;
			status_t status = _Send(netBuffer, NULL, netBuffer->size);
			if (status == B_OK)
				gBufferModule->free(netBuffer);
			return status;
		}

		case ETHER_RECEIVE_NET_BUFFER:
		{
			if (buffer == NULL || length == 0)
				return B_BAD_DATA;
			if (!IS_KERNEL_ADDRESS(buffer))
				return B_BAD_ADDRESS;
			MutexLocker locker(fRxLock);
			status_t status;
			do {
				status = _Receive(true, (net_buffer**)buffer, NULL, NULL);
			} while (status == B_NO_MEMORY);
			return status;
		}

		case ETHER_RECEIVE_NET_BUFFERS:
		{
			ether_receive_batch* batch = (ether_receive_batch*)buffer;
			if (buffer == NULL || length < sizeof(ether_receive_batch))
				return B_BAD_DATA;
			if (!IS_KERNEL_ADDRESS(buffer)
				|| !IS_KERNEL_ADDRESS(batch->buffers)) {
				return B_BAD_ADDRESS;
			}
			return _ReceiveBatch(batch);
		}

		case USB_NCM_GET_INFO:
			return GetInfo(buffer, length);

		default:
			// The stack asks every driver for statistics (SIOCGIFSTATS) and
			// more; not knowing them is not worth a syslog line each time.
			TRACE("unsupported ioctl %" B_PRIu32 "\n", op);
	}

	return B_DEV_INVALID_IOCTL;
}


//	#pragma mark - notifications


void
NCMDevice::_NotifyCallback(void* cookie, int32 status, void* data,
	size_t actualLength)
{
	NCMDevice* device = (NCMDevice*)cookie;
	atomic_add(&device->fInsideNotify, 1);
	if (status == B_CANCELED || device->fRemoved
		|| atomic_get(&device->fNotifyStopping) != 0) {
		atomic_add(&device->fInsideNotify, -1);
		atomic_add(&device->fPendingTransfers, -1);
		return;
	}

	if (status == B_OK && actualLength >= sizeof(cdc_notification)) {
		bool linkStateChange = false;
		const cdc_notification* notification
			= (const cdc_notification*)device->fNotifyBuffer;
		device->fStats.notifications++;

		switch (notification->notification_code) {
			case CDC_NOTIFY_NETWORK_CONNECTION:
			{
				bool hasConnection
					= B_LENDIAN_TO_HOST_INT16(notification->value) != 0;
				linkStateChange = hasConnection != device->fHasConnection;
				device->fHasConnection = hasConnection;
				if (linkStateChange) {
					TRACE_ALWAYS("link %s\n", hasConnection ? "up" : "down");
				}
				break;
			}

			case CDC_NOTIFY_CONNECTION_SPEED_CHANGE:
			{
				// DLBitRate, then ULBitRate (CDC 1.2, 6.3.3)
				if (actualLength < sizeof(cdc_notification) + 8) {
					TRACE_ALWAYS("short connection speed notification\n");
					break;
				}
				uint32 downstream
					= B_LENDIAN_TO_HOST_INT32(*(uint32*)&notification->data[0]);
				uint32 upstream
					= B_LENDIAN_TO_HOST_INT32(*(uint32*)&notification->data[4]);
				linkStateChange = downstream != device->fDownstreamSpeed
					|| upstream != device->fUpstreamSpeed;
				device->fDownstreamSpeed = downstream;
				device->fUpstreamSpeed = upstream;
				if (linkStateChange) {
					TRACE_ALWAYS("speed %" B_PRIu32 "/%" B_PRIu32 " Mbit/s\n",
						downstream / 1000000, upstream / 1000000);
				}
				break;
			}

			default:
				TRACE_ALWAYS("unsupported notification 0x%02x\n",
					notification->notification_code);
				break;
		}

		// Some functions repeat their notifications: only a change is worth
		// waking the stack's link checker for.
		if (linkStateChange && device->fLinkStateChangeSem >= B_OK) {
			release_sem_etc(device->fLinkStateChangeSem, 1,
				B_DO_NOT_RESCHEDULE);
		}
	}

	if (status != B_OK) {
		TRACE_ALWAYS("notification status error 0x%08" B_PRIx32 "\n", status);
		if (status == B_DEV_STALLED) {
			// The halt can only be cleared with a request, which cannot
			// complete while this thread (the controller's) waits for it:
			// the recovery thread clears it and queues the next one.
			atomic_add(&device->fInsideNotify, -1);
			device->_ScheduleRecovery(RECOVER_NOTIFY);
			atomic_add(&device->fPendingTransfers, -1);
			return;
		}
		// other errors mostly mean that the device is going away
		snooze(1000);
	}

	device->_QueueNotify();
	atomic_add(&device->fInsideNotify, -1);
	atomic_add(&device->fPendingTransfers, -1);
}


status_t
NCMDevice::_QueueNotify()
{
	if (fRemoved || atomic_get(&fNotifyStopping) != 0)
		return B_INTERRUPTED;

	atomic_add(&fPendingTransfers, 1);
	status_t status = gUSBModule->queue_interrupt(fNotifyEndpoint,
		fNotifyBuffer, fNotifyBufferLength, _NotifyCallback, this);
	if (status != B_OK)
		atomic_add(&fPendingTransfers, -1);
	return status;
}


/*!	Keeps the notification hook and the recovery thread from queueing
	anything more, and waits until they are done.
*/
void
NCMDevice::_StopNotifications()
{
	atomic_set(&fNotifyStopping, 1);

	thread_id thread;
	{
		MutexLocker locker(fRecoveryLock);
		thread = fRecoveryThread;
		fRecoveryFlags = 0;
	}
	if (thread >= 0)
		wait_for_thread(thread, NULL);

	while (atomic_get(&fInsideNotify) != 0)
		snooze(100);
}


/*!	Waits (a while) until no transfer can complete into our buffers anymore:
	cancelled transfers call back with B_CANCELED, but one that completed
	just before may still be on its way through the controller's thread.
*/
void
NCMDevice::_WaitForTransfers()
{
	bigtime_t timeout = system_time() + 1000000;
	while (atomic_get(&fPendingTransfers) > 0 && system_time() < timeout)
		snooze(1000);
	if (atomic_get(&fPendingTransfers) > 0 && !fRemoved) {
		TRACE_ALWAYS("%" B_PRId32 " transfers still pending\n",
			atomic_get(&fPendingTransfers));
	}
}


void
NCMDevice::_ScheduleRecovery(uint32 what)
{
	MutexLocker locker(fRecoveryLock);
	if (fRemoved || atomic_get(&fNotifyStopping) != 0)
		return;

	fRecoveryFlags |= what;
	if (fRecoveryThread >= 0)
		return;

	fRecoveryThread = spawn_kernel_thread(_RecoveryThread,
		DRIVER_NAME " recovery", B_NORMAL_PRIORITY, this);
	if (fRecoveryThread < 0) {
		TRACE_ALWAYS("cannot start the recovery thread\n");
		fRecoveryThread = -1;
		fRecoveryFlags = 0;
		return;
	}
	resume_thread(fRecoveryThread);
}


/*!	Clears the halt of a stalled pipe, which takes a control request: that
	cannot be made in a completion hook, which runs in the thread that would
	have to complete the request.
*/
status_t
NCMDevice::_RecoveryThread(void* data)
{
	NCMDevice* device = (NCMDevice*)data;

	while (true) {
		uint32 flags;
		{
			MutexLocker locker(device->fRecoveryLock);
			flags = device->fRecoveryFlags;
			device->fRecoveryFlags = 0;
			if (flags == 0 || device->fRemoved
				|| atomic_get(&device->fNotifyStopping) != 0) {
				device->fRecoveryThread = -1;
				return B_OK;
			}
		}

		if ((flags & RECOVER_NOTIFY) != 0) {
			if (gUSBModule->clear_feature(device->fNotifyEndpoint,
					USB_FEATURE_ENDPOINT_HALT) != B_OK) {
				TRACE_ALWAYS("clearing the notification pipe halt failed\n");
			}
			// not again at once, should it keep stalling
			snooze(10000);
			atomic_add(&device->fInsideNotify, 1);
			device->_QueueNotify();
			atomic_add(&device->fInsideNotify, -1);
		}

		if ((flags & RECOVER_WRITE) != 0) {
			if (gUSBModule->clear_feature(device->fWriteEndpoint,
					USB_FEATURE_ENDPOINT_HALT) != B_OK) {
				TRACE_ALWAYS("clearing the write pipe halt failed\n");
			}
			snooze(1000);
			MutexLocker locker(device->fTxLock);
			atomic_set(&device->fTxStalled, 0);
			if (device->fTxRunning && !device->fRemoved)
				device->_SubmitLocked(FLUSH_COMPLETION);
			device->fTxCondition.NotifyAll();
		}
	}
}


//	#pragma mark - Realtek RTL815x PHY power


/*!	The Realtek RTL815x adapters keep their PHY powered, and so their link and
	its lights, while nothing uses them; usb_ecm powers it down while its
	interface is down, which a later boot may find still so. Their vendor
	register requests (as Linux' r8152 uses them) work in the CDC
	configurations: the PHY is powered up at open and down at close here as
	well. Suspended (U3), the adapter powers the PHY up on its own: the bus
	keeps it awake meanwhile.
*/
bool
NCMDevice::_HasRealtekPHY() const
{
	if (fVendorID != 0x0bda)
		return false;

	switch (fProductID) {
		case 0x8152:
		case 0x8153:
		case 0x8155:
		case 0x8156:
		case 0x8157:
			return true;
		default:
			return false;
	}
}


static const uint8 kRealtekRequestRegisters = 0x05;
static const uint16 kRealtekTypePLA = 0x0100;
static const uint16 kRealtekPHYBase = 0xe86c;
	// PLA_OCP_GPHY_BASE
static const uint16 kRealtekMIIBase = 0xa400;
	// OCP_BASE_MII
static const uint16 kBMCRPowerDown = 0x0800;
static const uint16 kBMCRAutoNegotiate = 0x1000;
static const uint16 kBMCRRestartAutoNegotiation = 0x0200;
static const uint16 kBMSRLinkStatus = 0x0004;


status_t
NCMDevice::_RealtekRead(uint16 index, uint32* value)
{
	size_t actualLength = 0;
	status_t result = gUSBModule->send_request(fDevice,
		USB_REQTYPE_VENDOR | USB_REQTYPE_DEVICE_IN, kRealtekRequestRegisters,
		index & ~3, kRealtekTypePLA, sizeof(uint32), value, &actualLength);
	if (result == B_OK && actualLength != sizeof(uint32))
		result = B_IO_ERROR;
	return result;
}


status_t
NCMDevice::_RealtekWriteWord(uint16 index, uint16 value)
{
	// a 32 bit write with the bytes of the word enabled
	uint16 byteEnable = 0x33;
	uint32 data = value;
	if ((index & 2) != 0) {
		byteEnable <<= 2;
		data <<= 16;
	}

	size_t actualLength = 0;
	return gUSBModule->send_request(fDevice,
		USB_REQTYPE_VENDOR | USB_REQTYPE_DEVICE_OUT, kRealtekRequestRegisters,
		index & ~3, kRealtekTypePLA | byteEnable, sizeof(uint32), &data,
		&actualLength);
}


status_t
NCMDevice::_RealtekPHYRead(uint16 reg, uint16* value)
{
	uint16 address = kRealtekMIIBase + reg * 2;
	status_t result = _RealtekWriteWord(kRealtekPHYBase, address & 0xf000);
	if (result != B_OK)
		return result;

	uint16 index = (address & 0x0fff) | 0xb000;
	uint32 data;
	result = _RealtekRead(index, &data);
	if (result == B_OK)
		*value = (index & 2) != 0 ? data >> 16 : data & 0xffff;
	return result;
}


status_t
NCMDevice::_RealtekPHYWrite(uint16 reg, uint16 value)
{
	uint16 address = kRealtekMIIBase + reg * 2;
	status_t result = _RealtekWriteWord(kRealtekPHYBase, address & 0xf000);
	if (result != B_OK)
		return result;

	return _RealtekWriteWord((address & 0x0fff) | 0xb000, value);
}


void
NCMDevice::_SetPHYPower(bool on)
{
	if (fRemoved || !_HasRealtekPHY())
		return;

	uint16 control;
	if (_RealtekPHYRead(0, &control) != B_OK) {
		TRACE_ALWAYS("cannot read the PHY control register\n");
		return;
	}

	if (on) {
		if ((control & kBMCRPowerDown) == 0) {
			_SetKeepAwake(false);
			return;
		}
		control &= ~kBMCRPowerDown;
		control |= kBMCRAutoNegotiate | kBMCRRestartAutoNegotiation;
	} else {
		// Without a link there are no lights to turn off, and a PHY powered
		// up again would not find a link to wait for: leave it alone.
		uint16 status;
		if (_RealtekPHYRead(1, &status) != B_OK
			|| _RealtekPHYRead(1, &status) != B_OK
			|| (status & kBMSRLinkStatus) == 0) {
			return;
		}
		control |= kBMCRPowerDown;
	}

	if (_RealtekPHYWrite(0, control) != B_OK) {
		TRACE_ALWAYS("cannot power the PHY %s\n", on ? "up" : "down");
		return;
	}
	_SetKeepAwake(!on);
	if (!on)
		return;

	// The data interface reset only brings the frames back with a link: wait
	// for it (an adapter without a cable gives up after a while).
	bigtime_t timeout = system_time() + 5000000;
	while (system_time() < timeout) {
		uint16 status;
		// the link bit latches low: the second read tells the state
		if (_RealtekPHYRead(1, &status) == B_OK
			&& _RealtekPHYRead(1, &status) == B_OK
			&& (status & kBMSRLinkStatus) != 0) {
			return;
		}
		snooze(100000);
	}
	TRACE_ALWAYS("no link after powering the PHY up\n");
}


void
NCMDevice::_SetKeepAwake(bool keepAwake)
{
	if (keepAwake == fKeepAwake)
		return;

	usb_keep_awake_args args = { fDevice, keepAwake };
	if (gUSBModule->usb_ioctl(B_USB_KEEP_AWAKE, &args, sizeof(args)) == B_OK)
		fKeepAwake = keepAwake;
}
