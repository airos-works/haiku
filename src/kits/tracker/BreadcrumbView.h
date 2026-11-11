/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _TRACKER_BREADCRUMB_VIEW_H
#define _TRACKER_BREADCRUMB_VIEW_H


#include <ObjectList.h>
#include <Path.h>
#include <String.h>
#include <View.h>


namespace BPrivate {


const uint32 kBreadcrumbEdit = 'BCed';
	// sent to the target when the empty part is clicked: type a path


// The folder's path as a row of buttons: a click goes to that folder, the
// arrow after one lists the folders in it, items dropped on one go there.
class BreadcrumbView : public BView {
public:
								BreadcrumbView(BHandler* target);

			void				SetPath(const BPath& path);

	virtual	void				Draw(BRect updateRect);
	virtual	void				MouseDown(BPoint where);
	virtual	void				MouseMoved(BPoint where, uint32 transit,
									const BMessage* dragMessage);
	virtual	void				MessageReceived(BMessage* message);
	virtual	BSize				MinSize();
	virtual	BSize				PreferredSize();
	virtual	void				FrameResized(float width, float height);

private:
			struct Segment {
				BString		label;
				BString		path;
				BRect		frame;
				BRect		arrowFrame;
			};

			void				_Layout();
			int32				_SegmentAt(BPoint where, bool* onArrow) const;
			void				_Go(const char* path);
			void				_ShowFolderMenu(int32 index);
			void				_ShowHiddenMenu();

			BHandler*			fTarget;
			BObjectList<Segment, true> fSegments;
			int32				fFirstVisible;
			BRect				fEllipsisFrame;
			int32				fHover;
			bool				fHoverArrow;
			bool				fDropping;
};


} // namespace BPrivate

using namespace BPrivate;


#endif	// _TRACKER_BREADCRUMB_VIEW_H
