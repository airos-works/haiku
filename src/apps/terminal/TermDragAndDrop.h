/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef TERM_DRAG_AND_DROP_H
#define TERM_DRAG_AND_DROP_H


#include <functional>
#include <map>
#include <string>
#include <vector>

#include <Message.h>
#include <Point.h>
#include <SupportDefs.h>

#include "Base64.h"


class BBitmap;
class BView;


/*!	kitty's drag and drop protocol (OSC 72) for one terminal. A program can
	take drops: it hears about drags over it, then asks for the dropped data
	by type, and a program on another machine (over ssh) can read dropped
	files and directories through the terminal. A program can also start
	drags of its own, with data and images it sends first. Everything runs
	on the window thread; answers go to the program through the writer.
*/
class TermDragAndDrop {
public:
	typedef std::function<void(const std::string&)> Writer;

								TermDragAndDrop(BView* view, Writer writer);
								~TermDragAndDrop();

			//! An OSC 72 command from the program (what follows "72;").
			void				Command(const std::string& data);

			// drops onto the terminal; positions in cells and in pixels
			bool				IsAcceptingDrops() const
									{ return fAccepting; }
			void				DragMoved(int32 column, int32 row,
									int32 pixelX, int32 pixelY,
									const BMessage* message);
			void				DragExited();
			//! Returns whether the program took the drop.
			bool				Dropped(const BMessage* message, int32 column,
									int32 row, int32 pixelX, int32 pixelY);

			// drags the program starts: the gesture is the program's while
			// it offers drags
			bool				IsOfferingDrags() const
									{ return fOffering; }
			void				MouseDown(BPoint where, int32 column,
									int32 row, int32 pixelX, int32 pixelY);
			bool				MouseMoved(BPoint where);
			void				MouseUp();

	//! This installation's machine id, as kitty's protocol hashes it.
	static	const std::string&	MachineID();

private:
			typedef std::map<char, std::string> Keys;

			struct Directory {
				std::string		path;
				std::vector<std::string> entries;
			};

			void				_Packet(const Keys& keys,
									const std::string& payload);
			void				_DropRequest(const Keys& keys);
			void				_PreSent(const Keys& keys,
									const std::string& payload, bool end);
			void				_StartDrag();
			BBitmap*			_MakeImage(int32 index);

			void				_Send(const std::string& keys,
									const std::string& payload = "");
			void				_SendData(const std::string& keys,
									const std::string& data);
			void				_SendError(const std::string& keys,
									const char* error);
			void				_SendEntry(const std::string& keys,
									const std::string& path);
			bool				_DropData(const std::string& type,
									std::string& data) const;
			void				_EndDrop();
			void				_EndOffer();
			bool				_IsRemote() const;

private:
			BView*				fView;
			Writer				fWriter;

			// a chain of chunks (m=1)
			bool				fChaining;
			Keys				fChainKeys;
			std::string			fChainText;
			int32				fClientID;		// i, echoed back

			// taking drops
			bool				fAccepting;
			std::string			fClientMachine;
			bool				fDragInside;
			int32				fDragColumn;
			int32				fDragRow;
			std::vector<std::string> fDragTypes;
			int32				fClientOperation;
			bool				fDropped;
			BMessage			fDropMessage;
			std::vector<std::string> fDropTypes;
			std::vector<std::string> fDropPaths;
			bool				fDropFromHere;
			int32				fUriListIndex;
			std::map<int32, Directory> fDirectories;
			int32				fNextHandle;

			// starting drags
			bool				fOffering;
			bool				fMouseDown;
			BPoint				fGesturePoint;
			int32				fGestureColumn;
			int32				fGestureRow;
			int32				fGestureX;
			int32				fGestureY;
			bool				fGestureSent;
			bool				fOfferActive;
			int32				fOfferOperations;
			std::vector<std::string> fOfferTypes;
			std::map<int32, std::string> fPreSent;
			std::map<int32, Base64Decoder> fDecoders;	// by x of t=p
			size_t				fPreSentSize;
			struct Image {
				int32			format;
				int32			width;
				int32			height;
				int32			opacity;
				std::string		data;
				bool			complete;
			};
			std::map<int32, Image> fImages;
			int32				fImageIndex;
			bool				fDragStarted;
};


#endif	// TERM_DRAG_AND_DROP_H
