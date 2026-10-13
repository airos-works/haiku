/*
 * Copyright 2007-2009 Oliver Ruiz Dorantes, oliver.ruiz.dorantes_at_gmail.com
 * Copyright 2008 Mika Lindqvist, monni1995_at_gmail.com
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <String.h>

#include <bluetooth/L2CAP/btL2CAP.h>
#include <sdp.h>

#include <vector>

#include "Debug.h"
#include "SDPServer.h"


// #pragma mark - service records


/*!	The services this device offers, as SDP records (Core Vol 3 Part B).
	Only 16-bit UUIDs are used. A2DP 1.3 section 5.3 gives the Audio Source
	record.
*/
namespace {

typedef std::vector<uint8> Bytes;

struct Attribute {
	uint16	id;
	Bytes	value;
};

struct Record {
	uint32					handle;
	std::vector<Attribute>	attributes;
	std::vector<uint16>		uuids;
		// every UUID in the record, for service searches
};


void
put16(Bytes& data, uint16 value)
{
	data.push_back(value >> 8);
	data.push_back(value & 0xff);
}


void
put32(Bytes& data, uint32 value)
{
	put16(data, value >> 16);
	put16(data, value & 0xffff);
}


Bytes
uint16_element(uint16 value)
{
	Bytes data;
	data.push_back(SDP_DATA_UINT16);
	put16(data, value);
	return data;
}


Bytes
uint32_element(uint32 value)
{
	Bytes data;
	data.push_back(SDP_DATA_UINT32);
	put32(data, value);
	return data;
}


Bytes
uuid_element(uint16 uuid)
{
	Bytes data;
	data.push_back(SDP_DATA_UUID16);
	put16(data, uuid);
	return data;
}


Bytes
string_element(const char* text)
{
	Bytes data;
	data.push_back(SDP_DATA_STR8);
	data.push_back(strlen(text));
	data.insert(data.end(), text, text + strlen(text));
	return data;
}


Bytes
sequence(const std::vector<Bytes>& elements)
{
	Bytes contents;
	for (size_t i = 0; i < elements.size(); i++)
		contents.insert(contents.end(), elements[i].begin(), elements[i].end());
	Bytes data;
	if (contents.size() < 256) {
		data.push_back(SDP_DATA_SEQ8);
		data.push_back(contents.size());
	} else {
		data.push_back(SDP_DATA_SEQ16);
		put16(data, contents.size());
	}
	data.insert(data.end(), contents.begin(), contents.end());
	return data;
}


void
add(Record& record, uint16 id, const Bytes& value)
{
	Attribute attribute;
	attribute.id = id;
	attribute.value = value;
	record.attributes.push_back(attribute);
}


const std::vector<Record>&
records()
{
	static std::vector<Record> sRecords;
	if (!sRecords.empty())
		return sRecords;

	// The SDP server itself.
	Record server;
	server.handle = 0;
	add(server, SDP_ATTR_SERVICE_RECORD_HANDLE, uint32_element(0));
	add(server, SDP_ATTR_SERVICE_CLASS_ID_LIST, sequence({
		uuid_element(SDP_SERVICE_CLASS_SERVICE_DISCOVERY_SERVER) }));
	server.uuids = { SDP_SERVICE_CLASS_SERVICE_DISCOVERY_SERVER, 0x0001,
		0x0100 };
	sRecords.push_back(server);

	// Audio Source: L2CAP PSM 0x19, AVDTP 1.3, A2DP 1.3, a player.
	Record source;
	source.handle = 0x00010001;
	add(source, SDP_ATTR_SERVICE_RECORD_HANDLE, uint32_element(source.handle));
	add(source, SDP_ATTR_SERVICE_CLASS_ID_LIST, sequence({
		uuid_element(SDP_SERVICE_CLASS_AUDIO_SOURCE) }));
	add(source, SDP_ATTR_PROTOCOL_DESCRIPTOR_LIST, sequence({
		sequence({ uuid_element(0x0100), uint16_element(0x0019) }),
		sequence({ uuid_element(0x0019), uint16_element(0x0103) }) }));
	add(source, SDP_ATTR_BROWSE_GROUP_LIST, sequence({
		uuid_element(SDP_SERVICE_CLASS_PUBLIC_BROWSE_GROUP) }));
	add(source, SDP_ATTR_LANGUAGE_BASE_ATTRIBUTE_ID_LIST, sequence({
		uint16_element(0x656e), uint16_element(0x006a),
		uint16_element(0x0100) }));
	add(source, SDP_ATTR_BLUETOOTH_PROFILE_DESCRIPTOR_LIST, sequence({
		sequence({ uuid_element(SDP_SERVICE_CLASS_ADVANCED_AUDIO_DISTRIBUTION),
			uint16_element(0x0103) }) }));
	add(source, 0x0100, string_element("Audio Source"));
	add(source, SDP_ATTR_SUPPORTED_FEATURES, uint16_element(0x0001));
	source.uuids = { SDP_SERVICE_CLASS_AUDIO_SOURCE, 0x0100, 0x0019,
		SDP_SERVICE_CLASS_PUBLIC_BROWSE_GROUP,
		SDP_SERVICE_CLASS_ADVANCED_AUDIO_DISTRIBUTION };
	sRecords.push_back(source);

	return sRecords;
}


/*!	One data element of a request: type, where its value starts, its size,
	and the size of the whole element.
*/
struct Element {
	uint8			type;
	const uint8*	value;
	size_t			size;
	size_t			total;
};


bool
read_element(const uint8* data, size_t length, Element& element)
{
	if (length < 1)
		return false;
	element.type = data[0] >> 3;
	const uint8 index = data[0] & 7;
	size_t header = 1;
	size_t size;
	if (element.type == 0)
		size = 0;
	else if (index < 5)
		size = 1 << index;
	else {
		const size_t lengthBytes = 1 << (index - 5);
		if (length < 1 + lengthBytes)
			return false;
		size = 0;
		for (size_t i = 0; i < lengthBytes; i++)
			size = (size << 8) | data[1 + i];
		header += lengthBytes;
	}
	if (header + size > length)
		return false;
	element.value = data + header;
	element.size = size;
	element.total = header + size;
	return true;
}


uint32
element_unsigned(const Element& element)
{
	uint32 value = 0;
	for (size_t i = 0; i < element.size && i < 4; i++)
		value = (value << 8) | element.value[i];
	return value;
}


/*!	The UUIDs of a search pattern, as 16-bit values; a UUID not on the
	Bluetooth base becomes 0, which matches nothing.
*/
bool
read_pattern(const Element& sequence, std::vector<uint16>& uuids)
{
	if (sequence.type != 6)
		return false;
	const uint8* data = sequence.value;
	size_t left = sequence.size;
	while (left > 0) {
		Element uuid;
		if (!read_element(data, left, uuid) || uuid.type != 3)
			return false;
		if (uuid.size == 2 || uuid.size == 4)
			uuids.push_back(element_unsigned(uuid) & 0xffff);
		else if (uuid.size == 16) {
			static const uint8 kBase[12] = { 0x00, 0x00, 0x10, 0x00, 0x80,
				0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb };
			bool base = memcmp(uuid.value + 4, kBase, 12) == 0
				&& uuid.value[0] == 0 && uuid.value[1] == 0;
			uuids.push_back(base ? (uuid.value[2] << 8) | uuid.value[3] : 0);
		}
		data += uuid.total;
		left -= uuid.total;
	}
	return !uuids.empty();
}


bool
matches(const Record& record, const std::vector<uint16>& pattern)
{
	for (size_t i = 0; i < pattern.size(); i++) {
		bool found = false;
		for (size_t j = 0; j < record.uuids.size() && !found; j++)
			found = record.uuids[j] == pattern[i];
		if (!found)
			return false;
	}
	return true;
}


/*!	The attributes of a record that a list of IDs and ID ranges asks for,
	as a sequence of ID and value pairs.
*/
bool
attribute_list(const Record& record, const Element& ids, Bytes& list)
{
	if (ids.type != 6)
		return false;

	std::vector<Bytes> pairs;
	for (size_t a = 0; a < record.attributes.size(); a++) {
		const Attribute& attribute = record.attributes[a];
		const uint8* data = ids.value;
		size_t left = ids.size;
		bool wanted = false;
		while (left > 0 && !wanted) {
			Element id;
			if (!read_element(data, left, id) || id.type != 1)
				return false;
			const uint32 value = element_unsigned(id);
			if (id.size == 2)
				wanted = attribute.id == value;
			else if (id.size == 4) {
				wanted = attribute.id >= (value >> 16)
					&& attribute.id <= (value & 0xffff);
			}
			data += id.total;
			left -= id.total;
		}
		if (wanted) {
			pairs.push_back(uint16_element(attribute.id));
			pairs.push_back(attribute.value);
		}
	}
	list = sequence(pairs);
	return true;
}

} // namespace


