/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */


#include "NearbyScanner.h"

#include <Catalog.h>
#include <InterfaceDefs.h>
#include <Looper.h>
#include <MessageRunner.h>
#include <Messenger.h>

#include <bluetooth/DiscoveryAgent.h>
#include <bluetooth/DiscoveryListener.h>
#include <bluetooth/LocalDevice.h>
#include <bluetooth/RemoteDevice.h>

#include <LELog.h>
#include <bluetoothserver_p.h>

#include "BluetoothWorkers.h"

#include <string.h>

#include <algorithm>


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "Nearby scanner"


#define TRACE(format...) \
	Bluetooth::LELog(Bluetooth::LE_LOG_DEBUG, "prefs", format)


static const uint32 kMsgScanStartReply = 'scRp';
static const uint32 kMsgScanStartTimeout = 'scTo';
static const uint32 kMsgScanRetry = 'scRt';
static const uint32 kMsgScanStopTimeout = 'scSt';
static const uint32 kMsgInquiryDue = 'iqDu';
static const uint32 kMsgInquiryStarted = 'iqSt';
static const uint32 kMsgInquiryDevice = 'iqDv';
static const uint32 kMsgInquiryDone = 'iqDn';
static const uint32 kMsgInquiryTimeout = 'iqTo';
static const uint32 kMsgPausedNotify = 'psNt';

static const bigtime_t kLEStartTimeout = 10000000;
static const bigtime_t kLEStopTimeout = 4000000;
static const bigtime_t kBusyRetryDelay = 3000000;
static const bigtime_t kErrorRetryDelay = 5000000;
static const bigtime_t kFirstInquiryDelay = 4000000;
static const bigtime_t kInquiryInterval = 12000000;
static const bigtime_t kInquiryTimeout = 25000000;
// A Classic inquiry takes most of the radio: on the Cubie A7S a Bluetooth
// mouse sent 30 reports a second instead of 89 during one (gaps over 30 ms
// went from 6% to 51%), and a keyboard being set up lost its SDP query to
// it. So inquiries are short (3.84 s finds a device in pairing mode, which
// listens every 1.28 s), start only once the mouse and keyboard have been
// left alone for a moment, and keep away from a Classic device that was
// just connected.
static const uint8 kInquiryLength = 3;
static const bigtime_t kInquiryIdleTime = 2000000;
static const bigtime_t kInquiryIdleCheck = 1000000;
	// in units of 1.28 seconds

enum {
	STATUS_OFF,
	STATUS_SEARCHING,
	STATUS_BUSY,
	STATUS_FAILING
};


class InquiryListener : public Bluetooth::DiscoveryListener {
public:
	InquiryListener(const BMessenger& target)
		:
		fTarget(target)
	{
	}

	virtual void DeviceDiscovered(Bluetooth::RemoteDevice* device,
		DeviceClass deviceClass)
	{
		BMessage message(kMsgInquiryDevice);
		message.AddPointer("listener", this);
		bdaddr_t address = device->GetBluetoothAddress();
		message.AddData("address", B_RAW_TYPE, address.b, 6);
		message.AddUInt32("class", deviceClass.Record());
		message.AddString("name", device->GetCachedFriendlyName());
		message.AddUInt8("page_repetition_mode",
			device->GetPageRepetitionMode());
		message.AddUInt16("clock_offset", device->GetClockOffset());
		_Send(message);
	}

	virtual void InquiryResponse(int type)
	{
		BMessage message(kMsgInquiryDone);
		message.AddPointer("listener", this);
		message.AddInt32("type", type);
		_Send(message);
	}

	virtual void InquiryStarted(status_t status)
	{
		BMessage message(kMsgInquiryStarted);
		message.AddPointer("listener", this);
		message.AddInt32("status", status);
		_Send(message);
	}

private:
	void _Send(BMessage& message)
	{
		fTarget.SendMessage(&message, (BHandler*)NULL, 1000000);
	}

	BMessenger	fTarget;
};


NearbyScanner::NearbyScanner(ScannerListener* listener)
	:
	BHandler("nearby scanner"),
	fListener(listener),
	fDevice(NULL),
	fAgent(NULL),
	fInquiryListener(NULL),
	fEnabled(false),
	fPaused(false),
	fPauseNotified(false),
	fLEState(LE_IDLE),
	fLEGeneration(0),
	fInquiryActive(false),
	fInquiryNotBefore(0),
	fInquiryGeneration(0),
	fFailures(0),
	fStatus(STATUS_OFF)
{
}


NearbyScanner::~NearbyScanner()
{
	Stop();
}


