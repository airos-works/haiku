/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include <neural/Tensor.h>

#include <math.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Message.h>

#include <NeuralGraphData.h>


using BPrivate::NeuralGraphData;


// Memory of tensors is aligned for the widest vectors a kernel may use and
// padded, so that kernels may read (not write) a little past the end.
static const size_t kTensorAlignment = 64;
static const size_t kTensorPadding = 64;


// #pragma mark - BTensorInfo


BTensorInfo::BTensorInfo()
{
	Unset();
}


BTensorInfo::BTensorInfo(neural_data_type type, int32 rank,
	const int32* dimensions, const char* name)
{
	SetTo(type, rank, dimensions, name);
}


BTensorInfo::BTensorInfo(const BTensorInfo& other)
{
	*this = other;
}


BTensorInfo::~BTensorInfo()
{
}


BTensorInfo&
BTensorInfo::operator=(const BTensorInfo& other)
{
	if (this == &other)
		return *this;

	fName = other.fName;
	fType = other.fType;
	fRank = other.fRank;
	memcpy(fDimensions, other.fDimensions, sizeof(fDimensions));
	fScale = other.fScale;
	fZeroPoint = other.fZeroPoint;
	return *this;
}


bool
BTensorInfo::operator==(const BTensorInfo& other) const
{
	if (fType != other.fType || fRank != other.fRank
		|| fScale != other.fScale || fZeroPoint != other.fZeroPoint) {
		return false;
	}

	for (int32 i = 0; i < fRank; i++) {
		if (fDimensions[i] != other.fDimensions[i])
			return false;
	}
	return true;
}


bool
BTensorInfo::operator!=(const BTensorInfo& other) const
{
	return !(*this == other);
}


status_t
BTensorInfo::SetTo(neural_data_type type, int32 rank,
	const int32* dimensions, const char* name)
{
	Unset();

	if (neural_data_type_size(type) == 0 || rank < 0
		|| rank > B_NEURAL_MAX_RANK || (rank > 0 && dimensions == NULL)) {
		return B_BAD_VALUE;
	}

	int64 elements = 1;
	for (int32 i = 0; i < rank; i++) {
		if (dimensions[i] <= 0)
			return B_BAD_VALUE;
		elements *= dimensions[i];
		if (elements > ((int64)1 << 40))
			return B_BAD_VALUE;
	}

	fType = type;
	fRank = rank;
	for (int32 i = 0; i < rank; i++)
		fDimensions[i] = dimensions[i];
	fName = name;
	return B_OK;
}


void
BTensorInfo::Unset()
{
	fName = "";
	fType = B_NEURAL_NO_TYPE;
	fRank = 0;
	memset(fDimensions, 0, sizeof(fDimensions));
	fScale = 0;
	fZeroPoint = 0;
	memset(_reserved, 0, sizeof(_reserved));
}


bool
BTensorInfo::IsValid() const
{
	return fType != B_NEURAL_NO_TYPE;
}


const char*
BTensorInfo::Name() const
{
	return fName.String();
}


void
BTensorInfo::SetName(const char* name)
{
	fName = name;
}


neural_data_type
BTensorInfo::Type() const
{
	return fType;
}


int32
BTensorInfo::Rank() const
{
	return fRank;
}


int32
BTensorInfo::DimensionAt(int32 index) const
{
	if (index < 0 || index >= fRank)
		return 0;
	return fDimensions[index];
}


int64
BTensorInfo::CountElements() const
{
	if (!IsValid())
		return 0;

	int64 elements = 1;
	for (int32 i = 0; i < fRank; i++)
		elements *= fDimensions[i];
	return elements;
}


size_t
BTensorInfo::ByteSize() const
{
	return (size_t)CountElements() * neural_data_type_size(fType);
}


bool
BTensorInfo::IsQuantized() const
{
	return fScale != 0;
}


float
BTensorInfo::Scale() const
{
	return fScale;
}


int32
BTensorInfo::ZeroPoint() const
{
	return fZeroPoint;
}


void
BTensorInfo::SetQuantization(float scale, int32 zeroPoint)
{
	fScale = scale;
	fZeroPoint = scale != 0 ? zeroPoint : 0;
}


