#!/usr/bin/env python3
# Copyright 2026, the air/OS project.
# Distributed under the terms of the MIT License.
"""USB devices that do what real ones should not, for a system in QEMU.

QEMU's own USB devices have fixed descriptors and never fail a transfer. Its
usb-redir device hands every request to whoever is at the other end of a
socket, though: this program, playing the "USB host" side of the usbredir
protocol with a device that exists only here. It can declare any descriptors
and answer any transfer with an error (QEMU's xHCI turns them into the
completion codes of a real controller: Stall, USB Transaction Error, Babble
Detected, and halts the endpoint as a real one does).

usage: usbredir_fake_device.py SOCKET DEVICE [option=value ...]

    listens on the unix socket SOCKET; then in the QEMU monitor
        chardev-add socket,id=fake,path=SOCKET
        device_add usb-redir,chardev=fake,id=fakedev,bus=xhci.0
    and to unplug
        device_del fakedev
        chardev-remove fake
    Serves one connection after the other until killed.

devices:
    vendor      class 0xff, an interrupt IN (0x81), a bulk IN (0x82) and a
                bulk OUT (0x02) endpoint, driven by usb_fake_device_test over
                usb_raw with vendor requests (see VendorDevice)
    alt-gap     the same, its endpoints in alternate setting 2 of an interface
                whose only other setting is 0
    no-interfaces  a configuration that says it has 0 interfaces and has one
    mouse       a HID mouse; options: xbits=N (the size of its X axis field,
                default 8), error=ioerror|babble|stall (answer every 20th
                report with it, or every Nth with every=N),
                period=SECONDS between reports (0.05)
    disk        a mass storage device (bulk only, SCSI) of 16 MiB of zeros;
                options: blocksize=N (512; what READ CAPACITY says),
                capacity_reply=N (bytes of the READ CAPACITY reply sent, 8)
"""

import os
import select
import socket
import struct
import sys
import time

# message types
HELLO, DEVICE_CONNECT, DEVICE_DISCONNECT, RESET, INTERFACE_INFO, EP_INFO, \
	SET_CONFIGURATION, GET_CONFIGURATION, CONFIGURATION_STATUS, \
	SET_ALT_SETTING, GET_ALT_SETTING, ALT_SETTING_STATUS, START_ISO_STREAM, \
	STOP_ISO_STREAM, ISO_STREAM_STATUS, START_INTERRUPT_RECEIVING, \
	STOP_INTERRUPT_RECEIVING, INTERRUPT_RECEIVING_STATUS, ALLOC_BULK_STREAMS, \
	FREE_BULK_STREAMS, BULK_STREAMS_STATUS, CANCEL_DATA_PACKET, \
	FILTER_REJECT, FILTER_FILTER, DEVICE_DISCONNECT_ACK = range(25)
CONTROL_PACKET, BULK_PACKET, ISO_PACKET, INTERRUPT_PACKET = range(100, 104)

# capabilities (bits of the first capability word)
CAP_CONNECT_DEVICE_VERSION = 1 << 1
CAP_EP_INFO_MAX_PACKET_SIZE = 1 << 4
CAP_64BITS_IDS = 1 << 5
CAP_32BITS_BULK_LENGTH = 1 << 6
OUR_CAPABILITIES = CAP_CONNECT_DEVICE_VERSION | CAP_EP_INFO_MAX_PACKET_SIZE \
	| CAP_64BITS_IDS | CAP_32BITS_BULK_LENGTH
	# QEMU refuses a device without these on an xHCI port. Not offered: bulk
	# streams, bulk receiving, filters.

SUCCESS, CANCELLED, INVAL, IOERROR, STALL, TIMEOUT, BABBLE = range(7)
STATUS_NAMES = {"ok": SUCCESS, "ioerror": IOERROR, "stall": STALL,
	"timeout": TIMEOUT, "babble": BABBLE}

TYPE_CONTROL, TYPE_ISO, TYPE_BULK, TYPE_INTERRUPT, TYPE_INVALID = 0, 1, 2, 3, 255
SPEED_FULL, SPEED_HIGH = 1, 2

START = time.time()


def log(text):
	print("%8.3f %s" % (time.time() - START, text), flush=True)


def pattern(length, seed=0):
	"""The data the fake devices send: byte i is (seed + i * 7) & 0xff."""
	return bytes((seed + i * 7) & 0xff for i in range(length))