void
NearbyScanner::SetAdapter(Bluetooth::LocalDevice* device)
{
	if (device == fDevice)
		return;

	bool enabled = fEnabled;
	Stop();
	fDevice = device;
	// DiscoveryAgent objects cannot be deleted by applications.
	fAgent = device != NULL ? device->GetDiscoveryAgent() : NULL;
	if (enabled)
		Start();
	else if (fPaused)
		_NotifyPausedIfIdle();
}


void
NearbyScanner::Start()
{
	if (fEnabled)
		return;
	fEnabled = true;
	fFailures = 0;
	if (!fPaused)
		_Begin();
}


void
NearbyScanner::Stop()
{
	fEnabled = false;
	_Halt();
	_SetStatus(STATUS_OFF);
	if (fPaused)
		_NotifyPausedIfIdle();
}


void
NearbyScanner::Pause()
{
	if (fPaused)
		return;
	fPaused = true;
	fPauseNotified = false;
	fListener->ScannerStatusChanged();

	// A running inquiry is left to finish (about ten seconds) rather than
	// cancelled: an Inquiry Cancel racing the LE scan disable has left the
	// controller without command credits. ScannerPaused() follows once both
	// are done.
	if (fLEState == LE_STARTING || fLEState == LE_ACTIVE)
		_StopLE();
	// LE_REQUESTED and LE_STOPPING finish on their own; the handlers see
	// fPaused.
	_NotifyPausedIfIdle();
}


void
NearbyScanner::HoldInquiry(bigtime_t duration)
{
	fInquiryNotBefore = std::max(fInquiryNotBefore, system_time() + duration);
}


void
NearbyScanner::Resume()
{
	if (!fPaused)
		return;
	fPaused = false;
	fPauseNotified = false;
	fFailures = 0;
	fListener->ScannerStatusChanged();
	if (fEnabled)
		_Begin();
}


bool
NearbyScanner::IsSearching() const
{
	return fEnabled && !fPaused && fDevice != NULL;
}


BString
NearbyScanner::StatusText() const
{
	if (fDevice == NULL)
		return BString();
	if (fPaused)
		return B_TRANSLATE("Searching is paused while connecting");
	if (!fEnabled)
		return BString();
	switch (fStatus) {
		case STATUS_BUSY:
			return B_TRANSLATE("Waiting for the Bluetooth controller"
				B_UTF8_ELLIPSIS);
		case STATUS_FAILING:
			return B_TRANSLATE("Low Energy scan failed, retrying"
				B_UTF8_ELLIPSIS);
		default:
			return B_TRANSLATE("Searching" B_UTF8_ELLIPSIS);
	}
}