BString
BTensorInfo::ToString() const
{
	BString string;
	if (!IsValid())
		return string;

	if (fName.Length() > 0)
		string << fName << " ";
	string << neural_data_type_name(fType) << " [";
	for (int32 i = 0; i < fRank; i++) {
		if (i > 0)
			string << ", ";
		string << fDimensions[i];
	}
	string << "]";

	if (IsQuantized()) {
		char buffer[64];
		snprintf(buffer, sizeof(buffer), " scale %g zero %" B_PRId32,
			fScale, fZeroPoint);
		string << buffer;
	}
	return string;
}


status_t
BTensorInfo::Archive(BMessage* into) const
{
	if (into == NULL)
		return B_BAD_VALUE;

	status_t status = into->AddString("neural:name", fName);
	if (status == B_OK)
		status = into->AddInt32("neural:type", fType);
	for (int32 i = 0; status == B_OK && i < fRank; i++)
		status = into->AddInt32("neural:dimension", fDimensions[i]);
	if (status == B_OK && IsQuantized()) {
		status = into->AddFloat("neural:scale", fScale);
		if (status == B_OK)
			status = into->AddInt32("neural:zero point", fZeroPoint);
	}
	return status;
}


status_t
BTensorInfo::Unarchive(const BMessage* from)
{
	Unset();
	if (from == NULL)
		return B_BAD_VALUE;

	int32 type;
	status_t status = from->FindInt32("neural:type", &type);
	if (status != B_OK)
		return status;

	int32 dimensions[B_NEURAL_MAX_RANK];
	int32 rank = 0;
	while (rank < B_NEURAL_MAX_RANK && from->FindInt32("neural:dimension",
			rank, &dimensions[rank]) == B_OK) {
		rank++;
	}
	if (from->HasInt32("neural:dimension", rank))
		return B_BAD_VALUE;

	status = SetTo((neural_data_type)type, rank, dimensions,
		from->GetString("neural:name", ""));
	if (status != B_OK)
		return status;

	float scale;
	if (from->FindFloat("neural:scale", &scale) == B_OK)
		SetQuantization(scale, from->GetInt32("neural:zero point", 0));
	return B_OK;
}


// #pragma mark - BTensor


BTensor::BTensor()
	:
	fData(NULL),
	fSize(0),
	fOwnsData(false),
	fStatus(B_NO_INIT)
{
	memset(_reserved, 0, sizeof(_reserved));
}


BTensor::BTensor(const BTensorInfo& info)
	:
	fData(NULL),
	fSize(0),
	fOwnsData(false),
	fStatus(B_NO_INIT)
{
	memset(_reserved, 0, sizeof(_reserved));
	SetTo(info);
}


BTensor::BTensor(const BTensorInfo& info, void* data, size_t size)
	:
	fData(NULL),
	fSize(0),
	fOwnsData(false),
	fStatus(B_NO_INIT)
{
	memset(_reserved, 0, sizeof(_reserved));
	SetTo(info, data, size);
}


BTensor::BTensor(const BTensor& other)
	:
	BArchivable(),
	fData(NULL),
	fSize(0),
	fOwnsData(false),
	fStatus(B_NO_INIT)
{
	memset(_reserved, 0, sizeof(_reserved));
	*this = other;
}


BTensor::BTensor(BMessage* archive)
	:
	BArchivable(archive),
	fData(NULL),
	fSize(0),
	fOwnsData(false),
	fStatus(B_NO_INIT)
{
	memset(_reserved, 0, sizeof(_reserved));

	BTensorInfo info;
	fStatus = info.Unarchive(archive);
	if (fStatus != B_OK)
		return;

	fStatus = SetTo(info);
	if (fStatus != B_OK)
		return;

	const void* data;
	ssize_t size;
	if (archive->FindData("neural:data", B_RAW_TYPE, &data, &size) == B_OK) {
		if ((size_t)size != fSize) {
			Unset();
			fStatus = B_BAD_DATA;
			return;
		}
		memcpy(fData, data, fSize);
	}
}


BTensor::~BTensor()
{
	Unset();
}


BTensor&
BTensor::operator=(const BTensor& other)
{
	if (this == &other)
		return *this;

	Unset();
	if (other.fStatus != B_OK) {
		fStatus = other.fStatus;
		return *this;
	}

	if (SetTo(other.fInfo) == B_OK)
		memcpy(fData, other.fData, fSize);
	return *this;
}


