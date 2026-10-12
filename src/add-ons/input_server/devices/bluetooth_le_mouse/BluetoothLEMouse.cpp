/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 *
 * Connects bonded Bluetooth Low Energy mice (HID over GATT) and feeds their
 * reports to the input server, using the same speed, acceleration, button
 * map and click speed settings as wired mice.
 */

#include <LEAttributeClient.h>
#include <LEBondStore.h>
#include <LEDeviceStatus.h>
#include <LEHIDMouseDecoder.h>
#include <LEHIDService.h>
#include <LELog.h>
#include <LEPairingSession.h>

#include <InputServerDevice.h>
#include <InterfaceDefs.h>
#include <kb_mouse_settings.h>
#include <Message.h>
#include <OS.h>
#include <bluetooth/LocalDevice.h>
#include <bluetooth/bdaddrUtils.h>

#include <atomic>
#include <math.h>
#include <string.h>


using Bluetooth::LELog;
using Bluetooth::LE_LOG_ERROR;
using Bluetooth::LE_LOG_INFO;
using Bluetooth::LE_LOG_DEBUG;
using Bluetooth::LE_LOG_TRACE;

#define LOG(level, format, args...) LELog(level, "mouse", format, ##args)


namespace {

static const char* kDeviceName = "Bluetooth LE mouse";
// How long each reconnection scan waits for a bonded mouse to advertise.
static const bigtime_t kScanWindow = 15000000;


class BluetoothLEMouse : public BInputServerDevice {
public:
	BluetoothLEMouse()
		:
		fRunning(false),
		fThread(-1),
		fButtons(0),
		fClicks(0),
		fLastClick(0),
		fHistoryX(0),
		fHistoryY(0),
		fUpdateSettings(true)
	{
		fReference.name = (char*)kDeviceName;
		fReference.type = B_POINTING_DEVICE;
		fReference.cookie = this;
		memset(&fSettings, 0, sizeof(fSettings));
	}

	~BluetoothLEMouse() override
	{
		Stop(fReference.name, this);
	}

	status_t InitCheck() override
	{
		input_device_ref* devices[] = { &fReference, NULL };
		return RegisterDevices(devices);
	}

	status_t Start(const char*, void*) override
	{
		if (fRunning.exchange(true))
			return B_OK;
		fThread = spawn_thread(_Run, "Bluetooth LE mouse", B_NORMAL_PRIORITY,
			this);
		if (fThread < B_OK) {
			fRunning = false;
			return fThread;
		}
		resume_thread(fThread);
		return B_OK;
	}

	status_t Stop(const char*, void*) override
	{
		fRunning = false;
		if (fThread >= B_OK) {
			status_t result;
			wait_for_thread(fThread, &result);
			fThread = -1;
		}
		return B_OK;
	}

	status_t Control(const char*, void*, uint32 command, BMessage*) override
	{
		if (command == B_MOUSE_SPEED_CHANGED
			|| command == B_MOUSE_ACCELERATION_CHANGED
			|| command == B_MOUSE_MAP_CHANGED
			|| command == B_CLICK_SPEED_CHANGED)
			fUpdateSettings = true;
		return B_OK;
	}

private:
	static int32 _Run(void* cookie)
	{
		return ((BluetoothLEMouse*)cookie)->_Loop();
	}

	void _Sleep(bigtime_t duration)
	{
		for (bigtime_t slept = 0; slept < duration && fRunning;
				slept += 250000)
			snooze(250000);
	}

