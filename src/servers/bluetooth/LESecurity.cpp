/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 *
 * Pairing for applications (BT_MSG_LE_PAIR): an application holding an LE
 * link asks the server to secure it. The server encrypts with the stored
 * bond, or pairs (LE Secure Connections when the device supports it) after
 * the user agrees in a system prompt, and stores the new bond.
 */

#include "LocalDeviceImpl.h"

#include "CommandManager.h"

#include <Alert.h>
#include <Application.h>
#include <Autolock.h>
#include <Entry.h>
#include <Invoker.h>
#include <Looper.h>
#include <Roster.h>
#include <String.h>

#include <bluetooth/HCI/btHCI_command.h>
#include <bluetooth/HCI/btHCI_event.h>

#include <LEBondStore.h>
#include <LELegacyPairingClient.h>
#include <LELog.h>
#include <LESMPInitiator.h>

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>


using namespace Bluetooth;

#define LOG(level, format, args...) LELog(level, "pair-app", format, ##args)

static const bigtime_t kEncryptTimeout = 10000000;
static const uint32 kPromptAnswer = 'lePa';
static const uint32 kPasskeyClosed = 'lePc';


struct LocalDeviceImpl::LESecurityJob {
	LocalDeviceImpl* device;
	BMessage*	request;
	uint32		serial;
	uint8		address[6];
	uint8		addressType;
	BString		applicationName;
	BString		deviceName;
	bool		fresh;
	bool		bondedOnly;
};


namespace {

struct Outcome {
	status_t	status;
	uint8		smpReason;
	bool		smpReasonRemote;
	uint8		hciStatus;
	bool		encrypted;
	bool		bonded;
	bool		reusedBond;
	bool		secureConnections;
	bool		authenticated;
	int32		method;
	uint8		keySize;
	char		detail[256];
};


void
ClearSecret(void* data, size_t length)
{
	volatile uint8* bytes = (volatile uint8*)data;
	while (length-- > 0)
		*bytes++ = 0;
}


struct ScopedBond : public LELegacyBondKey {
	ScopedBond() { memset(this, 0, sizeof(*this)); }
	~ScopedBond() { ClearSecret(this, sizeof(*this)); }
};


//! Printable, at most 48 characters: it goes into a system prompt.
BString
SanitizedName(const char* name)
{
	BString result;
	if (name == NULL)
		return result;
	for (const char* c = name; *c != '\0' && result.Length() < 256; c++) {
		unsigned char byte = (unsigned char)*c;
		if (byte < 0x20 || byte == 0x7f)
			continue;
		result.Append(*c, 1);
	}
	result.TruncateChars(48);
	result.Trim();
	return result;
}


BString
ApplicationName(const BMessage* request)
{
	team_id team = request->ReturnAddress().Team();
	app_info info;
	if (team >= 0 && be_roster->GetRunningAppInfo(team, &info) == B_OK)
		return SanitizedName(info.ref.name);
	return BString();
}


status_t
ReadLocalAddress(int32 hciID, uint8 address[6])
{
	size_t size;
	void* command = buildReadBdAddr(&size);
	if (command == NULL)
		return B_NO_MEMORY;
	BMessage request(BT_MSG_HANDLE_SIMPLE_REQUEST);
	request.AddInt32("hci_id", hciID);
	request.AddData("raw command", B_ANY_TYPE, command, size);
	request.AddInt16("eventExpected", HCI_EVENT_CMD_COMPLETE);
	request.AddInt16("opcodeExpected",
		PACK_OPCODE(OGF_INFORMATIONAL_PARAM, OCF_READ_BD_ADDR));
	free(command);
	BMessage reply;
	status_t status = be_app_messenger.SendMessage(&request, &reply, 2000000,
		2000000);
	if (status != B_OK)
		return status;
	const void* data;
	ssize_t length;
	if (reply.FindData("bdaddr", B_ANY_TYPE, &data, &length) != B_OK
		|| length != 6)
		return B_ERROR;
	memcpy(address, data, 6);
	return B_OK;
}


status_t
OpenSMPChannel(const uint8 peerAddress[6], int& smpSocket)
{
	smpSocket = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BLUETOOTH_PROTO_L2CAP);
	if (smpSocket < 0)
		return errno;
	timeval sendTimeout = { 10, 0 };
	if (setsockopt(smpSocket, SOL_SOCKET, SO_SNDTIMEO, &sendTimeout,
			sizeof(sendTimeout)) != 0)
		return errno;
	sockaddr_l2cap peer = {};
	peer.l2cap_len = sizeof(peer);
	peer.l2cap_family = AF_BLUETOOTH;
	peer.l2cap_psm = B_L2CAP_LE_SMP_CID;
	memcpy(peer.l2cap_bdaddr.b, peerAddress, 6);
	if (connect(smpSocket, (const sockaddr*)&peer, sizeof(peer)) == 0)
		return B_OK;
	return errno;
}