void
NearbyScanner::MessageReceived(BMessage* message)
{
	int32 generation = message->GetInt32("generation", -1);

	switch (message->what) {
		case kMsgScanStartReply:
		{
			status_t status = message->GetInt32("status", B_ERROR);
			if (generation != fLEGeneration || fLEState != LE_REQUESTED) {
				// Stop() or Pause() came first; stop what this started.
				if (status == B_OK)
					_SendLEStop();
				break;
			}
			if (status == B_OK) {
				if (!fEnabled || fPaused) {
					_StopLE();
					break;
				}
				fLEState = LE_STARTING;
				_Timer(kMsgScanStartTimeout, fLEGeneration, kLEStartTimeout);
				break;
			}

			TRACE("LE scan request failed: %s", strerror(status));
			fLEState = LE_IDLE;
			_LEFailed(status == B_BUSY ? STATUS_BUSY : STATUS_FAILING,
				status == B_BUSY ? kBusyRetryDelay : kErrorRetryDelay);
			break;
		}

		case BT_MSG_LE_SCAN_STARTED:
			// Also sent when we join a scan another client already runs.
			if (fLEState == LE_STARTING) {
				TRACE("LE scan started");
				fLEState = LE_ACTIVE;
				fLEGeneration++;
				fFailures = 0;
				_SetStatus(STATUS_SEARCHING);
			} else if (fLEState == LE_IDLE)
				_SendLEStop();
			break;

		case BT_MSG_LE_ADVERTISEMENT:
		{
			if (fLEState != LE_ACTIVE && fLEState != LE_STARTING)
				break;
			const uint8* address;
			const uint8* data;
			ssize_t addressSize, dataSize;
			if (message->FindData("address", B_RAW_TYPE,
					(const void**)&address, &addressSize) != B_OK
				|| addressSize != 6
				|| message->FindData("data", B_RAW_TYPE,
					(const void**)&data, &dataSize) != B_OK)
				break;
			uint8 addressType = 0;
			uint8 eventType = 0xff;
			int8 rssi = 0;
			message->FindUInt8("address_type", &addressType);
			message->FindUInt8("event_type", &eventType);
			bool hasRSSI = message->FindInt8("rssi", &rssi) == B_OK
				&& rssi != 127;
			LEAdvertisingData advertising = ParseLEAdvertisingData(data,
				(size_t)dataSize);
			fListener->LEAdvertisement(address, addressType, eventType,
				advertising, hasRSSI, rssi);
			break;
		}

		case BT_MSG_LE_SCAN_ERROR:
			TRACE("LE scan error %u", message->GetUInt8("status", 0));
			if (fLEState == LE_STOPPING)
				_LEStopped();
			else if (fLEState == LE_STARTING || fLEState == LE_ACTIVE) {
				fLEState = LE_IDLE;
				_LEFailed(STATUS_FAILING, kBusyRetryDelay);
			}
			break;

		case BT_MSG_LE_SCAN_STOPPED:
			TRACE("LE scan stopped");
			if (fLEState == LE_STOPPING)
				_LEStopped();
			else if (fLEState == LE_ACTIVE || fLEState == LE_STARTING) {
				// Stopped by someone else; start again.
				fLEState = LE_IDLE;
				_LEFailed(STATUS_SEARCHING, kBusyRetryDelay);
			}
			break;

		case kMsgScanStartTimeout:
			if (generation != fLEGeneration || fLEState != LE_STARTING)
				break;
			TRACE("LE scan did not start within 10 s");
			_SendLEStop();
			fLEState = LE_IDLE;
			_LEFailed(STATUS_FAILING, kBusyRetryDelay);
			break;

		case kMsgScanRetry:
			if (generation == fLEGeneration && fEnabled && !fPaused
				&& fLEState == LE_IDLE)
				_StartLE();
			break;

		case kMsgScanStopTimeout:
			if (generation == fLEGeneration && fLEState == LE_STOPPING)
				_LEStopped();
			break;

		case kMsgInquiryDue:
		{
			if (generation != fInquiryGeneration || !fEnabled || fPaused
				|| fInquiryActive)
				break;
			bigtime_t now = system_time();
			if (now < fInquiryNotBefore) {
				_ScheduleInquiry(fInquiryNotBefore - now);
				break;
			}
			// Someone is using the mouse or keyboard: wait until they stop.
			if (idle_time() < kInquiryIdleTime) {
				_ScheduleInquiry(kInquiryIdleCheck);
				break;
			}
			_StartInquiry();
			break;
		}

		case kMsgInquiryStarted:
		case kMsgInquiryDevice:
		case kMsgInquiryDone:
		{
			void* listener = NULL;
			if (message->FindPointer("listener", &listener) != B_OK
				|| listener != fInquiryListener || !fInquiryActive)
				break;

			if (message->what == kMsgInquiryStarted) {
				TRACE("inquiry started: status %" B_PRId32,
					message->GetInt32("status", B_ERROR));
				if (message->GetInt32("status", B_ERROR) != B_OK)
					_EndInquiry();
			} else if (message->what == kMsgInquiryDone) {
				TRACE("inquiry finished: type %" B_PRId32,
					message->GetInt32("type", 0));
				_EndInquiry();
			} else {
				const uint8* address;
				ssize_t size;
				if (message->FindData("address", B_RAW_TYPE,
						(const void**)&address, &size) != B_OK || size != 6)
					break;
				TRACE("inquiry result %02X:%02X:%02X:%02X:%02X:%02X class "
					"%06" B_PRIx32 " name \"%s\"", address[5], address[4],
					address[3], address[2], address[1], address[0],
					message->GetUInt32("class", 0),
					message->GetString("name", ""));
				fListener->ClassicDeviceFound(address,
					message->GetUInt32("class", 0),
					message->GetString("name", ""),
					message->GetUInt8("page_repetition_mode", 0),
					message->GetUInt16("clock_offset", 0));
			}
			break;
		}

		case kMsgInquiryTimeout:
			if (generation == fInquiryGeneration && fInquiryActive) {
				TRACE("inquiry timed out");
				_EndInquiry();
			}
			break;

		case kMsgPausedNotify:
			if (fPaused && fLEState == LE_IDLE && !fInquiryActive)
				fListener->ScannerPaused();
			else
				fPauseNotified = false;
			break;

		default:
			BHandler::MessageReceived(message);
			break;
	}
}


void
NearbyScanner::_Begin()
{
	// The LE scan runs for as long as the window is visible: every stop and
	// start is another controller state change, and the controller has been
	// seen to stop answering around those. A Classic inquiry runs alongside
	// it now and then.
	if (fDevice == NULL)
		return;
	if (fLEState == LE_IDLE)
		_StartLE();
	_ScheduleInquiry(kFirstInquiryDelay);
}


void
NearbyScanner::_Halt()
{
	// Invalidates pending timers; a late scan-start reply stops the scan it
	// started.
	fLEGeneration++;
	fInquiryGeneration++;

	if (fLEState == LE_STARTING || fLEState == LE_ACTIVE)
		_SendLEStop();
	fLEState = LE_IDLE;

	if (fInquiryActive)
		_EndInquiry();
}