	int32 _Loop()
	{
		Bluetooth::LocalDevice* local = NULL;
		bool announcedIdle = false;
		while (fRunning) {
			char directory[1024];
			std::vector<Bluetooth::LEHIDMouseDevice> mice;
			if (Bluetooth::DefaultLEBondDirectory(directory,
					sizeof(directory)) != B_OK
				|| Bluetooth::ListLEHIDMice(directory, mice) != B_OK
				|| mice.empty()) {
				if (!announcedIdle) {
					LOG(LE_LOG_DEBUG, "no paired LE mice; idle");
					announcedIdle = true;
				}
				_Sleep(5000000);
				continue;
			}
			announcedIdle = false;

			// The Bluetooth server may start after us. LocalDevice objects
			// are owned by the kit and cannot be deleted, so keep one; only
			// a restarted server makes us let go of it (and leak it).
			if (local == NULL) {
				bigtime_t asked = system_time();
				local = Bluetooth::LocalDevice::GetLocalDevice();
				if (local != NULL) {
					LOG(LE_LOG_DEBUG, "adapter hci %" B_PRId32 " ready after %"
						B_PRId64 " ms", local->ID(),
						(system_time() - asked) / 1000);
				}
			}
			if (local == NULL) {
				// At boot the server and the radio come up after us; look
				// again soon, so that the mouse is back the moment they are.
				LOG(LE_LOG_DEBUG, "no Bluetooth adapter yet");
				_Sleep(1000000);
				continue;
			}
			bdaddr_t address = local->GetBluetoothAddress();
			bdaddr_t failed = bdaddrUtils::LocalAddress();
			if (memcmp(address.b, failed.b, sizeof(address.b)) == 0) {
				// The kit answers this when the server it was given the
				// adapter by is gone - after bluetooth_server restarted, say.
				// That adapter will never answer again; ask for the new one.
				LOG(LE_LOG_INFO, "the Bluetooth server is gone; asking the "
					"new one for its adapter");
				local = NULL;
				_Sleep(1000000);
				continue;
			}
			int32 hciID = local->ID();

			bool anyMatching = false;
			for (const Bluetooth::LEHIDMouseDevice& mouse : mice) {
				if (!fRunning)
					break;
				if (memcmp(address.b, mouse.localAddress, 6) != 0)
					continue;
				anyMatching = true;
				_UseMouse(hciID, mouse);
			}
			if (!anyMatching) {
				char text[18];
				LOG(LE_LOG_DEBUG, "no paired mouse belongs to adapter %s",
					Bluetooth::LEAddressString(address.b, text));
			}
			// Scanning already paced the loop; back off when nothing
			// belongs to this adapter.
			_Sleep(anyMatching ? 1000000 : 5000000);
		}
		return B_OK;
	}

