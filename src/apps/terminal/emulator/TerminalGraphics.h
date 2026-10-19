/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef TERMINAL_GRAPHICS_H
#define TERMINAL_GRAPHICS_H


#include <map>
#include <memory>
#include <string>
#include <vector>

#include <OS.h>

#include "Base64.h"
#include "TerminalCell.h"


class Terminal;


//! Pixels of one frame of an image: straight RGBA, 4 bytes per pixel.
struct ImageFrame {
	std::vector<uint8>	pixels;
	int32				gap;		// ms before the next frame, < 0 gapless
	uint64				serial;		// unique, changes with the pixels
};


//! An image to draw: a rectangle of a frame, into a rectangle of pixels.
struct ImageDraw {
	uint32				imageId;
	std::shared_ptr<const ImageFrame> frame;
	int32				imageWidth;
	int32				imageHeight;
	float				sourceX;
	float				sourceY;
	float				sourceWidth;
	float				sourceHeight;
	int32				x;			// relative to the top left of the first
	int32				y;			// row drawn, in pixels
	int32				width;
	int32				height;
	int32				z;
};


// z-index bands
static const int32 kBelowBackgroundsZ = -1073741824;


/*!	kitty's terminal graphics protocol: the images a program sent, and where
	it put them. Each screen has its own images. Placements are anchored to
	lines, so they scroll with the text into the history.
*/
class TerminalGraphics {
public:
	typedef bool (*DecodeFunction)(const uint8* data, size_t length,
		int32& width, int32& height, std::vector<uint8>& rgba);
	typedef bool (*InflateFunction)(const uint8* data, size_t length,
		size_t expectedLength, std::vector<uint8>& output);

public:
								TerminalGraphics(Terminal* terminal);
								~TerminalGraphics();

	static	void				SetCodecs(DecodeFunction decodePNG,
									InflateFunction inflate);

			void				HandleCommand(const char* data,
									size_t length);

			// hooks from the terminal
	inline	void				CellsChanged(int32 row, int32 from,
									int32 to) {}
			void				Scrolled(int32 count, bool toHistory,
									int32 bottom = -1);
			void				RegionScrolled(int32 top, int32 bottom,
									int32 left, int32 right, int32 count);
	inline	void				RowsCleared(int32 first, int32 last) {}
			void				ScreenCleared();
			void				HistoryCleared();
			void				HistoryTrimmed();
			void				ScreenSwitched(bool alternate);
			void				Reset();
			void				Resized(int32 oldColumns, int32 oldRows);
			void				Reflowed(const std::vector<int64>& lineMap,
									int64 oldBase, int64 newBase);

			// for drawing; the terminal must be locked
			bool				HasPlacements() const;
			void				CollectDraws(int32 firstRow, int32 rowCount,
									std::vector<ImageDraw>& draws);
			//! Draws for the image placeholder cells (U+10EEEE) of a row.
			void				CollectPlaceholderDraws(
									const TerminalCell* cells,
									int32 columns, int32 viewRow,
									std::vector<ImageDraw>& draws);

			// animation
			bool				HasAnimations() const;
			//! Advances animations to \a now; returns when to call again
			//! (B_INFINITE_TIMEOUT for never), and whether something changed.
			bigtime_t			Animate(bigtime_t now, bool& _changed);

			uint64				Generation() const	{ return fGeneration; }

private:
			struct Image;
			struct Placement;
			struct Store;
			struct Command;

			Store&				_Store();
			Image*				_FindImage(uint32 id);
			Image*				_FindImageByNumber(uint32 number);
			void				_Reply(const Command& command,
									const char* message);
			bool				_ParseCommand(const char* data,
									size_t length, Command& command,
									std::string& payload);

			void				_Transmit(Command& command,
									const std::string& data);
			bool				_LoadData(Command& command,
									const std::string& payload,
									std::vector<uint8>& pixels,
									int32& width, int32& height,
									std::string& error);
			bool				_ReadMedium(const Command& command,
									const std::string& payload,
									std::vector<uint8>& data);
			void				_Put(Command& command, Image* image);
			void				_Delete(Command& command);
			void				_Frame(Command& command,
									const std::string& data);
			void				_AnimationControl(Command& command);
			void				_Compose(Command& command);

			void				_DeleteImage(uint32 id);
			void				_DeletePlacement(Placement* placement,
									bool freeData);
			void				_DeleteChildren(uint32 imageId,
									uint32 placementId, bool freeData);
			void				_RemoveUnusedImages();
			void				_EnforceQuota(size_t incoming);
			void				_ComputeExtent(Placement& placement,
									const Image& image);
			bool				_PlacementPosition(const Placement& placement,
									int64& line, int32& column,
									int32 depth = 0) const;
			int32				_PlacementRow(const Placement& placement)
									const;
			void				_Changed();

private:
			Terminal*			fTerminal;
			Store*				fMain;
			Store*				fAlternate;
			bool				fAlternateActive;
			uint64				fGeneration;

			// a chunked transmission in progress
			bool				fLoading;
			std::string			fLoadData;
			std::string			fLoadHeader;
			Base64Decoder		fLoadDecoder;
};


#endif	// TERMINAL_GRAPHICS_H
