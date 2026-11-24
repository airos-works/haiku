#!/usr/bin/env python3
# Writes the ACPI table ioctl_pointer_test wants in a virtual machine (QEMU:
# -acpitable file=ioctl_test.aml): a thermal zone, so that acpi_thermal has a
# device, and three methods for /dev/acpi/call to evaluate. As ASL:
#
#	DefinitionBlock ("", "SSDT", 2, "AIROS ", "IOCTLTST", 1)
#	{
#		Scope (\_TZ) {
#			ThermalZone (TZ00) {
#				Method (_TMP, 0) { Return (3010) }
#				Method (_CRT, 0) { Return (3732) }
#			}
#		}
#		Method (TST0, 0) { Return (7) }
#		Method (TST1, 1) { Return (Arg0) }
#		Method (TST2, 2) { Return (Add (Arg0, Arg1)) }
#	}
#
# (Assembled by hand: the build host has no iasl.)
import struct
import sys


def package(opcode, content):
	assert len(content) + 1 < 64
	return opcode + bytes([len(content) + 1]) + content


def method(name, argCount, body):
	return package(b'\x14', name.encode() + bytes([argCount]) + body)


def return_word(value):
	return b'\xa4\x0b' + struct.pack('<H', value)


zone = package(b'\x5b\x85', b'TZ00' + method('_TMP', 0, return_word(3010))
	+ method('_CRT', 0, return_word(3732)))
body = package(b'\x10', b'\\_TZ_' + zone)
body += method('TST0', 0, b'\xa4\x0a\x07')
body += method('TST1', 1, b'\xa4\x68')
body += method('TST2', 2, b'\xa4\x72\x68\x69\x00')

table = bytearray(b'SSDT' + struct.pack('<I', 36 + len(body)) + bytes([2, 0])
	+ b'AIROS ' + b'IOCTLTST' + struct.pack('<I', 1) + b'INTL'
	+ struct.pack('<I', 0x20200925) + body)
table[9] = -sum(table) & 0xff

with open(sys.argv[1] if len(sys.argv) > 1 else 'ioctl_test.aml', 'wb') as file:
	file.write(table)