void
SDPServer::_HandleClient(int client)
{
	timeval timeout = { 30, 0 };
	setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

	// A response bigger than what the client takes at once continues in
	// the next request; the continuation state is the offset reached.
	Bytes pending;
	uint8 request[1024];
	while (true) {
		ssize_t size = recv(client, request, sizeof(request), 0);
		if (size <= 0)
			break;
		if (size < 5)
			continue;

		const uint8 pdu = request[0];
		const uint16 transaction = (request[1] << 8) | request[2];
		const size_t parameterLength = (request[3] << 8) | request[4];
		const uint8* parameters = request + 5;
		size_t left = min_c((size_t)size - 5, parameterLength);

		Bytes reply;
		uint16 error = 0;
		uint8 replyPDU = 0;

		Element first;
		std::vector<uint16> pattern;
		if (pdu == SDP_PDU_SERVICE_SEARCH_REQUEST) {
			// Pattern, maximum record count, continuation.
			if (!read_element(parameters, left, first)
				|| !read_pattern(first, pattern) || left < first.total + 2) {
				error = SDP_ERROR_CODE_INVALID_REQUEST_SYNTAX;
			} else {
				const uint16 maximum = (parameters[first.total] << 8)
					| parameters[first.total + 1];
				Bytes handles;
				uint16 count = 0;
				for (size_t i = 0; i < records().size() && count < maximum;
						i++) {
					if (matches(records()[i], pattern)) {
						put32(handles, records()[i].handle);
						count++;
					}
				}
				replyPDU = SDP_PDU_SERVICE_SEARCH_RESPONSE;
				put16(reply, count);
				put16(reply, count);
				reply.insert(reply.end(), handles.begin(), handles.end());
				reply.push_back(0);
			}
		} else if (pdu == SDP_PDU_SERVICE_ATTRIBUTE_REQUEST
			|| pdu == SDP_PDU_SERVICE_SEARCH_ATTRIBUTE_REQUEST) {
			// Handle or pattern, maximum byte count, attribute IDs,
			// continuation.
			size_t offset;
			const Record* single = NULL;
			if (pdu == SDP_PDU_SERVICE_ATTRIBUTE_REQUEST) {
				offset = 4;
				if (left >= 4) {
					const uint32 handle = (parameters[0] << 24)
						| (parameters[1] << 16) | (parameters[2] << 8)
						| parameters[3];
					for (size_t i = 0; i < records().size(); i++) {
						if (records()[i].handle == handle)
							single = &records()[i];
					}
					if (single == NULL)
						error = SDP_ERROR_CODE_INVALID_SERVICE_RECORD_HANDLE;
				} else
					error = SDP_ERROR_CODE_INVALID_REQUEST_SYNTAX;
			} else {
				if (!read_element(parameters, left, first)
					|| !read_pattern(first, pattern))
					error = SDP_ERROR_CODE_INVALID_REQUEST_SYNTAX;
				offset = first.total;
			}

			Element ids;
			if (error == 0 && (left < offset + 2
				|| !read_element(parameters + offset + 2,
					left - offset - 2, ids))) {
				error = SDP_ERROR_CODE_INVALID_REQUEST_SYNTAX;
			}

			if (error == 0) {
				const uint16 maximum = (parameters[offset] << 8)
					| parameters[offset + 1];
				const uint8* state = parameters + offset + 2 + ids.total;
				const bool continuing = state < parameters + left
					&& state[0] == 2 && state + 3 <= parameters + left;
				size_t start = 0;

				Bytes full;
				if (continuing) {
					full = pending;
					start = (state[1] << 8) | state[2];
				} else if (single != NULL) {
					if (!attribute_list(*single, ids, full))
						error = SDP_ERROR_CODE_INVALID_REQUEST_SYNTAX;
				} else {
					std::vector<Bytes> lists;
					for (size_t i = 0; i < records().size(); i++) {
						if (!matches(records()[i], pattern))
							continue;
						Bytes list;
						if (!attribute_list(records()[i], ids, list)) {
							error = SDP_ERROR_CODE_INVALID_REQUEST_SYNTAX;
							break;
						}
						lists.push_back(list);
					}
					full = sequence(lists);
				}

				if (error == 0 && start > full.size())
					error = SDP_ERROR_CODE_INVALID_CONTINUATION_STATE;
				if (error == 0) {
					// Stay well inside the default 672-byte MTU.
					size_t chunk = min_c((size_t)maximum, (size_t)600);
					chunk = min_c(chunk, full.size() - start);
					replyPDU = pdu + 1;
					put16(reply, chunk);
					reply.insert(reply.end(), full.begin() + start,
						full.begin() + start + chunk);
					if (start + chunk < full.size()) {
						reply.push_back(2);
						put16(reply, start + chunk);
						pending = full;
					} else
						reply.push_back(0);
				}
			}
		} else
			error = SDP_ERROR_CODE_INVALID_REQUEST_SYNTAX;

		if (error != 0) {
			replyPDU = SDP_PDU_ERROR_RESPONSE;
			reply.clear();
			put16(reply, error);
		}

		Bytes packet;
		packet.push_back(replyPDU);
		put16(packet, transaction);
		put16(packet, reply.size());
		packet.insert(packet.end(), reply.begin(), reply.end());
		if (send(client, packet.data(), packet.size(), 0) < 0)
			break;
	}

	close(client);
}