status_t
BTensor::Archive(BMessage* into, bool deep) const
{
	if (fStatus != B_OK)
		return fStatus;

	status_t status = BArchivable::Archive(into, deep);
	if (status == B_OK)
		status = fInfo.Archive(into);
	if (status == B_OK && deep)
		status = into->AddData("neural:data", B_RAW_TYPE, fData, fSize);
	return status;
}


/*static*/ BArchivable*
BTensor::Instantiate(BMessage* archive)
{
	if (!validate_instantiation(archive, "BTensor"))
		return NULL;
	return new(std::nothrow) BTensor(archive);
}


status_t
BTensor::InitCheck() const
{
	return fStatus;
}


status_t
BTensor::SetTo(const BTensorInfo& info)
{
	Unset();
	if (!info.IsValid())
		return fStatus = B_BAD_VALUE;

	size_t size = info.ByteSize();
	void* data;
	if (posix_memalign(&data, kTensorAlignment, size + kTensorPadding) != 0)
		return fStatus = B_NO_MEMORY;
	memset(data, 0, size + kTensorPadding);

	fInfo = info;
	fData = data;
	fSize = size;
	fOwnsData = true;
	return fStatus = B_OK;
}


status_t
BTensor::SetTo(const BTensorInfo& info, void* data, size_t size)
{
	Unset();
	if (!info.IsValid() || data == NULL || size < info.ByteSize())
		return fStatus = B_BAD_VALUE;

	fInfo = info;
	fData = data;
	fSize = info.ByteSize();
	fOwnsData = false;
	return fStatus = B_OK;
}


void
BTensor::Unset()
{
	if (fOwnsData)
		free(fData);
	fData = NULL;
	fSize = 0;
	fOwnsData = false;
	fInfo.Unset();
	fStatus = B_NO_INIT;
}


const BTensorInfo&
BTensor::Info() const
{
	return fInfo;
}


neural_data_type
BTensor::Type() const
{
	return fInfo.Type();
}


int32
BTensor::Rank() const
{
	return fInfo.Rank();
}


int32
BTensor::DimensionAt(int32 index) const
{
	return fInfo.DimensionAt(index);
}


int64
BTensor::CountElements() const
{
	return fInfo.CountElements();
}


void*
BTensor::Data()
{
	return fData;
}


const void*
BTensor::Data() const
{
	return fData;
}


size_t
BTensor::ByteSize() const
{
	return fSize;
}


status_t
BTensor::CopyFrom(const void* data, size_t size)
{
	if (fStatus != B_OK)
		return fStatus;
	if (data == NULL || size != fSize)
		return B_BAD_VALUE;

	memcpy(fData, data, fSize);
	return B_OK;
}


status_t
BTensor::CopyTo(void* data, size_t size) const
{
	if (fStatus != B_OK)
		return fStatus;
	if (data == NULL || size != fSize)
		return B_BAD_VALUE;

	memcpy(data, fData, fSize);
	return B_OK;
}


status_t
BTensor::CopyFrom(const BTensor& other)
{
	if (fStatus != B_OK)
		return fStatus;
	if (other.fStatus != B_OK)
		return B_BAD_VALUE;
	if (other.fInfo.Type() != fInfo.Type())
		return B_NEURAL_TYPE_MISMATCH;
	if (other.fInfo != fInfo)
		return B_NEURAL_SHAPE_MISMATCH;

	memcpy(fData, other.fData, fSize);
	return B_OK;
}


static inline float
load_value(const void* data, neural_data_type type, int64 index,
	float scale, int32 zeroPoint)
{
	float stored;
	switch (type) {
		case B_NEURAL_FLOAT32:
			return ((const float*)data)[index];
		case B_NEURAL_INT8:
			stored = ((const int8*)data)[index];
			break;
		case B_NEURAL_UINT8:
			stored = ((const uint8*)data)[index];
			break;
		case B_NEURAL_INT16:
			stored = ((const int16*)data)[index];
			break;
		case B_NEURAL_INT32:
			stored = ((const int32*)data)[index];
			break;
		case B_NEURAL_INT64:
			stored = ((const int64*)data)[index];
			break;
		case B_NEURAL_BOOL:
			return ((const uint8*)data)[index] != 0 ? 1 : 0;
		default:
			return 0;
	}

	if (scale != 0)
		return scale * (stored - zeroPoint);
	return stored;
}