	void _UseMouse(int32 hciID, const Bluetooth::LEHIDMouseDevice& mouse)
	{
		char text[18];
		Bluetooth::LEAddressString(mouse.peerAddress, text);
		Bluetooth::LEEncryptedLink link;
		status_t status = link.ConnectBonded(hciID, mouse.localAddress,
			mouse.peerAddress, mouse.peerAddressType, kScanWindow);
		if (status != B_OK) {
			if (status != B_TIMED_OUT) {
				LOG(LE_LOG_INFO, "mouse %s: connection attempt ended: %s",
					text, strerror(status));
				// Avoid hammering a peer that refuses us.
				_Sleep(5000000);
			}
			return;
		}
		bdaddr_t peer = {};
		link.ConnectionAddress(peer.b);
		Bluetooth::LEAttributeClient client;
		status = client.Connect(peer);
		if (status != B_OK) {
			LOG(LE_LOG_ERROR, "mouse %s: no ATT channel: %s", text,
				strerror(status));
			return;
		}
		Bluetooth::LEHIDService service;
		status = service.Discover(client);
		if (status != B_OK) {
			LOG(LE_LOG_ERROR, "mouse %s: HID discovery failed: %s", text,
				strerror(status));
			return;
		}
		Bluetooth::LEHIDMouseDecoder decoder;
		status = decoder.Init(service.ReportMap().data(),
			service.ReportMap().size());
		if (status == B_OK)
			status = service.EnableInputReports(client);
		if (status != B_OK) {
			LOG(LE_LOG_ERROR, "mouse %s: enabling input reports failed: %s",
				text, strerror(status));
			return;
		}
		LOG(LE_LOG_INFO, "mouse %s connected; %zu input report(s) enabled",
			text, service.InputReports().size());

		// Battery Service (optional): read it now, follow notifications,
		// and publish it with the connection state for status displays.
		uint16 batteryHandle = 0;
		bool batteryNotifies = false;
		int32 battery = _ReadBattery(client, batteryHandle, batteryNotifies);
		LOG(LE_LOG_INFO, "mouse %s battery: %s%" B_PRId32 "%s", text,
			battery < 0 ? "not reported (" : "", battery,
			battery < 0 ? ")" : "%");
		_Publish(mouse, true, battery);
		bigtime_t lastBatteryRead = system_time();

		fButtons = 0;
		fHistoryX = fHistoryY = 0;
		uint32 reports = 0;
		bigtime_t lastBondCheck = system_time();
		while (fRunning) {
			// Without notifications, poll the battery now and then.
			if (batteryHandle != 0 && !batteryNotifies
				&& system_time() - lastBatteryRead > 600000000) {
				lastBatteryRead = system_time();
				std::vector<uint8> level;
				if (client.ReadAttribute(batteryHandle, level) == B_OK
					&& !level.empty() && level[0] <= 100
					&& level[0] != battery) {
					battery = level[0];
					_Publish(mouse, true, battery);
				}
			}
			// Let "Forget" in the Bluetooth preferences end the session.
			if (system_time() - lastBondCheck > 5000000) {
				lastBondCheck = system_time();
				if (!_StillPaired(mouse)) {
					LOG(LE_LOG_INFO, "mouse %s was forgotten; disconnecting",
						text);
					status = B_CANCELED;
					break;
				}
			}
			uint16 handle;
			std::vector<uint8> value;
			status = client.ReadNotification(handle, value);
			if (status == B_TIMED_OUT) {
				if (!link.IsConnected()) {
					status = B_DEV_NOT_READY;
					break;
				}
				continue;
			}
			if (status != B_OK)
				break;
			if (handle == batteryHandle && batteryHandle != 0) {
				if (!value.empty() && value[0] <= 100 && value[0] != battery) {
					battery = value[0];
					LOG(LE_LOG_INFO, "mouse %s battery now %" B_PRId32 "%%",
						text, battery);
					_Publish(mouse, true, battery);
				}
				continue;
			}
			const Bluetooth::LEHIDInputReport* report
				= service.FindInputReport(handle);
			if (report == NULL)
				continue;
			_CountGap(text);
			if (reports++ < 3 || Bluetooth::LELogLevel() >= LE_LOG_TRACE) {
				Bluetooth::LELogHex(LE_LOG_DEBUG, "mouse", "input report",
					value.data(), value.size());
			}
			Bluetooth::LEMouseReport movement;
			if (decoder.Decode(report->reportID, value.data(), value.size(),
					movement) == B_OK)
				_Emit(movement);
		}
		LOG(LE_LOG_INFO, "mouse %s disconnected after %" B_PRIu32
			" reports: %s", text, reports, strerror(status));
		_Publish(mouse, false, battery);
		// A link may drop while a button is held. Release it before trying to
		// reconnect so the desktop cannot retain a stale pressed-button state.
		if (fButtons != 0) {
			Bluetooth::LEMouseReport released = {};
			_Emit(released);
		}
	}

	// Returns the battery level in percent, or -1. Enables notifications
	// when the characteristic supports them.
	int32 _ReadBattery(Bluetooth::LEAttributeClient& client, uint16& handle,
		bool& notifies)
	{
		handle = 0;
		notifies = false;
		std::vector<Bluetooth::LEPrimaryService> services;
		if (client.DiscoverPrimaryServices(services) != B_OK)
			return -1;
		for (const Bluetooth::LEPrimaryService& service : services) {
			if (service.uuid16 != 0x180f)
				continue;
			std::vector<Bluetooth::LECharacteristic> characteristics;
			if (client.DiscoverCharacteristics(service.startHandle,
					service.endHandle, characteristics) != B_OK)
				return -1;
			for (size_t i = 0; i < characteristics.size(); i++) {
				const Bluetooth::LECharacteristic& level = characteristics[i];
				if (level.uuid16 != 0x2a19)
					continue;
				handle = level.valueHandle;
				std::vector<uint8> value;
				if (client.ReadAttribute(handle, value) != B_OK
					|| value.empty() || value[0] > 100)
					return -1;
				if ((level.properties & 0x10) != 0) {
					uint16 end = i + 1 < characteristics.size()
						? characteristics[i + 1].declarationHandle - 1
						: service.endHandle;
					uint16 configuration;
					const uint8 enable[2] = { 1, 0 };
					notifies = end > handle
						&& client.FindClientConfiguration(handle + 1, end,
							configuration) == B_OK
						&& client.WriteAttribute(configuration, enable, 2)
							== B_OK;
				}
				return value[0];
			}
		}
		return -1;
	}