/*static*/ status_t
SDPServer::_ClientThread(void* cookie)
{
	_HandleClient((int)(addr_t)cookie);
	return B_OK;
}


SDPServer::SDPServer()
	:
	fThreadID(-1),
	fServerSocket(-1),
	fIsShuttingDown(false)
{
}


SDPServer::~SDPServer()
{
	Stop();
}


status_t
SDPServer::Start()
{
	if (fThreadID > 0) {
		// Already running
		return B_OK;
	}

	fIsShuttingDown = false;

	// Spawn the SDP thread
	fThreadID = spawn_thread(_ListenThread, "SDP server thread", B_NORMAL_PRIORITY, this);
	if (fThreadID < 0) {
		TRACE_BT("SDP: Failed launching the SDP server thread\n");
		return fThreadID;
	}

	return resume_thread(fThreadID);
}


void
SDPServer::Stop()
{
	if (fThreadID > 0) {
		fIsShuttingDown = true;

		// Close the socket to unblock the accept() call
		if (fServerSocket > 0) {
			close(fServerSocket);
			fServerSocket = -1;
		}

		status_t threadReturnStatus;
		wait_for_thread(fThreadID, &threadReturnStatus);
		TRACE_BT("SDP: Server thread exited with: %s\n", strerror(threadReturnStatus));

		fThreadID = -1;
	}
}


