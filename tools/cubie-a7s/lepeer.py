#!/usr/bin/env python3
"""Synthetic LE peripheral for timing a central's connection events.

lepeer.py [seconds] [period ms]

Advertises "airos-probe" (connectable) with one service whose characteristic
can be read and notifies: once subscribed, it queues a notification every
<period> ms carrying a sequence number (uint32 LE) and this host's monotonic
clock in microseconds (uint32 LE, wraps). The central times arrivals; the
sequence numbers show nothing is lost, the stamps show queueing delay.

Uses BlueZ over D-Bus only (no adapter settings change): registering an
advertisement and a GATT application, both removed on exit.
"""
import struct
import sys
import time

import dbus
import dbus.exceptions
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

BLUEZ = "org.bluez"
ADAPTER = "/org/bluez/hci0"
SERVICE_UUID = "c0be0001-a1f0-4b5e-9d3c-7a1b2c3d4e5f"
CHAR_UUID = "c0be0002-a1f0-4b5e-9d3c-7a1b2c3d4e5f"
PROPS = "org.freedesktop.DBus.Properties"
OM = "org.freedesktop.DBus.ObjectManager"

seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 600
period = float(sys.argv[2]) if len(sys.argv) > 2 else 2.0


def log(text):
	print(time.strftime("%H:%M:%S", time.gmtime()) + " " + text, flush=True)


class Advertisement(dbus.service.Object):
	PATH = "/org/airos/probe/advertisement0"

	def __init__(self, bus):
		super().__init__(bus, self.PATH)

	@dbus.service.method(PROPS, in_signature="ss", out_signature="v")
	def Get(self, interface, name):
		return self.GetAll(interface)[name]

	@dbus.service.method(PROPS, in_signature="s", out_signature="a{sv}")
	def GetAll(self, interface):
		return {
			"Type": "peripheral",
			"ServiceUUIDs": dbus.Array([SERVICE_UUID], signature="s"),
			"LocalName": "airos-probe",
		}

	@dbus.service.method("org.bluez.LEAdvertisement1")
	def Release(self):
		log("advertisement released")


class Characteristic(dbus.service.Object):
	PATH = "/org/airos/probe/service0/char0"

	def __init__(self, bus):
		super().__init__(bus, self.PATH)
		self.notifying = False
		self.sequence = 0
		self.timer = None
		self.started = 0

	def properties(self):
		return {
			"org.bluez.GattCharacteristic1": {
				"Service": dbus.ObjectPath(Service.PATH),
				"UUID": CHAR_UUID,
				"Flags": dbus.Array(["read", "notify"], signature="s"),
			}
		}

	def value(self):
		stamp = int(time.monotonic() * 1e6) & 0xffffffff
		return dbus.Array(
			[dbus.Byte(b) for b in struct.pack("<II", self.sequence, stamp)],
			signature="y")

	@dbus.service.method(PROPS, in_signature="s", out_signature="a{sv}")
	def GetAll(self, interface):
		return self.properties()[interface]

	@dbus.service.method("org.bluez.GattCharacteristic1",
		in_signature="a{sv}", out_signature="ay")
	def ReadValue(self, options):
		return self.value()

	@dbus.service.method("org.bluez.GattCharacteristic1")
	def StartNotify(self):
		if self.notifying:
			return
		self.notifying = True
		self.started = time.monotonic()
		self.sequence = 0
		log(f"subscribed; notifying every {period} ms")
		self.timer = GLib.timeout_add(max(1, int(period)), self.tick)

	@dbus.service.method("org.bluez.GattCharacteristic1")
	def StopNotify(self):
		if self.notifying:
			log(f"unsubscribed after {self.sequence} notifications in "
				f"{time.monotonic() - self.started:.1f} s")
		self.notifying = False

	def tick(self):
		if not self.notifying:
			return False
		self.sequence += 1
		self.PropertiesChanged("org.bluez.GattCharacteristic1",
			{"Value": self.value()}, [])
		return True

	@dbus.service.signal(PROPS, signature="sa{sv}as")
	def PropertiesChanged(self, interface, changed, invalidated):
		pass


class Service(dbus.service.Object):
	PATH = "/org/airos/probe/service0"

	def __init__(self, bus):
		super().__init__(bus, self.PATH)

	def properties(self):
		return {
			"org.bluez.GattService1": {
				"UUID": SERVICE_UUID,
				"Primary": True,
				"Characteristics": dbus.Array([dbus.ObjectPath(Characteristic.PATH)],
					signature="o"),
			}
		}

	@dbus.service.method(PROPS, in_signature="s", out_signature="a{sv}")
	def GetAll(self, interface):
		return self.properties()[interface]


class Application(dbus.service.Object):
	PATH = "/org/airos/probe"

	def __init__(self, bus):
		super().__init__(bus, self.PATH)
		self.service = Service(bus)
		self.characteristic = Characteristic(bus)

	@dbus.service.method(OM, out_signature="a{oa{sa{sv}}}")
	def GetManagedObjects(self):
		return {
			dbus.ObjectPath(Service.PATH): self.service.properties(),
			dbus.ObjectPath(Characteristic.PATH):
				self.characteristic.properties(),
		}


def main():
	dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
	bus = dbus.SystemBus()
	adapter = bus.get_object(BLUEZ, ADAPTER)
	gatt = dbus.Interface(adapter, "org.bluez.GattManager1")
	advertising = dbus.Interface(adapter, "org.bluez.LEAdvertisingManager1")
	application = Application(bus)
	advertisement = Advertisement(bus)
	loop = GLib.MainLoop()

	def failed(what):
		def handler(error):
			log(f"{what} failed: {error}")
			loop.quit()
		return handler

	gatt.RegisterApplication(Application.PATH, {},
		reply_handler=lambda: log("GATT application registered"),
		error_handler=failed("RegisterApplication"))
	advertising.RegisterAdvertisement(Advertisement.PATH, {},
		reply_handler=lambda: log("advertising as airos-probe"),
		error_handler=failed("RegisterAdvertisement"))
	GLib.timeout_add(int(seconds * 1000), loop.quit)
	try:
		loop.run()
	except KeyboardInterrupt:
		pass
	finally:
		for call in (lambda: advertising.UnregisterAdvertisement(
				Advertisement.PATH),
				lambda: gatt.UnregisterApplication(Application.PATH)):
			try:
				call()
			except dbus.exceptions.DBusException as error:
				log(f"cleanup: {error}")
		log("stopped")


if __name__ == "__main__":
	main()
