/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	Host test of L2CAP's l2cap_receive_data(): the function itself (and
	connection_for()), cut out of l2cap.cpp by run.sh (RECEIVE_DATA_SOURCE),
	against stand-ins for the network stack, the HCI layer and the endpoints.
	What it checks (haiku#185): data for a channel is only taken from the
	link the channel is on, and the received buffer is passed on or freed on
	every path: the ACL layer forgets it whatever comes back.
*/


#include <endian.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>


// #pragma mark - what the function needs


typedef int32_t status_t;
typedef int32_t int32;
typedef uint32_t uint32;
typedef uint16_t uint16;
typedef uint8_t uint8;
typedef int64_t bigtime_t;
typedef int32_t hci_id;

#define B_OK			0
#define B_ERROR			(-1)
#define B_BAD_VALUE		(-2147483643)
#define B_PRIu32		"u"
#define _PACKED			__attribute__((packed))

#define TRACE(x...)
#define ERROR(x...)
#define CALLED()
#define L2CAP_LE_TRACE(x...)
#define dprintf(x...)	(void)0

#define L2CAP_SIGNALING_CID			0x0001
#define L2CAP_CONNECTIONLESS_CID	0x0002
#define L2CAP_ATT_CID				0x0004
#define L2CAP_LE_SIGNALING_CID		0x0005
#define L2CAP_SMP_CID				0x0006

struct bdaddr_t {
	uint8 b[6];
};

struct sockaddr {
	uint8 data[32];
};

struct sockaddr_l2cap {
	uint8		l2cap_len;
	uint8		l2cap_family;
	uint16		l2cap_psm;
	bdaddr_t	l2cap_bdaddr;
};

struct sockaddr_dl {
	uint8		sdl_len;
	uint8		sdl_family;
	uint32		sdl_index;
};

struct net_interface_address {
	sockaddr*	local;
};

struct net_buffer {
	sockaddr*				source;
	net_interface_address*	interface_address;
	uint32					size;

	// the test's
	std::vector<uint8>		data;
	sockaddr				sourceStorage;
};

struct l2cap_basic_header {
	uint16 length;
	uint16 dcid;
} _PACKED;

struct l2cap_connectionless_header {
	uint16 psm;
} _PACKED;

struct HciConnection {
	bool		isLE;
	uint16		handle;
	bdaddr_t	destination;
	hci_id		Hid;
};


static bigtime_t sTime = 10000000;

static bigtime_t
system_time()
{
	return sTime += 1000;
}


static bool sPanicked;

static void
panic(const char*, ...)
{
	sPanicked = true;
}


// the buffers

static int sLiveBuffers;
static bool sFailNextHeaderRead;

struct BufferModule {
	void free(net_buffer* buffer)
	{
		sLiveBuffers--;
		delete buffer;
	}

	status_t read(net_buffer* buffer, size_t offset, void* data, size_t size)
	{
		if (offset + size > buffer->data.size())
			return B_BAD_VALUE;
		memcpy(data, buffer->data.data() + offset, size);
		return B_OK;
	}
};

static BufferModule sBufferModule;
static BufferModule* gBufferModule = &sBufferModule;


template<typename Type>
class NetBufferHeaderReader {
public:
	NetBufferHeaderReader(net_buffer* buffer)
		:
		fBuffer(buffer)
	{
		fStatus = gBufferModule->read(buffer, 0, &fData, sizeof(Type));
		if (sFailNextHeaderRead) {
			sFailNextHeaderRead = false;
			fStatus = ENOBUFS;
		}
	}

	status_t Status() const { return fStatus; }
	Type* operator->() { return &fData; }

	void Remove()
	{
		fBuffer->data.erase(fBuffer->data.begin(),
			fBuffer->data.begin() + sizeof(Type));
		fBuffer->size -= sizeof(Type);
	}

private:
	net_buffer*	fBuffer;
	status_t	fStatus;
	Type		fData;
};


// the connections

static HciConnection sKeyboardLink = { false, 0x0b, { { 1, 1, 1, 1, 1, 1 } },
	0 };
static HciConnection sOtherLink = { false, 0x0c, { { 2, 2, 2, 2, 2, 2 } },
	0 };
static HciConnection* const kLinks[] = { &sKeyboardLink, &sOtherLink };

struct CoreData {
	HciConnection* ConnectionByDestination(const bdaddr_t& destination,
		hci_id hid)
	{
		for (HciConnection* link : kLinks) {
			if (link->Hid == hid && memcmp(&link->destination, &destination,
					sizeof(bdaddr_t)) == 0) {
				return link;
			}
		}
		return NULL;
	}
};

static CoreData sCoreData;
static CoreData* btCoreData = &sCoreData;


// the endpoints

struct net_socket {
	int references;
};

struct SocketModule {
	void release_socket(net_socket* socket) { socket->references--; }
};

static SocketModule sSocketModule;
static SocketModule* gSocketModule = &sSocketModule;

struct L2capEndpoint {
	net_socket*		socket;
	HciConnection*	connection;
	bool			queueFull;
	int				received;

	bool IsOnConnection(const HciConnection* link) const
	{
		return connection == link;
	}

	status_t ReceiveData(net_buffer* buffer)
	{
		// as fifo_enqueue_buffer(): a buffer that does not fit stays the
		// caller's
		if (queueFull)
			return ENOBUFS;
		received++;
		gBufferModule->free(buffer);
		return B_OK;
	}

	uint16 ATTReceiveMTU() const { return 0; }
	void ATTMTUChanged(uint16, bool) {}
};

static const uint16 kKeyboardChannel = 0x0040;
static const uint16 kListenedPSM = 0x0011;

static net_socket sKeyboardSocket;
static L2capEndpoint sKeyboardEndpoint = { &sKeyboardSocket, &sKeyboardLink };
static net_socket sConnectionlessSocket;
static L2capEndpoint sConnectionlessEndpoint = { &sConnectionlessSocket, NULL };

struct EndpointManager {
	L2capEndpoint* GetForChannel(uint16 cid)
	{
		if (cid != kKeyboardChannel)
			return NULL;
		sKeyboardSocket.references++;
		return &sKeyboardEndpoint;
	}

	L2capEndpoint* GetForPSM(uint16 psm)
	{
		if (psm != kListenedPSM)
			return NULL;
		sConnectionlessSocket.references++;
		return &sConnectionlessEndpoint;
	}

	L2capEndpoint* GetForFixedChannel(HciConnection*, uint16)
	{
		return NULL;
	}
};

static EndpointManager gL2capEndpointManager;


// signaling and LE: not this test's subject; they take the buffer

static int sSignalingCommands;

static status_t
l2cap_handle_signaling_command(HciConnection*, net_buffer* buffer)
{
	sSignalingCommands++;
	gBufferModule->free(buffer);
	return B_OK;
}

static status_t
l2cap_le_handle_signaling(HciConnection*, net_buffer* buffer)
{
	gBufferModule->free(buffer);
	return B_OK;
}

struct l2cap_le_att_update {
	uint16	mtu;
	bool	exchangeDone;
};

static void l2cap_le_dump(const char*, HciConnection*, uint16, net_buffer*) {}

static bool
l2cap_le_answer_att(HciConnection*, net_buffer*, uint16,
	l2cap_le_att_update& update)
{
	update.mtu = 0;
	return false;
}


static HciConnection* connection_for(net_buffer* buffer);

#include RECEIVE_DATA_SOURCE


// #pragma mark - the test


static int sFailures;

static void
check(bool ok, const char* what)
{
	if (!ok)
		sFailures++;
	printf("%s: %s\n", ok ? "ok" : "FAILED", what);
}


/*!	A frame as the ACL layer hands it up: from \a link, for channel \a cid,
	saying it has \a claimedLength bytes of payload and having \a length.
*/
static net_buffer*
make_frame(HciConnection* link, uint16 cid, size_t length,
	size_t claimedLength)
{
	static sockaddr_dl sLocal = {};
	static net_interface_address sInterfaceAddress
		= { (sockaddr*)&sLocal };

	net_buffer* buffer = new net_buffer;
	sLiveBuffers++;

	l2cap_basic_header header;
	header.length = htole16(claimedLength);
	header.dcid = htole16(cid);
	buffer->data.resize(sizeof(header) + length, 0xa1);
	memcpy(buffer->data.data(), &header, sizeof(header));
	buffer->size = buffer->data.size();

	sockaddr_l2cap* source = (sockaddr_l2cap*)&buffer->sourceStorage;
	memset(source, 0, sizeof(buffer->sourceStorage));
	source->l2cap_bdaddr = link->destination;
	buffer->source = &buffer->sourceStorage;
	buffer->interface_address = &sInterfaceAddress;
	return buffer;
}


static void
reset()
{
	sKeyboardEndpoint.queueFull = false;
	sKeyboardEndpoint.received = 0;
	sConnectionlessEndpoint.queueFull = false;
	sConnectionlessEndpoint.received = 0;
	sKeyboardSocket.references = 0;
	sConnectionlessSocket.references = 0;
	sPanicked = false;
	sSignalingCommands = 0;
}


static bool
nothing_lost()
{
	return sLiveBuffers == 0 && sKeyboardSocket.references == 0
		&& sConnectionlessSocket.references == 0 && !sPanicked;
}


int
main()
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	char text[160];

	// the keyboard's own report
	reset();
	status_t status = l2cap_receive_data(make_frame(&sKeyboardLink,
		kKeyboardChannel, 9, 9));
	check(status == B_OK && sKeyboardEndpoint.received == 1 && nothing_lost(),
		"data for a channel from its own link is delivered");

	// the same channel number, from another device
	reset();
	status = l2cap_receive_data(make_frame(&sOtherLink, kKeyboardChannel, 9,
		9));
	snprintf(text, sizeof(text), "data for the channel from another link is "
		"%s", sKeyboardEndpoint.received == 0 ? "dropped"
			: "DELIVERED to the channel");
	check(status != B_OK && sKeyboardEndpoint.received == 0 && nothing_lost(),
		text);

	// and from a link that is gone
	reset();
	HciConnection gone = { false, 0x0d, { { 3, 3, 3, 3, 3, 3 } }, 0 };
	status = l2cap_receive_data(make_frame(&gone, kKeyboardChannel, 9, 9));
	check(status != B_OK && sKeyboardEndpoint.received == 0 && nothing_lost(),
		"data for the channel from no known link is dropped");

	// the paths that end without a taker
	struct {
		const char*	what;
		uint16		cid;
		size_t		length;
		size_t		claimed;
		bool		queueFull;
		bool		headerFails;
	} kCases[] = {
		{ "a frame for a channel nobody has", 0x0077, 9, 9, false, false },
		{ "a frame whose length field is wrong", kKeyboardChannel, 9, 200,
			false, false },
		{ "a frame for a full receive queue", kKeyboardChannel, 9, 9, true,
			false },
		{ "a frame whose header cannot be read", kKeyboardChannel, 9, 9,
			false, true },
		{ "a connectionless frame for a PSM nobody listens on",
			L2CAP_CONNECTIONLESS_CID, 9, 9, false, false },
		{ "a connectionless frame too short for its PSM",
			L2CAP_CONNECTIONLESS_CID, 1, 1, false, false },
	};
	for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); i++) {
		reset();
		sLiveBuffers = 0;
		sKeyboardEndpoint.queueFull = kCases[i].queueFull;
		sFailNextHeaderRead = kCases[i].headerFails;
		status = l2cap_receive_data(make_frame(&sKeyboardLink, kCases[i].cid,
			kCases[i].length, kCases[i].claimed));
		snprintf(text, sizeof(text), "%s: an error, and the buffer is %s",
			kCases[i].what, sLiveBuffers == 0 ? "freed" : "LEAKED");
		check(status != B_OK && nothing_lost(), text);
	}
	sLiveBuffers = 0;

	// a connectionless frame somebody listens for
	reset();
	net_buffer* frame = make_frame(&sKeyboardLink, L2CAP_CONNECTIONLESS_CID,
		9, 9);
	uint16 psm = htole16(kListenedPSM);
	memcpy(frame->data.data() + sizeof(l2cap_basic_header), &psm, 2);
	status = l2cap_receive_data(frame);
	check(status == B_OK && sConnectionlessEndpoint.received == 1
			&& nothing_lost(),
		"a connectionless frame for a listener is delivered");

	// signaling
	reset();
	status = l2cap_receive_data(make_frame(&sKeyboardLink,
		L2CAP_SIGNALING_CID, 8, 8));
	check(status == B_OK && sSignalingCommands == 1 && nothing_lost(),
		"a signaling frame goes to the signaling code");

	reset();
	status = l2cap_receive_data(make_frame(&gone, L2CAP_SIGNALING_CID, 8, 8));
	snprintf(text, sizeof(text), "a signaling frame from a link that is "
		"gone: %s", sPanicked ? "PANIC" : sLiveBuffers == 0 ? "an error, "
			"freed" : "LEAKED");
	check(status != B_OK && sSignalingCommands == 0 && nothing_lost(), text);

	printf("%d failed\n", sFailures);
	return sFailures == 0 ? 0 : 1;
}