static inline void
store_value(void* data, neural_data_type type, int64 index, float scale,
	int32 zeroPoint, float value)
{
	if (type == B_NEURAL_FLOAT32) {
		((float*)data)[index] = value;
		return;
	}
	if (type == B_NEURAL_BOOL) {
		((uint8*)data)[index] = value != 0 ? 1 : 0;
		return;
	}

	double stored = value;
	if (scale != 0)
		stored = round(stored / scale) + zeroPoint;
	else
		stored = round(stored);
	if (stored != stored)
		stored = 0;

	switch (type) {
		case B_NEURAL_INT8:
			((int8*)data)[index] = (int8)fmax(-128, fmin(127, stored));
			break;
		case B_NEURAL_UINT8:
			((uint8*)data)[index] = (uint8)fmax(0, fmin(255, stored));
			break;
		case B_NEURAL_INT16:
			((int16*)data)[index] = (int16)fmax(-32768, fmin(32767, stored));
			break;
		case B_NEURAL_INT32:
			((int32*)data)[index] = (int32)fmax(-2147483648.0,
				fmin(2147483647.0, stored));
			break;
		case B_NEURAL_INT64:
			((int64*)data)[index] = (int64)fmax(-9.2e18, fmin(9.2e18, stored));
			break;
		default:
			break;
	}
}


float
BTensor::ValueAt(int64 index) const
{
	if (fStatus != B_OK || index < 0 || index >= fInfo.CountElements())
		return 0;
	return load_value(fData, fInfo.Type(), index, fInfo.Scale(),
		fInfo.ZeroPoint());
}


status_t
BTensor::SetValueAt(int64 index, float value)
{
	if (fStatus != B_OK)
		return fStatus;
	if (index < 0 || index >= fInfo.CountElements())
		return B_BAD_INDEX;
	if (fInfo.Type() == B_NEURAL_FLOAT16)
		return B_NOT_SUPPORTED;

	store_value(fData, fInfo.Type(), index, fInfo.Scale(),
		fInfo.ZeroPoint(), value);
	return B_OK;
}


status_t
BTensor::GetValues(float* values, int64 count, int64 first) const
{
	if (fStatus != B_OK)
		return fStatus;
	if (values == NULL || count < 0 || first < 0
		|| first + count > fInfo.CountElements()) {
		return B_BAD_VALUE;
	}
	if (fInfo.Type() == B_NEURAL_FLOAT16)
		return B_NOT_SUPPORTED;

	neural_data_type type = fInfo.Type();
	float scale = fInfo.Scale();
	int32 zeroPoint = fInfo.ZeroPoint();
	for (int64 i = 0; i < count; i++)
		values[i] = load_value(fData, type, first + i, scale, zeroPoint);
	return B_OK;
}


status_t
BTensor::SetValues(const float* values, int64 count, int64 first)
{
	if (fStatus != B_OK)
		return fStatus;
	if (values == NULL || count < 0 || first < 0
		|| first + count > fInfo.CountElements()) {
		return B_BAD_VALUE;
	}
	if (fInfo.Type() == B_NEURAL_FLOAT16)
		return B_NOT_SUPPORTED;

	neural_data_type type = fInfo.Type();
	float scale = fInfo.Scale();
	int32 zeroPoint = fInfo.ZeroPoint();
	for (int64 i = 0; i < count; i++)
		store_value(fData, type, first + i, scale, zeroPoint, values[i]);
	return B_OK;
}


int64
BTensor::IndexOfMaximum() const
{
	if (fStatus != B_OK || fInfo.Type() == B_NEURAL_FLOAT16)
		return -1;

	// The order of stored values is that of the real ones (scales are
	// positive), so the conversion can be left out.
	neural_data_type type = fInfo.Type();
	int64 count = fInfo.CountElements();
	int64 best = -1;
	float bestValue = 0;
	for (int64 i = 0; i < count; i++) {
		float value = load_value(fData, type, i, 0, 0);
		if (best < 0 || value > bestValue) {
			best = i;
			bestValue = value;
		}
	}
	return best;
}


void BTensor::_ReservedTensor1() {}
void BTensor::_ReservedTensor2() {}
void BTensor::_ReservedTensor3() {}
void BTensor::_ReservedTensor4() {}