// #pragma mark - the system prompt


class PromptLooper : public BLooper {
public:
	PromptLooper()
		:
		BLooper("LE pairing prompt"),
		fAnswers(create_sem(0, "LE pairing answer")),
		fPrompt(0),
		fChoice(-1),
		fPasskeyClosed(false)
	{
	}

	~PromptLooper() override
	{
		delete_sem(fAnswers);
	}

	void MessageReceived(BMessage* message) override
	{
		switch (message->what) {
			case kPromptAnswer:
			{
				// Only the prompt asked last counts.
				if (message->GetInt32("prompt", -1) == atomic_get(&fPrompt)) {
					atomic_set(&fChoice, message->GetInt32("which", -1));
					release_sem(fAnswers);
				}
				break;
			}
			case kPasskeyClosed:
				fPasskeyClosed = true;
				break;
			default:
				BLooper::MessageReceived(message);
		}
	}

	int32 NewPrompt()
	{
		atomic_set(&fChoice, -1);
		while (acquire_sem_etc(fAnswers, 1, B_RELATIVE_TIMEOUT, 0) == B_OK)
			;
		return atomic_add(&fPrompt, 1) + 1;
	}

	sem_id Answers() const { return fAnswers; }
	int32 Choice() { return atomic_get(&fChoice); }
	bool PasskeyClosed() const { return fPasskeyClosed; }

private:
	sem_id fAnswers;
	int32 fPrompt;
	int32 fChoice;
	volatile bool fPasskeyClosed;
};


class SystemPrompt : public LESMPUserAgent {
public:
	SystemPrompt(const BString& application, const BString& device,
		const char* address, volatile bool& goingAway)
		:
		fApplication(application),
		fDevice(device),
		fGoingAway(goingAway),
		fLooper(new PromptLooper())
	{
		fDeviceText = fDevice.IsEmpty() ? BString(address)
			: BString().SetToFormat("“%s” (%s)", fDevice.String(), address);
		fApplicationText = fApplication.IsEmpty()
			? BString("An application")
			: BString().SetToFormat("“%s”", fApplication.String());
		fLooper->Run();
	}

	~SystemPrompt() override
	{
		_Close(fQuestion);
		_Close(fPasskey);
		if (fLooper->Lock())
			fLooper->Quit();
	}

	bool Confirm(LESMPMethod method, uint32 value, bigtime_t deadline) override
	{
		BString text;
		const char* accept = "Pair";
		if (method == LE_SMP_NUMERIC_COMPARISON) {
			text.SetToFormat("%s wants to pair with the Bluetooth device "
				"%s.\n\nPair only if the device shows this number:\n\n"
				"%03" B_PRIu32 " %03" B_PRIu32, fApplicationText.String(),
				fDeviceText.String(), value / 1000, value % 1000);
			accept = "Numbers match";
		} else {
			text.SetToFormat("%s wants to pair with the Bluetooth device "
				"%s.\n\nThe device and this computer will encrypt their "
				"connection and remember each other. The device cannot show "
				"a code to check, so pair only if you asked for this.",
				fApplicationText.String(), fDeviceText.String());
		}
		int32 prompt = fLooper->NewPrompt();
		BMessage* answer = new BMessage(kPromptAnswer);
		answer->AddInt32("prompt", prompt);
		BAlert* alert = new BAlert("Bluetooth pairing", text.String(),
			"Don't pair", accept, NULL, B_WIDTH_AS_USUAL, B_OFFSET_SPACING,
			B_IDEA_ALERT);
		alert->SetShortcut(0, B_ESCAPE);
		fQuestion = BMessenger(alert);
		alert->Go(new BInvoker(answer, BMessenger(fLooper)));

		LOG(LE_LOG_INFO, "asking the user (%s) for %s",
			LESMPMethodName(method), fApplicationText.String());
		bool answered = false;
		while (!fGoingAway && system_time() < deadline) {
			if (acquire_sem_etc(fLooper->Answers(), 1, B_RELATIVE_TIMEOUT,
					100000) == B_OK) {
				answered = true;
				break;
			}
		}
		_Close(fQuestion);
		bool accepted = answered && fLooper->Choice() == 1;
		LOG(LE_LOG_INFO, "the user %s", !answered ? "did not answer in time"
			: accepted ? "accepted" : "declined");
		return accepted;
	}

