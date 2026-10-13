/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _SDP_HID_RECORD_H_
#define _SDP_HID_RECORD_H_


#include <DataIO.h>
#include <Message.h>

#include <bluetooth/SDPBuffer.h>
#include <bluetooth/sdp.h>


/*!	Reading the HID parts of the SDP records SDPClient collects: one BMessage
	per record, attributes named "0x%04X", sequences as their raw contents.
*/
namespace Bluetooth {


static const uint8 kSDPHIDDescriptorReport = 0x22;


/*!	Reads the header of the SDP data element at \a offset (Core Vol 3 Part B
	3.3): its type descriptor and the size of its data, which then starts at
	\a offset.
*/
inline bool
SDPReadElementHeader(const uint8* data, size_t size, size_t& offset,
	uint8& type, size_t& length)
{
	if (offset >= size)
		return false;

	uint8 header = data[offset++];
	type = header >> 3;
	uint8 sizeIndex = header & 0x07;
	switch (sizeIndex) {
		case 0:
			length = type == 0 ? 0 : 1;
			break;
		case 1:
			length = 2;
			break;
		case 2:
			length = 4;
			break;
		case 3:
			length = 8;
			break;
		case 4:
			length = 16;
			break;
		case 5:
			if (offset + 1 > size)
				return false;
			length = data[offset];
			offset += 1;
			break;
		case 6:
			if (offset + 2 > size)
				return false;
			length = (data[offset] << 8) | data[offset + 1];
			offset += 2;
			break;
		default:
			if (offset + 4 > size)
				return false;
			length = ((uint32)data[offset] << 24) | (data[offset + 1] << 16)
				| (data[offset + 2] << 8) | data[offset + 3];
			offset += 4;
			break;
	}
	return length <= size - offset;
}


/*!	Finds the report descriptor in the contents of a HIDDescriptorList
	attribute (0x0206): a sequence of (descriptor type, descriptor)
	sequences (HID Profile 1.1, 5.3.4.11).
*/
inline bool
SDPFindHIDReportDescriptor(const uint8* data, size_t size,
	const uint8*& descriptor, size_t& descriptorLength)
{
	enum { kSequence = 6, kUnsigned = 1, kString = 4 };

	size_t offset = 0;
	while (offset < size) {
		uint8 type;
		size_t length;
		if (!SDPReadElementHeader(data, size, offset, type, length))
			return false;
		size_t end = offset + length;
		if (type != kSequence) {
			offset = end;
			continue;
		}

		uint8 classType;
		size_t classLength;
		if (!SDPReadElementHeader(data, end, offset, classType, classLength)
			|| classType != kUnsigned || classLength != 1)
			return false;
		uint8 descriptorType = data[offset];
		offset += classLength;

		uint8 valueType;
		size_t valueLength;
		if (!SDPReadElementHeader(data, end, offset, valueType, valueLength))
			return false;
		if (descriptorType == kSDPHIDDescriptorReport && valueType == kString
			&& valueLength > 0) {
			descriptor = data + offset;
			descriptorLength = valueLength;
			return true;
		}
		offset = end;
	}
	return false;
}


/*!	Splits the AttributeLists of a ServiceSearchAttribute response (its
	parts joined) into one BMessage per record, added to \a services as
	"service". The data comes from the remote device: a record that does
	not parse ends the parsing.
*/
inline status_t
SDPParseAttributeLists(BPositionIO* data, BMessage& services)
{
	data->Seek(0, SEEK_SET);
	SDPDataIO attributeLists(data);
	attributeLists.ReadSeq();
	if (attributeLists.ReadingStatus() != B_OK)
		return attributeLists.ReadingStatus();

	size_t recordSize;
	while ((recordSize = attributeLists.ReadSeq()) != 0
		&& attributeLists.ReadingStatus() == B_OK) {
		off_t end = data->Position() + recordSize;
		BMessage record;
		while (data->Position() < end
			&& attributeLists.ReadingStatus() == B_OK) {
			uint16 id = attributeLists.ReadNumber<uint16>();

			// attribute IDs in hex, as the specifications write them
			BString name;
			name.SetToFormat("0x%04X", id);
			attributeLists.ParseNext(&record, name);
		}
		if (attributeLists.ReadingStatus() != B_OK)
			return attributeLists.ReadingStatus();
		services.AddMessage("service", &record);
	}
	return B_OK;
}


/*!	Whether a record's ServiceClassIDList (0x0001) names \a serviceClass.
*/
inline bool
SDPHasServiceClass(const BMessage& record, uint16 serviceClass)
{
	const void* data;
	ssize_t size;
	if (record.FindData("0x0001", B_RAW_TYPE, &data, &size) != B_OK)
		return false;

	BMemoryIO io(data, size);
	SDPDataIO classes(&io);
	while (true) {
		sdp_uuid uuid = classes.ReadUUID();
		if (classes.ReadingStatus() != B_OK)
			return false;
		if (uuid.type == SDP_UUID_16 && GET_UUID16(uuid) == serviceClass)
			return true;
	}
}


}	// namespace Bluetooth


#endif	// _SDP_HID_RECORD_H_