	void _Publish(const Bluetooth::LEHIDMouseDevice& mouse, bool connected,
		int32 battery)
	{
		Bluetooth::LEDeviceStatus status;
		memcpy(status.localAddress, mouse.localAddress, 6);
		memcpy(status.address, mouse.peerAddress, 6);
		status.addressType = mouse.peerAddressType;
		status.connected = connected;
		status.battery = battery;
		status.updated = system_time();
		status_t result = Bluetooth::PublishLEDeviceStatus(status);
		if (result != B_OK)
			LOG(LE_LOG_ERROR, "could not publish mouse status: %s",
				strerror(result));
	}

	bool _StillPaired(const Bluetooth::LEHIDMouseDevice& mouse)
	{
		char directory[1024];
		std::vector<Bluetooth::LEHIDMouseDevice> mice;
		if (Bluetooth::DefaultLEBondDirectory(directory, sizeof(directory))
				!= B_OK
			|| Bluetooth::ListLEHIDMice(directory, mice) != B_OK)
			return true;
		for (const Bluetooth::LEHIDMouseDevice& candidate : mice) {
			if (memcmp(candidate.peerAddress, mouse.peerAddress, 6) == 0
				&& memcmp(candidate.localAddress, mouse.localAddress, 6) == 0)
				return true;
		}
		return false;
	}

	void _UpdateSettingsIfNeeded()
	{
		if (!fUpdateSettings.exchange(false))
			return;
		if (get_mouse_speed(kDeviceName, &fSettings.accel.speed) != B_OK)
			fSettings.accel.speed = 65536;
		if (get_mouse_acceleration(kDeviceName,
				&fSettings.accel.accel_factor) != B_OK)
			fSettings.accel.accel_factor = 0;
		if (get_click_speed(kDeviceName, &fSettings.click_speed) != B_OK
			|| fSettings.click_speed <= 0)
			fSettings.click_speed = 500000;
		if (get_mouse_map(kDeviceName, &fSettings.map) != B_OK) {
			for (int i = 0; i < B_MAX_MOUSE_BUTTONS; i++)
				fSettings.map.button[i] = 1 << i;
		}
		LOG(LE_LOG_DEBUG, "settings: speed %" B_PRId32 " acceleration %"
			B_PRId32 " click speed %" B_PRId64, fSettings.accel.speed,
			fSettings.accel.accel_factor, fSettings.click_speed);
	}

	// Mirrors MouseDevice::_ComputeAcceleration() of the wired mouse add-on.
	void _Accelerate(int32 rawX, int32 rawY, int32& outX, int32& outY)
	{
		float deltaX = (float)rawX * fSettings.accel.speed / 65536.0
			+ fHistoryX;
		float deltaY = (float)rawY * fSettings.accel.speed / 65536.0
			+ fHistoryY;
		double acceleration = 1;
		if (fSettings.accel.accel_factor) {
			acceleration = 1 + sqrt(deltaX * deltaX + deltaY * deltaY)
				* fSettings.accel.accel_factor / 524288.0;
		}
		deltaX *= acceleration;
		deltaY *= acceleration;
		outX = deltaX >= 0 ? (int32)floorf(deltaX) : (int32)ceilf(deltaX);
		outY = deltaY >= 0 ? (int32)floorf(deltaY) : (int32)ceilf(deltaY);
		fHistoryX = deltaX - outX;
		fHistoryY = deltaY - outY;
	}