def endpoint_index(address):
	return ((address & 0x80) >> 3) | (address & 0x0f)


def endpoint_descriptor(address, attributes, max_packet_size, interval=0):
	return struct.pack("<BBBBHB", 7, 5, address, attributes, max_packet_size,
		interval)


def interface_descriptor(number, alternate, endpoints, klass, subclass=0,
		protocol=0):
	return struct.pack("<BBBBBBBBB", 9, 4, number, alternate, endpoints,
		klass, subclass, protocol, 0)


def configuration_descriptor(interfaces, rest):
	return struct.pack("<BBHBBBBB", 9, 2, 9 + len(rest), interfaces, 1, 0,
		0x80, 50) + rest


class Device:
	"""What every fake device has; the subclasses fill in the rest."""
	speed = SPEED_HIGH
	device_class = 0xff
	vendor = 0x1209		# pid.codes, test devices
	product = 0x0001
	# (address, type, max packet size, interval, interface) of the endpoints
	# of the first configuration
	endpoints = ()
	interfaces = ((0, 0xff, 0, 0),)		# (number, class, subclass, protocol)

	def __init__(self, link, options):
		self.link = link
		self.options = options

	def device_descriptor(self):
		return struct.pack("<BBHBBBBHHHBBBB", 18, 1, 0x0200, self.device_class,
			0, 0, 64, self.vendor, self.product, 0x0100, 0, 0, 0, 1)

	def configuration(self):
		raise NotImplementedError

	def control(self, request_type, request, value, index, length, data):
		"""Returns (status, data) for a request that is not a standard one
		answered by Link."""
		return STALL, b""

	def bulk_out(self, endpoint, data):
		return SUCCESS

	def bulk_in(self, endpoint, length):
		return SUCCESS, b""

	def interrupt_started(self, endpoint):
		pass

	def set_alt(self, interface, alternate):
		return SUCCESS

	def tick(self):
		"""Called every few milliseconds while connected."""


class VendorDevice(Device):
	"""Does as usb_fake_device_test tells it with vendor requests to the
	device (request type 0x40, no data):

	0x10  the next transfer on endpoint wIndex ends with status wValue
	      (3 I/O error, 4 stall, 5 timeout, 6 babble). For the interrupt
	      endpoint this sends a packet with that status at once.
	0x11  the next IN transfer on bulk endpoint wIndex returns wValue bytes
	      (instead of as many as asked for)
	0x12  send wValue bytes on interrupt endpoint wIndex now
	0xc0/0x20 (IN)  returns how many bulk IN transfers were answered, and the
	      total of the bulk OUT bytes and the sum of them, as three uint32
	Bulk IN data is pattern(length, seed) with the seed counting up from 0
	with every transfer.
	"""
	product = 0x0001
	endpoints = ((0x81, TYPE_INTERRUPT, 64, 4, 0), (0x82, TYPE_BULK, 512, 0, 0),
		(0x02, TYPE_BULK, 512, 0, 0))

	def __init__(self, link, options):
		super().__init__(link, options)
		self.next_status = {}
		self.next_length = {}
		self.seed = 0
		self.out_bytes = 0
		self.out_sum = 0

	def endpoint_descriptors(self):
		return endpoint_descriptor(0x81, 3, 64, 4) \
			+ endpoint_descriptor(0x82, 2, 512) \
			+ endpoint_descriptor(0x02, 2, 512)

	def configuration(self):
		return configuration_descriptor(1,
			interface_descriptor(0, 0, 3, 0xff) + self.endpoint_descriptors())

	def control(self, request_type, request, value, index, length, data):
		if request_type == 0x40 and request == 0x10:
			if index == 0x81:
				self.link.send_interrupt(index, pattern(8), value)
			else:
				self.next_status[index] = value
			return SUCCESS, b""
		if request_type == 0x40 and request == 0x11:
			self.next_length[index] = value
			return SUCCESS, b""
		if request_type == 0x40 and request == 0x12:
			self.link.send_interrupt(index, pattern(value, self.seed), SUCCESS)
			self.seed += 1
			return SUCCESS, b""
		if request_type == 0xc0 and request == 0x20:
			return SUCCESS, struct.pack("<III", self.seed, self.out_bytes,
				self.out_sum & 0xffffffff)[:length]
		return STALL, b""

	def bulk_out(self, endpoint, data):
		status = self.next_status.pop(endpoint, SUCCESS)
		if status == SUCCESS:
			self.out_bytes += len(data)
			self.out_sum += sum(data)
		return status

	def bulk_in(self, endpoint, length):
		status = self.next_status.pop(endpoint, SUCCESS)
		if status != SUCCESS:
			return status, b""
		length = min(length, self.next_length.pop(endpoint, length))
		data = pattern(length, self.seed)
		self.seed += 1
		return SUCCESS, data