	void DisplayPasskey(uint32 passkey) override
	{
		BString text;
		text.SetToFormat("%s wants to pair with the Bluetooth device %s.\n\n"
			"To pair, type this number on the device and press Enter:\n\n"
			"%06" B_PRIu32, fApplicationText.String(), fDeviceText.String(),
			passkey);
		BAlert* alert = new BAlert("Bluetooth pairing", text.String(),
			"Cancel", NULL, NULL, B_WIDTH_AS_USUAL, B_INFO_ALERT);
		alert->SetShortcut(0, B_ESCAPE);
		fPasskey = BMessenger(alert);
		alert->Go(new BInvoker(new BMessage(kPasskeyClosed),
			BMessenger(fLooper)));
	}

	bool Cancelled() override
	{
		return fGoingAway || fLooper->PasskeyClosed();
	}

	void Finished(status_t status) override
	{
		_Close(fPasskey);
	}

private:
	void _Close(BMessenger& window)
	{
		if (window.IsValid())
			window.SendMessage(B_QUIT_REQUESTED);
		window = BMessenger();
	}

	BString fApplication;
	BString fDevice;
	BString fDeviceText;
	BString fApplicationText;
	volatile bool& fGoingAway;
	PromptLooper* fLooper;
	BMessenger fQuestion;
	BMessenger fPasskey;
};


const char*
HCIStatusText(uint8 status)
{
	switch (status) {
		case 0x05: return "authentication failure";
		case 0x06: return "PIN or key missing";
		case 0x08: return "connection timeout";
		case 0x13: return "the device ended the connection";
		case 0x3d: return "MIC failure";
		default: return "HCI error";
	}
}

} // unnamed namespace


// #pragma mark - LocalDeviceImpl


status_t
LocalDeviceImpl::PairLELink(BMessage* request)
{
	BMessenger listener;
	if (request == NULL
		|| request->FindMessenger("listener", &listener) != B_OK
		|| !listener.IsValid())
		return B_BAD_VALUE;
	const void* address = NULL;
	ssize_t addressSize = 0;
	if (request->FindData("address", B_RAW_TYPE, &address, &addressSize)
			!= B_OK || addressSize != 6)
		address = NULL;

	LESecurityJob* job = new(std::nothrow) LESecurityJob;
	if (job == NULL)
		return B_NO_MEMORY;
	{
		BAutolock lock(fLELock);
		LELink* found = NULL;
		bool othersLink = false;
		for (size_t i = 0; i < fLELinks.size(); i++) {
			LELink* link = fLELinks[i];
			if (link->state != LE_CONN_CONNECTED
				|| (address != NULL && memcmp(link->address, address, 6) != 0))
				continue;
			if (!(link->listener == listener)) {
				othersLink = true;
				continue;
			}
			if (found != NULL) {
				delete job;
				return B_BAD_VALUE;
					// several links: the request must name the peer
			}
			found = link;
		}
		if (found == NULL) {
			delete job;
			return othersLink ? B_NOT_ALLOWED : B_NO_INIT;
		}
		if (found->securing) {
			delete job;
			return B_BUSY;
		}
		found->securing = true;
		job->serial = found->serial;
		memcpy(job->address, found->address, 6);
		job->addressType = found->addressType;
	}
	job->device = this;
	job->request = request;
	job->applicationName = ApplicationName(request);
	job->deviceName = SanitizedName(request->GetString("device_name", NULL));
	job->fresh = request->GetBool("fresh", false);
	job->bondedOnly = request->GetBool("bonded_only", false);

	atomic_add(&fLESecurityJobs, 1);
	thread_id thread = spawn_thread(&_LESecurityThread, "LE pairing",
		B_NORMAL_PRIORITY, job);
	if (thread < 0 || resume_thread(thread) != B_OK) {
		atomic_add(&fLESecurityJobs, -1);
		_EndLESecurity(job->serial);
		job->request = NULL;
		delete job;
		return thread < 0 ? thread : B_ERROR;
	}
	return B_OK;
}