	// How smoothly reports arrive while the mouse moves: a report every
	// connection event (11.25 ms for most mice), gaps over 30 ms are missed
	// events the user sees as jerks. Gaps over 300 ms are the hand resting.
	void _CountGap(const char* mouse)
	{
		bigtime_t now = system_time();
		bigtime_t gap = now - fLastReport;
		fLastReport = now;
		if (gap > 300000)
			return;
		fGaps++;
		fMoving += gap;
		if (gap > 30000)
			fLate++;
		if (gap > 60000)
			fVeryLate++;
		if (gap > fLongest)
			fLongest = gap;
		if (now - fGapsSince < 30000000 || fGaps < 200)
			return;
		LOG(LE_LOG_INFO, "mouse %s smoothness: %" B_PRIu32 " reports in %.1f "
			"s moving (%.0f/s), %.1f%% of gaps over 30 ms, %.1f%% over 60 ms, "
			"longest %" B_PRId64 " ms", mouse, fGaps, fMoving / 1000000.0,
			fGaps * 1000000.0 / fMoving, 100.0 * fLate / fGaps,
			100.0 * fVeryLate / fGaps, fLongest / 1000);
		fGaps = fLate = fVeryLate = 0;
		fMoving = fLongest = 0;
		fGapsSince = now;
	}

	uint32 _RemapButtons(uint32 buttons) const
	{
		uint32 remapped = 0;
		for (int32 i = 0; buttons != 0 && i < B_MAX_MOUSE_BUTTONS; i++) {
			if ((buttons & 1) != 0)
				remapped |= fSettings.map.button[i];
			buttons >>= 1;
		}
		return remapped;
	}

	void _Emit(const Bluetooth::LEMouseReport& report)
	{
		_UpdateSettingsIfNeeded();
		bigtime_t now = system_time();
		// HID reports y and the wheel growing downwards; Haiku's mouse
		// events use the opposite sign (see the wired MouseProtocolHandler).
		int32 x, y;
		_Accelerate(report.x, -report.y, x, y);
		uint32 buttons = _RemapButtons(report.buttons);

		if (report.buttons != fButtons) {
			uint32 added = report.buttons & ~fButtons;
			BMessage* message = new BMessage(added != 0
				? B_MOUSE_DOWN : B_MOUSE_UP);
			message->AddInt64("when", now);
			message->AddInt32("be:device_subtype", B_MOUSE_POINTING_DEVICE);
			message->AddInt32("buttons", buttons);
			message->AddInt32("x", x);
			message->AddInt32("y", y);
			if (added != 0) {
				fClicks = now - fLastClick <= fSettings.click_speed
					? fClicks + 1 : 1;
				fLastClick = now;
				message->AddInt32("clicks", fClicks);
			}
			EnqueueMessage(message);
			fButtons = report.buttons;
			x = y = 0;
		}
		if (x != 0 || y != 0) {
			BMessage* message = new BMessage(B_MOUSE_MOVED);
			message->AddInt64("when", now);
			message->AddInt32("be:device_subtype", B_MOUSE_POINTING_DEVICE);
			message->AddInt32("buttons", buttons);
			message->AddInt32("x", x);
			message->AddInt32("y", y);
			EnqueueMessage(message);
		}
		if (report.wheelX != 0 || report.wheelY != 0) {
			BMessage* message = new BMessage(B_MOUSE_WHEEL_CHANGED);
			message->AddInt64("when", now);
			message->AddInt32("be:device_subtype", B_MOUSE_POINTING_DEVICE);
			message->AddFloat("be:wheel_delta_x", report.wheelX);
			message->AddFloat("be:wheel_delta_y", -report.wheelY);
			EnqueueMessage(message);
		}
	}

	input_device_ref fReference;
	std::atomic<bool> fRunning;
	thread_id fThread;
	uint32 fButtons;
	int32 fClicks;
	bigtime_t fLastClick;
	float fHistoryX;
	float fHistoryY;
	std::atomic<bool> fUpdateSettings;
	mouse_settings fSettings;

	bigtime_t fLastReport = 0;
	bigtime_t fGapsSince = 0;
	bigtime_t fMoving = 0;
	bigtime_t fLongest = 0;
	uint32 fGaps = 0;
	uint32 fLate = 0;
	uint32 fVeryLate = 0;
};

} // namespace


extern "C" BInputServerDevice*
instantiate_input_device()
{
	return new BluetoothLEMouse();
}