class AltGapDevice(VendorDevice):
	"""The endpoints are in alternate setting 2; there is no setting 1."""
	product = 0x0002
	endpoints = ()

	def configuration(self):
		return configuration_descriptor(1,
			interface_descriptor(0, 0, 0, 0xff)
			+ interface_descriptor(0, 2, 3, 0xff) + self.endpoint_descriptors())

	def set_alt(self, interface, alternate):
		if interface != 0 or alternate not in (0, 2):
			return STALL
		self.endpoints = VendorDevice.endpoints if alternate == 2 else ()
		return SUCCESS


class NoInterfacesDevice(VendorDevice):
	"""bNumInterfaces is 0, and an interface with endpoints follows."""
	product = 0x0003

	def configuration(self):
		return configuration_descriptor(0,
			interface_descriptor(0, 0, 3, 0xff) + self.endpoint_descriptors())


class MouseDevice(Device):
	product = 0x0004
	speed = SPEED_FULL
	device_class = 0
	endpoints = ((0x81, TYPE_INTERRUPT, 64, 10, 0),)
	interfaces = ((0, 3, 0, 0),)

	def __init__(self, link, options):
		super().__init__(link, options)
		self.xbits = int(options.get("xbits", 8))
		self.error = STATUS_NAMES[options.get("error", "ok")]
		self.every = int(options.get("every", 20))
		self.period = float(options.get("period", 0.05))
		self.started = False
		self.next = 0
		self.count = 0

	def report_descriptor(self):
		xsize = bytes([0x75, self.xbits]) if self.xbits < 256 \
			else bytes([0x76]) + struct.pack("<H", self.xbits)
		# an X field of up to 32 bits has the range of its size, so that
		# its value (0x41 in every byte) is a valid one
		xrange = b""
		if 8 < self.xbits <= 32:
			xrange = bytes([0x17]) + struct.pack("<i", -(1 << (self.xbits - 1))) \
				+ bytes([0x27]) + struct.pack("<i", (1 << (self.xbits - 1)) - 1)
		return bytes([
			0x05, 0x01, 0x09, 0x02, 0xa1, 0x01,		# mouse, application
			0x09, 0x01, 0xa1, 0x00,					# pointer, physical
			0x05, 0x09, 0x19, 0x01, 0x29, 0x03,		# buttons 1 to 3
			0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x03, 0x81, 0x02,
			0x75, 0x05, 0x95, 0x01, 0x81, 0x01,		# 5 bits of padding
			0x05, 0x01, 0x15, 0x81, 0x25, 0x7f])	\
			+ xrange + bytes([0x09, 0x30]) + xsize			\
			+ bytes([0x95, 0x01, 0x81, 0x06,				# X
			0x15, 0x81, 0x25, 0x7f,							# -127 to 127
			0x09, 0x31, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06,	# Y
			0xc0, 0xc0])

	def report_length(self):
		return 1 + (self.xbits + 7) // 8 + 1

	def configuration(self):
		hid = struct.pack("<BBHBBBH", 9, 0x21, 0x0111, 0, 1, 0x22,
			len(self.report_descriptor()))
		return configuration_descriptor(1,
			interface_descriptor(0, 0, 1, 3) + hid
			+ endpoint_descriptor(0x81, 3, 64, 10))

	def control(self, request_type, request, value, index, length, data):
		if request_type == 0x81 and request == 6 and value >> 8 == 0x22:
			return SUCCESS, self.report_descriptor()[:length]
		if request_type == 0x21 and request in (0x0a, 0x0b):
			return SUCCESS, b""		# set idle, set protocol
		if request_type == 0xa1 and request == 0x01:
			return SUCCESS, bytes(self.report_length())[:length]
		return STALL, b""

	def interrupt_started(self, endpoint):
		self.started = True

	def tick(self):
		if not self.started or time.time() < self.next:
			return
		self.next = time.time() + self.period
		self.count += 1
		# Y moves by 1, the middle button is down in every second report, and
		# the X field is all 0x41 ('A'): what ends up where it should not.
		report = bytes([0x04 if self.count % 2 else 0x00]) \
			+ b"\x41" * ((self.xbits + 7) // 8) + b"\x01"
		status = SUCCESS
		if self.error != SUCCESS and self.count % self.every == 0:
			status = self.error
			log("report %d with status %d" % (self.count, status))
		self.link.send_interrupt(0x81, report, status)


class DiskDevice(Device):
	product = 0x0005
	device_class = 0
	endpoints = ((0x81, TYPE_BULK, 512, 0, 0), (0x02, TYPE_BULK, 512, 0, 0))
	interfaces = ((0, 8, 6, 0x50),)
	BLOCKS = 32768

	def __init__(self, link, options):
		super().__init__(link, options)
		self.block_size = int(options.get("blocksize", 512))
		self.capacity_reply = int(options.get("capacity_reply", 8))
		self.pending = []		# what the next bulk IN transfers return
		self.sense = (0, 0, 0)
		self.expect_data = 0
		self.tag = 0

	def configuration(self):
		return configuration_descriptor(1,
			interface_descriptor(0, 0, 2, 8, 6, 0x50)
			+ endpoint_descriptor(0x81, 2, 512)
			+ endpoint_descriptor(0x02, 2, 512))

	def control(self, request_type, request, value, index, length, data):
		if request_type == 0xa1 and request == 0xfe:
			return SUCCESS, b"\x00"		# the highest LUN
		if request_type == 0x21 and request == 0xff:
			self.pending = []
			self.expect_data = 0
			return SUCCESS, b""			# mass storage reset
		return STALL, b""

	def bulk_out(self, endpoint, data):
		if self.expect_data > 0:
			self.expect_data -= len(data)
			if self.expect_data <= 0:
				self.pending.append(self.status(0, 0))
			return SUCCESS
		if len(data) != 31 or data[:4] != b"USBC":
			log("not a command block: %d bytes" % len(data))
			return STALL
		self.tag, length, flags, lun, command_length = struct.unpack(
			"<IIBBB", data[4:15])
		command = data[15:15 + command_length]
		reply, ok = self.scsi(command, length)
		log("SCSI %s -> %d bytes%s" % (command.hex(), len(reply),
			"" if ok else ", failed"))
		if flags & 0x80:
			reply = reply[:length]
			if length > 0 and (reply or ok):
				self.pending.append(reply)
			self.pending.append(self.status(length - len(reply), 0 if ok else 1))
		elif length > 0 and ok:
			self.expect_data = length
		else:
			self.pending.append(self.status(length, 0 if ok else 1))
		return SUCCESS

	def status(self, residue, status):
		return struct.pack("<4sIIB", b"USBS", self.tag, max(residue, 0), status)

	def scsi(self, command, length):
		operation = command[0]
		if operation == 0x00:		# test unit ready
			return b"", True
		if operation == 0x03:		# request sense
			key, asc, ascq = self.sense
			self.sense = (0, 0, 0)
			return struct.pack(">BBBIBIBB", 0x70, 0, key, 0, 10, 0, asc, ascq) \
				+ bytes(4), True
		if operation == 0x12:		# inquiry
			return struct.pack(">BBBBB3x8s16s4s", 0, 0x80, 4, 2, 31,
				b"FAKE    ", b"odd disk        ", b"0001"), True
		if operation == 0x25:		# read capacity (10)
			return struct.pack(">II", self.BLOCKS - 1,
				self.block_size)[:self.capacity_reply], True
		if operation == 0x28:		# read (10)
			blocks = struct.unpack(">H", command[7:9])[0]
			return bytes(blocks * 512), True
		if operation == 0x2a:		# write (10)
			return b"", True
		if operation in (0x1b, 0x1e, 0x35):	# start/stop, lock, synchronize
			return b"", True
		self.sense = (5, 0x20, 0)	# illegal request, invalid command
		return b"", False

	def bulk_in(self, endpoint, length):
		if not self.pending:
			return STALL, b""
		return SUCCESS, self.pending.pop(0)[:length]


DEVICES = {"vendor": VendorDevice, "alt-gap": AltGapDevice,
	"no-interfaces": NoInterfacesDevice, "mouse": MouseDevice,
	"disk": DiskDevice}


class Link:
	"""One connection of QEMU's usb-redir device."""

	def __init__(self, connection, device_class, options):
		self.connection = connection
		self.buffer = b""
		self.device = device_class(self, options)
		self.configuration = 0
		self.peer_capabilities = None
		self.next_id = 0x40000000
		self.receiving = set()		# interrupt endpoints QEMU reads from
		self.waiting = {}			# packets for the others

	def header_format(self):
		# ids are 64 bits once both sides have said that they can
		if self.peer_capabilities is not None \
				and self.peer_capabilities & CAP_64BITS_IDS:
			return "<IIQ"
		return "<III"

	def send(self, message_type, message_id, payload):
		self.connection.sendall(struct.pack(self.header_format(),
			message_type, len(payload), message_id) + payload)

	def send_interrupt(self, endpoint, data, status):
		# QEMU asks for the packets of an interrupt endpoint when the guest
		# first reads from it, and drops what comes before: keep them.
		if endpoint not in self.receiving:
			self.waiting.setdefault(endpoint, []).append((data, status))
			return
		self.next_id += 1
		self.send(INTERRUPT_PACKET, self.next_id, struct.pack("<BBH",
			endpoint, status, len(data)) + data)

	def send_device(self):
		device = self.device
		types = [TYPE_INVALID] * 32
		intervals = [0] * 32
		interfaces = [0] * 32
		sizes = [0] * 32
		types[0] = types[16] = TYPE_CONTROL
		for address, kind, size, interval, interface in device.endpoints:
			index = endpoint_index(address)
			types[index] = kind
			intervals[index] = interval
			interfaces[index] = interface
			sizes[index] = size
		self.send(EP_INFO, 0, bytes(types) + bytes(intervals)
			+ bytes(interfaces) + struct.pack("<32H", *sizes))

		info = [[0] * 32 for i in range(4)]
		for i, interface in enumerate(device.interfaces):
			for j in range(4):
				info[j][i] = interface[j]
		self.send(INTERFACE_INFO, 0, struct.pack("<I", len(device.interfaces))
			+ b"".join(bytes(row) for row in info))

	def hello(self):
		self.send(HELLO, 0, b"usbredir_fake_device 1".ljust(64, b"\0")
			+ struct.pack("<I", OUR_CAPABILITIES))

	def connect(self):
		device = self.device
		self.send_device()
		payload = struct.pack("<BBBBHH", device.speed, device.device_class,
			0, 0, device.vendor, device.product)
		if self.peer_capabilities & CAP_CONNECT_DEVICE_VERSION:
			payload += struct.pack("<H", 0x0100)
		self.send(DEVICE_CONNECT, 0, payload)
		log("connected as %04x:%04x" % (device.vendor, device.product))

	def standard_request(self, request_type, request, value, index, length):
		"""The answer to a standard request, or None."""
		device = self.device
		if request_type == 0x80 and request == 6:
			kind = value >> 8
			if kind == 1:
				return SUCCESS, device.device_descriptor()[:length]
			if kind == 2:
				return SUCCESS, device.configuration()[:length]
			if kind == 3 and value & 0xff == 0:
				return SUCCESS, bytes([4, 3, 0x09, 0x04])[:length]
			return STALL, b""
		if request_type == 0x80 and request == 0:		# get status
			return SUCCESS, bytes(2)[:length]
		if request_type & 0x60 == 0 and request in (1, 3):
			return SUCCESS, b""		# clear/set feature (clear halt, too)
		return None

	def handle(self, message_type, message_id, payload):
		device = self.device
		if message_type == HELLO:
			self.peer_capabilities = struct.unpack("<I", payload[64:68])[0]
			log("hello from %s, capabilities 0x%x" % (
				payload[:64].rstrip(b"\0").decode(errors="replace"),
				self.peer_capabilities))
			self.connect()
		elif message_type == RESET:
			log("reset")
		elif message_type == SET_CONFIGURATION:
			self.configuration = payload[0]
			log("set configuration %d" % self.configuration)
			self.send_device()
			self.send(CONFIGURATION_STATUS, message_id,
				bytes([SUCCESS, self.configuration]))
		elif message_type == GET_CONFIGURATION:
			self.send(CONFIGURATION_STATUS, message_id,
				bytes([SUCCESS, self.configuration]))
		elif message_type == SET_ALT_SETTING:
			status = device.set_alt(payload[0], payload[1])
			log("set interface %d to alternate setting %d: status %d"
				% (payload[0], payload[1], status))
			self.send_device()
			self.send(ALT_SETTING_STATUS, message_id,
				bytes([status, payload[0], payload[1]]))
		elif message_type == GET_ALT_SETTING:
			self.send(ALT_SETTING_STATUS, message_id,
				bytes([SUCCESS, payload[0], 0]))
		elif message_type == START_INTERRUPT_RECEIVING:
			log("interrupt receiving started on 0x%02x" % payload[0])
			self.send(INTERRUPT_RECEIVING_STATUS, message_id,
				bytes([SUCCESS, payload[0]]))
			self.receiving.add(payload[0])
			for data, status in self.waiting.pop(payload[0], []):
				self.send_interrupt(payload[0], data, status)
			device.interrupt_started(payload[0])
		elif message_type == STOP_INTERRUPT_RECEIVING:
			log("interrupt receiving stopped on 0x%02x" % payload[0])
			self.receiving.discard(payload[0])
			self.send(INTERRUPT_RECEIVING_STATUS, message_id,
				bytes([SUCCESS, payload[0]]))
		elif message_type == CONTROL_PACKET:
			endpoint, request, request_type, status, value, index, length \
				= struct.unpack("<BBBBHHH", payload[:10])
			answer = self.standard_request(request_type, request, value,
				index, length)
			if answer is None:
				answer = device.control(request_type, request, value, index,
					length, payload[10:])
			status, data = answer
			if not request_type & 0x80:
				data = b""
			if request_type & 0x60 or status != SUCCESS:
				log("control %02x %02x value 0x%04x index 0x%04x length %d: "
					"status %d, %d bytes" % (request_type, request, value,
					index, length, status, len(data)))
			self.send(CONTROL_PACKET, message_id, struct.pack("<BBBBHHH",
				endpoint, request, request_type, status, value, index,
				len(data) if request_type & 0x80 else length) + data)
		elif message_type == BULK_PACKET:
			endpoint, status, length, stream, length_high = struct.unpack(
				"<BBHIH", payload[:10])
			length |= length_high << 16
			if endpoint & 0x80:
				status, data = device.bulk_in(endpoint, length)
				if status != SUCCESS:
					log("bulk in 0x%02x: status %d" % (endpoint, status))
				length = len(data)
			else:
				status = device.bulk_out(endpoint, payload[10:])
				if status != SUCCESS:
					log("bulk out 0x%02x: status %d" % (endpoint, status))
				data = b""
			self.send(BULK_PACKET, message_id, struct.pack("<BBHIH",
				endpoint, status, length & 0xffff, stream, length >> 16)
				+ data)
		elif message_type == CANCEL_DATA_PACKET:
			# everything is answered at once: nothing is left to cancel
			pass
		elif message_type in (DEVICE_DISCONNECT_ACK, FILTER_FILTER):
			pass
		else:
			log("message type %d (%d bytes) ignored" % (message_type,
				len(payload)))

	def run(self):
		self.hello()
		while True:
			readable, _, _ = select.select([self.connection], [], [], 0.005)
			if readable:
				data = self.connection.recv(65536)
				if not data:
					return
				self.buffer += data
				while True:
					header = struct.calcsize(self.header_format())
					if len(self.buffer) < header:
						break
					message_type, length, message_id = struct.unpack(
						self.header_format(), self.buffer[:header])
					if len(self.buffer) < header + length:
						break
					payload = self.buffer[header:header + length]
					self.buffer = self.buffer[header + length:]
					self.handle(message_type, message_id, payload)
			self.device.tick()


def main():
	if len(sys.argv) < 3 or sys.argv[2] not in DEVICES:
		sys.exit(__doc__)
	path = sys.argv[1]
	options = dict(option.split("=", 1) for option in sys.argv[3:])
	if os.path.exists(path):
		os.unlink(path)
	server = socket.socket(socket.AF_UNIX)
	server.bind(path)
	server.listen(1)
	log("%s device waiting on %s" % (sys.argv[2], path))
	while True:
		connection, _ = server.accept()
		log("QEMU connected")
		try:
			Link(connection, DEVICES[sys.argv[2]], options).run()
		except (BrokenPipeError, ConnectionResetError):
			pass
		connection.close()
		log("QEMU disconnected")


if __name__ == "__main__":
	main()
