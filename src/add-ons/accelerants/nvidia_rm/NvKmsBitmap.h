#pragma once

#include <NvRmDevice.h>
#include <NvKmsDevice.h>
#include <NvKmsSurface.h>

#include <GraphicsDefs.h>


class NvKmsBitmap {
private:
	NvRmDevice *fRmDev = nullptr;
	NvRmObject fMemory;
	NvKmsSurface fSurface;
	NvRmMemoryMapping fMapping;

	uint32 fWidth;
	uint32 fHeight;
	uint32 fBytesPerRow;
	color_space fColorSpace;
	bool fSystemMemory = false;

public:
	inline NvKmsBitmap();
	inline NvKmsBitmap(NvKmsBitmap &&other);
	// systemMemory puts the surface in ordinary memory rather than video
	// memory. Do not ask for it for anything the display scans out: this card
	// refuses, and the mode set fails - see STATUS. It is here for surfaces
	// the display never reads.
	// Without mapped, Bits() is NULL until Map(). Video memory is mapped for
	// the CPU through the card's BAR1, which is only 256 MiB on a GTX 1070:
	// a surface that is not drawn into yet need not take any of it.
	NvKmsBitmap(NvRmDevice &rmDev, NvKmsDevice &kmsDev, uint32 width, uint32 height,
		color_space colorSpace, bool systemMemory = false, bool mapped = true);

	inline NvKmsBitmap &operator=(NvKmsBitmap &&other);

	inline NvKmsBitmap(const NvKmsBitmap &other) = delete;
	inline NvKmsBitmap &operator=(const NvKmsBitmap &other) = delete;

	inline bool IsSet() const;
	void Map();
	void Unmap();

	inline uint32 Width() const;
	inline uint32 Height() const;
	inline void *Bits() const;
	inline int32 BitsLength() const;
	inline int32 BytesPerRow() const;
	inline color_space ColorSpace() const;
	inline bool IsSystemMemory() const {return fSystemMemory;}

	inline const NvRmObject &Memory() const;
	inline const NvKmsSurface &Surface() const;
};


NvKmsBitmap::NvKmsBitmap():
	fWidth(0),
	fHeight(0),
	fBytesPerRow(0),
	fColorSpace(B_NO_COLOR_SPACE)
{
}

NvKmsBitmap::NvKmsBitmap(NvKmsBitmap &&other):
	fRmDev(other.fRmDev),
	fWidth(other.fWidth),
	fHeight(other.fHeight),
	fBytesPerRow(other.fBytesPerRow),
	fColorSpace(other.fColorSpace)
{
	fMapping = std::move(other.fMapping);
	fSurface = std::move(other.fSurface);
	fMemory = std::move(other.fMemory);
	fSystemMemory = other.fSystemMemory;

	other.fWidth = 0;
	other.fHeight = 0;
	other.fBytesPerRow = 0;
	other.fColorSpace = B_NO_COLOR_SPACE;
}

NvKmsBitmap &NvKmsBitmap::operator=(NvKmsBitmap &&other)
{
	fRmDev = other.fRmDev;
	fMapping = std::move(other.fMapping);
	fSurface = std::move(other.fSurface);
	fMemory = std::move(other.fMemory);

	fWidth = other.fWidth;
	fHeight = other.fHeight;
	fBytesPerRow = other.fBytesPerRow;
	fColorSpace = other.fColorSpace;
	fSystemMemory = other.fSystemMemory;

	other.fWidth = 0;
	other.fHeight = 0;
	other.fBytesPerRow = 0;
	other.fColorSpace = B_NO_COLOR_SPACE;

	return *this;
}

bool NvKmsBitmap::IsSet() const
{
	return fSurface.IsSet();
}


uint32 NvKmsBitmap::Width() const
{
	return fWidth;
}

uint32 NvKmsBitmap::Height() const
{
	return fHeight;
}

void *NvKmsBitmap::Bits() const
{
	return fMapping.Address();
}

int32 NvKmsBitmap::BitsLength() const
{
	return fBytesPerRow*fHeight;
}

int32 NvKmsBitmap::BytesPerRow() const
{
	return fBytesPerRow;
}

color_space NvKmsBitmap::ColorSpace() const
{
	return fColorSpace;
}

const NvRmObject &NvKmsBitmap::Memory() const
{
	return fMemory;
}

const NvKmsSurface &NvKmsBitmap::Surface() const
{
	return fSurface;
}