/*static*/ status_t
LocalDeviceImpl::_LESecurityThread(void* data)
{
	LESecurityJob* job = (LESecurityJob*)data;
	// The device's destructor waits for its jobs to end.
	job->device->_RunLESecurity(job);
	return B_OK;
}


status_t
LocalDeviceImpl::_LESecurityLink(uint32 serial, uint16* handle,
	bool* encrypted, uint32* encryptionChanges)
{
	BAutolock lock(fLELock);
	for (size_t i = 0; i < fLELinks.size(); i++) {
		LELink* link = fLELinks[i];
		if (link->serial != serial)
			continue;
		if (link->state != LE_CONN_CONNECTED)
			return B_DEV_NOT_READY;
		if (handle != NULL)
			*handle = link->handle;
		if (encrypted != NULL)
			*encrypted = link->encrypted;
		if (encryptionChanges != NULL)
			*encryptionChanges = link->encryptionChanges;
		return B_OK;
	}
	return B_DEV_NOT_READY;
}


status_t
LocalDeviceImpl::_EncryptLESecurityLink(uint32 serial, const uint8* key,
	const uint8* randomNumber, uint16 encryptedDiversifier,
	uint32* encryptionChanges)
{
	BAutolock lock(fLELock);
	uint16 handle;
	status_t status = _LESecurityLink(serial, &handle, NULL,
		encryptionChanges);
	if (status != B_OK)
		return status;
	BMessage request;
	request.AddUInt16("handle", handle);
	return StartLEEncryption(&request, key, randomNumber,
		encryptedDiversifier);
}


status_t
LocalDeviceImpl::_WaitLESecurityEncryption(uint32 serial,
	uint32 changesBefore, bigtime_t timeout, uint8* hciStatus)
{
	*hciStatus = 0;
	bigtime_t deadline = system_time() + timeout;
	while (!fLEGoingAway && system_time() < deadline) {
		{
			BAutolock lock(fLELock);
			LELink* found = NULL;
			for (size_t i = 0; i < fLELinks.size(); i++) {
				if (fLELinks[i]->serial == serial)
					found = fLELinks[i];
			}
			if (found == NULL || found->state != LE_CONN_CONNECTED)
				return B_DEV_NOT_READY;
			if (found->encryptionChanges != changesBefore) {
				if (found->encrypted)
					return B_OK;
				*hciStatus = found->encryptionStatus;
				return B_ERROR;
			}
		}
		snooze(20000);
	}
	return fLEGoingAway ? B_CANCELED : B_TIMED_OUT;
}


void
LocalDeviceImpl::_EndLESecurity(uint32 serial)
{
	BAutolock lock(fLELock);
	for (size_t i = 0; i < fLELinks.size(); i++) {
		if (fLELinks[i]->serial == serial)
			fLELinks[i]->securing = false;
	}
}


