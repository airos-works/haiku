/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */
#ifndef _LE_ATTRIBUTE_CLIENT_H_
#define _LE_ATTRIBUTE_CLIENT_H_

#include <SupportDefs.h>
#include <bluetooth/bluetooth.h>

#include <vector>
#include <deque>


namespace Bluetooth {

struct LEPrimaryService {
	uint16 startHandle;
	uint16 endHandle;
	uint16 uuid16; // Zero for a 128-bit UUID.
};

struct LECharacteristic {
	uint16 declarationHandle;
	uint16 valueHandle;
	uint8 properties;
	uint16 uuid16; // Zero for a 128-bit UUID.
};


// Synchronous ATT client on the LE fixed L2CAP channel. One request may be
// outstanding at a time. Connect() has the kernel exchange MTUs with the
// peer, offering 517 bytes; MTU() is the result (23 bytes when the peer, or
// the kernel, does not exchange).
class LEAttributeClient {
public:
	LEAttributeClient();
	~LEAttributeClient();

	status_t Connect(const bdaddr_t& address);
	void AdoptConnectedSocket(int descriptor);
	void Disconnect();
	bool IsConnected() const { return fSocket >= 0; }
	uint8 LastATTError() const { return fLastATTError; }
	uint16 MTU() const { return fMTU; }

	status_t DiscoverPrimaryServices(std::vector<LEPrimaryService>& services);
	status_t DiscoverCharacteristics(uint16 startHandle, uint16 endHandle,
		std::vector<LECharacteristic>& characteristics);
	status_t FindClientConfiguration(uint16 startHandle, uint16 endHandle,
		uint16& handle);
	status_t FindDescriptor(uint16 startHandle, uint16 endHandle,
		uint16 uuid16, uint16& handle);
	status_t ReadAttribute(uint16 handle, std::vector<uint8>& value);
	status_t WriteAttribute(uint16 handle, const uint8* value, size_t length);
	status_t ReadNotification(uint16& handle, std::vector<uint8>& value);

private:
	struct Notification {
		uint16 handle;
		std::vector<uint8> value;
	};

	status_t _Exchange(const uint8* request, size_t requestLength,
		uint8 expectedOpcode, std::vector<uint8>& response);
	status_t _Receive(std::vector<uint8>& response);
	bool _HandlePeerPDU(const std::vector<uint8>& pdu);

	void _UpdateMTU();

	int fSocket;
	uint8 fLastATTError;
	uint16 fMTU;
		// (in what was padding, so that the class keeps its size)
	std::deque<Notification> fNotifications;
};

} // namespace Bluetooth

#endif
