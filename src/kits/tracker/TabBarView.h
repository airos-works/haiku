/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _TRACKER_TAB_BAR_VIEW_H
#define _TRACKER_TAB_BAR_VIEW_H


#include <ObjectList.h>
#include <String.h>
#include <StringList.h>
#include <View.h>


namespace BPrivate {


const uint32 kSelectTab = 'Tsel';
	// "index": show that tab
const uint32 kCloseTab = 'Tcls';
	// "index": close that tab (the shown one without it)
const uint32 kNewTab = 'Tnew';
	// a tab for the folder shown now, or "refs" for those folders;
	// "background" not to show it
const uint32 kTabDrop = 'Tdrp';
	// "index" and the dropped "refs": move them into that tab's folder


// The row of tabs at the top of a folder window that has more than one:
// a click shows a tab, the close mark or a middle click closes it, the plus
// opens another, items dropped on a tab go into its folder.
class TabBarView : public BView {
public:
								TabBarView();

			void				SetTabs(const BStringList& labels,
									int32 selected);

	virtual	void				Draw(BRect updateRect);
	virtual	void				MouseDown(BPoint where);
	virtual	void				MouseMoved(BPoint where, uint32 transit,
									const BMessage* dragMessage);
	virtual	void				MessageReceived(BMessage* message);
	virtual	void				FrameResized(float width, float height);
	virtual	BSize				MinSize();
	virtual	BSize				MaxSize();
	virtual	BSize				PreferredSize();

private:
			void				_Layout();
			int32				_TabAt(BPoint where) const;
			BRect				_CloseRect(int32 index) const;

			BStringList			fLabels;
			BObjectList<BRect, true> fFrames;
			BRect				fPlusFrame;
			int32				fSelected;
			int32				fHover;
			int32				fDropTarget;
};


} // namespace BPrivate

using namespace BPrivate;


#endif	// _TRACKER_TAB_BAR_VIEW_H