void
NearbyScanner::_StartLE()
{
	if (fDevice == NULL)
		return;

	fLEGeneration++;
	fLEState = LE_REQUESTED;
	TRACE("requesting LE scan");

	BMessage request(BT_MSG_LE_SCAN_START);
	request.AddInt32("hci_id", _HCIID());
	request.AddMessenger("listener", BMessenger(this));
	BMessage result(kMsgScanStartReply);
	result.AddInt32("generation", fLEGeneration);
	if (SendServerRequestAsync(request, BMessenger(this), result) != B_OK) {
		fLEState = LE_IDLE;
		_LEFailed(STATUS_FAILING, kErrorRetryDelay);
	}
}


void
NearbyScanner::_StopLE()
{
	_SendLEStop();
	fLEState = LE_STOPPING;
	fLEGeneration++;
	_Timer(kMsgScanStopTimeout, fLEGeneration, kLEStopTimeout);
}


void
NearbyScanner::_SendLEStop()
{
	if (fDevice == NULL)
		return;
	// The server shares one scan between its clients; naming the listener
	// stops only ours and leaves e.g. the input_server mouse add-on scanning.
	BMessage request(BT_MSG_LE_SCAN_STOP);
	request.AddInt32("hci_id", _HCIID());
	request.AddMessenger("listener", BMessenger(this));
	BMessenger(BLUETOOTH_SIGNATURE).SendMessage(&request, (BHandler*)NULL,
		1000000);
}


void
NearbyScanner::_LEStopped()
{
	fLEState = LE_IDLE;
	fLEGeneration++;
	if (fPaused)
		_NotifyPausedIfIdle();
	else if (fEnabled)
		_StartLE();
}


void
NearbyScanner::_LEFailed(int32 status, bigtime_t retryDelay)
{
	fLEGeneration++;
	if (status != STATUS_SEARCHING)
		fFailures++;
	_SetStatus(status);
	if (fPaused)
		_NotifyPausedIfIdle();
	else if (fEnabled)
		_Timer(kMsgScanRetry, fLEGeneration, retryDelay);
}


void
NearbyScanner::_ScheduleInquiry(bigtime_t delay)
{
	fInquiryGeneration++;
	_Timer(kMsgInquiryDue, fInquiryGeneration, delay);
}


void
NearbyScanner::_StartInquiry()
{
	if (fAgent == NULL)
		return;

	InquiryListener* listener = new InquiryListener(BMessenger(this));
	fInquiryListener = listener;
	fInquiryActive = true;
	fInquiryGeneration++;
	TRACE("starting Classic inquiry");
	if (fAgent->StartInquiry(BT_GIAC, listener, kInquiryLength) != B_OK) {
		TRACE("inquiry request could not be sent");
		_EndInquiry();
		return;
	}
	_Timer(kMsgInquiryTimeout, fInquiryGeneration, kInquiryTimeout);
}


void
NearbyScanner::_EndInquiry()
{
	// Never cancels the inquiry in the controller (see Pause()); when the
	// window closes or the inquiry times out, the listener just goes away
	// and late results are dropped.
	if (!fInquiryActive)
		return;

	fInquiryActive = false;
	fInquiryGeneration++;

	Bluetooth::DiscoveryListener* listener = fInquiryListener;
	fInquiryListener = NULL;
	if (listener != NULL && listener->Lock()) {
		// The listener's list does not own the devices it created; this
		// inquiry's listener was the agent's last one.
		Bluetooth::RemoteDevicesList devices = fAgent->RetrieveDevices(0);
		listener->Quit();
		for (int32 i = 0; i < devices.CountItems(); i++)
			delete devices.ItemAt(i);
	}

	fListener->ClassicInquiryFinished();

	if (fPaused)
		_NotifyPausedIfIdle();
	else if (fEnabled)
		_ScheduleInquiry(kInquiryInterval);
}


void
NearbyScanner::_NotifyPausedIfIdle()
{
	if (!fPaused || fPauseNotified || fLEState != LE_IDLE || fInquiryActive)
		return;
	fPauseNotified = true;
	BMessenger(this).SendMessage(kMsgPausedNotify);
}


void
NearbyScanner::_SetStatus(int32 status)
{
	if (status == fStatus)
		return;
	fStatus = status;
	fListener->ScannerStatusChanged();
}


void
NearbyScanner::_Timer(uint32 what, int32 generation, bigtime_t delay)
{
	BMessage message(what);
	message.AddInt32("generation", generation);
	BMessageRunner::StartSending(BMessenger(this), &message, delay, 1);
}


int32
NearbyScanner::_HCIID() const
{
	return fDevice != NULL ? fDevice->ID() : -1;
}
