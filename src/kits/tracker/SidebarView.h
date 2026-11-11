/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _TRACKER_SIDEBAR_VIEW_H
#define _TRACKER_SIDEBAR_VIEW_H


#include <Entry.h>
#include <GraphicsDefs.h>
#include <ObjectList.h>
#include <String.h>
#include <View.h>


namespace BPrivate {


class Model;


// The list at the left of a folder window: favorites (the folders in
// Tracker's Go menu), standard places, mounted volumes with an eject
// button, and tags. A click goes there, items dropped on a folder go into
// it, folders dropped on Favorites become favorites.
class SidebarView : public BView {
public:
								SidebarView();
	virtual						~SidebarView();

			void				SetCurrent(const entry_ref* ref);

	virtual	void				AttachedToWindow();
	virtual	void				DetachedFromWindow();
	virtual	void				Draw(BRect updateRect);
	virtual	void				MouseDown(BPoint where);
	virtual	void				MouseMoved(BPoint where, uint32 transit,
									const BMessage* dragMessage);
	virtual	void				MessageReceived(BMessage* message);
	virtual	BSize				MinSize();
	virtual	BSize				PreferredSize();

private:
			enum RowType {
				kHeader,
				kFolder,
				kFavorite,
				kVolume,
				kTag
			};

			struct Row {
				RowType			type;
				BString			label;
				entry_ref		ref;
				Model*			model;
				dev_t			device;
				bool			ejectable;
				rgb_color		color;
				BRect			frame;

								Row() : model(NULL), device(-1),
									ejectable(false) {}
								~Row();
			};

			void				_Rebuild();
			void				_AddHeader(const char* label);
			void				_AddFolder(RowType type, const entry_ref& ref,
									const char* label);
			void				_Layout();
			int32				_RowAt(BPoint where) const;
			BRect				_EjectRect(const Row* row) const;
			void				_Open(const Row* row, bool newWindow);
			void				_ShowMenu(Row* row, BPoint where);
			void				_Drop(BMessage* message, int32 index);
			float				_RowHeight() const;

			BObjectList<Row, true> fRows;
			entry_ref			fCurrent;
			int32				fHover;
			int32				fDropTarget;
			float				fHeight;
};


} // namespace BPrivate

using namespace BPrivate;


#endif	// _TRACKER_SIDEBAR_VIEW_H