int32
SDPServer::_ListenThread(void* data)
{
	SDPServer* server = (SDPServer*)data;
	return server->_Run();
}


int32
SDPServer::_Run()
{
	// Set up the SDP socket
	struct sockaddr_l2cap loc_addr = {0};
	status_t status;

	TRACE_BT("SDP: SDP server thread up...\n");

	fServerSocket = socket(PF_BLUETOOTH, SOCK_SEQPACKET, BLUETOOTH_PROTO_L2CAP);

	if (fServerSocket < 0) {
		TRACE_BT("SDP: Could not create server socket ...\n");
		return B_ERROR;
	}

	// bind socket to port 0x1001 of the first available
	// bluetooth adapter
	loc_addr.l2cap_family = AF_BLUETOOTH;
	loc_addr.l2cap_bdaddr = BDADDR_ANY;
	loc_addr.l2cap_psm = B_HOST_TO_LENDIAN_INT16(1);
	loc_addr.l2cap_len = sizeof(struct sockaddr_l2cap);

	status = bind(fServerSocket, (struct sockaddr*)&loc_addr, sizeof(struct sockaddr_l2cap));

	if (status < 0) {
		TRACE_BT("SDP: Could not bind server socket (%s)...\n", strerror(status));
		close(fServerSocket);
		fServerSocket = -1;
		return status;
	}

	// Listen for up to 10 connections
	status = listen(fServerSocket, 10);

	if (status != B_OK) {
		TRACE_BT("SDP: Could not listen server socket (%s)...\n", strerror(status));
		close(fServerSocket);
		fServerSocket = -1;
		return status;
	}

	while (!fIsShuttingDown) {
		uint len = sizeof(struct sockaddr_l2cap);
		int client = accept(fServerSocket, (struct sockaddr*)&loc_addr, &len);

		// Check if we woke up because of shutdown
		if (fIsShuttingDown) {
			if (client > 0)
				close(client);
			break;
		}

		if (client < 0) {
			TRACE_BT("SDP: Error accepting connection\n");
			snooze(50000);
			continue;
		}

		// A client asks a few questions and goes; one that does not answer
		// must not hold up the others.
		thread_id thread = spawn_thread(&_ClientThread, "SDP client",
			B_NORMAL_PRIORITY, (void*)(addr_t)client);
		if (thread < 0 || resume_thread(thread) != B_OK)
			close(client);
	}

	return B_NO_ERROR;
}