void
LocalDeviceImpl::_RunLESecurity(LESecurityJob* job)
{
	Outcome out;
	memset(&out, 0, sizeof(out));
	out.status = B_ERROR;
	out.method = -1;
	char addressText[18];
	LEAddressString(job->address, addressText);
	LOG(LE_LOG_INFO, "=== securing the link to %s (type %u) for %s%s ===",
		addressText, job->addressType, job->applicationName.IsEmpty()
			? "an application" : job->applicationName.String(),
		job->fresh ? ", pairing again" : job->bondedOnly ? ", bond only" : "");
	bigtime_t started = system_time();

	int smpSocket = -1;
	uint8 local[6];
	char directory[PATH_MAX];
	ScopedBond bond;
	uint8 storedAddress[6] = {};
	uint8 storedType = 0;
	bool hadBond = false;
	uint16 handle = 0;
	bool encrypted = false;
	uint32 changes = 0;
	LESMPOptions options;
	LESMPResult smp;
	memset(&smp, 0, sizeof(smp));

	status_t status = _LESecurityLink(job->serial, &handle, &encrypted,
		&changes);
	if (status != B_OK) {
		out.status = status;
		snprintf(out.detail, sizeof(out.detail), "The link to the device is "
			"gone.");
		goto reply;
	}
	if (encrypted) {
		// Pairing again on an encrypted link would need the link's key
		// refreshed under the client's feet; a new connection is simpler.
		out.status = job->fresh ? B_BUSY : B_OK;
		out.encrypted = true;
		snprintf(out.detail, sizeof(out.detail), job->fresh
			? "The link is already encrypted. Connect again to pair anew."
			: "The link is already encrypted.");
		goto reply;
	}
	status = ReadLocalAddress(GetID(), local);
	if (status == B_OK)
		status = DefaultLEBondDirectory(directory, sizeof(directory));
	if (status != B_OK) {
		out.status = status;
		snprintf(out.detail, sizeof(out.detail), "The adapter's address or "
			"the bond directory is not available (%s).", strerror(status));
		goto reply;
	}

	if (!job->fresh && FindLEBond(directory, local, job->address,
			job->addressType, bond, storedAddress, &storedType) == B_OK) {
		hadBond = true;
		LOG(LE_LOG_INFO, "encrypting with the stored bond (%s, %u-byte key)",
			bond.secureConnections ? "Secure Connections" : "legacy",
			bond.keySize);
		uint8 hci = 0;
		status = _EncryptLESecurityLink(job->serial, bond.longTermKey,
			bond.randomNumber, bond.encryptedDiversifier, &changes);
		if (status == B_OK) {
			status = _WaitLESecurityEncryption(job->serial, changes,
				kEncryptTimeout, &hci);
		}
		if (status == B_OK) {
			out.status = B_OK;
			out.encrypted = true;
			out.bonded = true;
			out.reusedBond = true;
			out.secureConnections = bond.secureConnections;
			out.authenticated = bond.authenticated;
			out.keySize = bond.keySize;
			goto reply;
		}
		out.hciStatus = hci;
		bool linkUp = _LESecurityLink(job->serial, NULL, NULL, &changes)
			== B_OK;
		if (hci == 0x06 && linkUp && !job->bondedOnly) {
			LOG(LE_LOG_INFO, "the device no longer has the bond; pairing "
				"again");
		} else {
			out.status = status;
			if (hci != 0) {
				snprintf(out.detail, sizeof(out.detail), "Encrypting with "
					"the stored key failed: %s (HCI %#x).%s",
					HCIStatusText(hci), hci, hci == 0x06 ? " The device has "
						"forgotten this computer and must be paired again."
						: "");
			} else {
				snprintf(out.detail, sizeof(out.detail), "Encrypting with "
					"the stored key failed: %s.", strerror(status));
			}
			goto reply;
		}
	} else if (job->bondedOnly) {
		out.status = B_ENTRY_NOT_FOUND;
		snprintf(out.detail, sizeof(out.detail), "No bond is stored for the "
			"device.");
		goto reply;
	}

	{
		status = OpenSMPChannel(job->address, smpSocket);
		if (status != B_OK) {
			out.status = status;
			snprintf(out.detail, sizeof(out.detail), "Could not open the "
				"security channel: %s.", strerror(status));
			goto reply;
		}
		SystemPrompt prompt(job->applicationName, job->deviceName,
			addressText, fLEGoingAway);
		// This side can show a number and take a yes or no.
		options.ioCapability = LE_SMP_IO_DISPLAY_YES_NO;
		options.mitm = true;
		options.secureConnections = true;
		status = LESMPPairAsInitiator(smpSocket, local, 0, job->address,
			job->addressType, options, &prompt, smp, NULL, out.detail,
			sizeof(out.detail));
	}
	out.smpReason = smp.reason;
	out.smpReasonRemote = smp.remoteReason;
	if (status != B_OK) {
		out.status = status;
		goto reply;
	}
	out.secureConnections = smp.secureConnections;
	out.authenticated = smp.authenticated;
	out.method = smp.method;
	out.keySize = smp.keySize;

	{
		uint8 hci = 0;
		status = _EncryptLESecurityLink(job->serial, smp.key, NULL, 0,
			&changes);
		if (status == B_OK) {
			status = _WaitLESecurityEncryption(job->serial, changes,
				kEncryptTimeout, &hci);
		}
		if (status != B_OK) {
			out.status = status;
			out.hciStatus = hci;
			snprintf(out.detail, sizeof(out.detail), "Paired, but encrypting "
				"the link failed: %s.", hci != 0 ? HCIStatusText(hci)
					: strerror(status));
			goto reply;
		}
	}
	out.encrypted = true;

	status = LELegacyReceiveResponderKeys(smpSocket, smp.responderKeys,
		smp.keySize, bond, out.detail, sizeof(out.detail));
	if (status != B_OK) {
		out.status = status;
		goto reply;
	}
	if (smp.secureConnections && smp.bonding) {
		memcpy(bond.longTermKey, smp.key, 16);
		memset(bond.randomNumber, 0, sizeof(bond.randomNumber));
		bond.encryptedDiversifier = 0;
		bond.hasLongTermKey = true;
	}
	bond.secureConnections = smp.secureConnections;
	bond.authenticated = smp.authenticated;
	if (bond.hasLongTermKey) {
		status = SaveLEBond(directory, local, 0, job->address,
			job->addressType, bond);
		if (status != B_OK) {
			LOG(LE_LOG_ERROR, "could not save the bond: %s",
				strerror(status));
		} else {
			out.bonded = true;
			// A bond the device has forgotten, kept under an older address.
			if (hadBond && (storedType != job->addressType
					|| memcmp(storedAddress, job->address, 6) != 0))
				RemoveLEBond(directory, local, 0, storedAddress, storedType);
		}
	} else {
		LOG(LE_LOG_INFO, "the device did not bond; the keys are for this "
			"connection only");
	}
	out.status = B_OK;
	out.detail[0] = '\0';

reply:
	if (smpSocket >= 0)
		close(smpSocket);
	ClearSecret(&smp, sizeof(smp));
	ClearSecret(local, sizeof(local));

	LOG(out.status == B_OK ? LE_LOG_INFO : LE_LOG_ERROR,
		"=== securing %s %s after %.1f s: %s%s%s%s ===", addressText,
		out.status == B_OK ? "succeeded" : "failed",
		(system_time() - started) / 1000000.0,
		out.reusedBond ? "stored bond" : out.method >= 0
			? LESMPMethodName((LESMPMethod)out.method) : "no pairing",
		out.secureConnections ? ", Secure Connections" : "",
		out.detail[0] != '\0' ? "; " : "", out.detail);

	BMessage reply(BT_MSG_LE_PAIR_RESULT);
	reply.AddInt32("status", out.status);
	reply.AddUInt8("smp_reason", out.smpReason);
	reply.AddBool("smp_reason_remote", out.smpReasonRemote);
	reply.AddUInt8("hci_status", out.hciStatus);
	reply.AddBool("encrypted", out.encrypted);
	reply.AddBool("bonded", out.bonded);
	reply.AddBool("reused_bond", out.reusedBond);
	reply.AddBool("secure_connections", out.secureConnections);
	reply.AddBool("authenticated", out.authenticated);
	reply.AddInt32("method", out.method);
	reply.AddUInt8("key_size", out.keySize);
	reply.AddString("detail", out.detail);
	job->request->SendReply(&reply);

	_EndLESecurity(job->serial);
	delete job->request;
	delete job;
	atomic_add(&fLESecurityJobs, -1);
}
