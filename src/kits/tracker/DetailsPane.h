/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _TRACKER_DETAILS_PANE_H
#define _TRACKER_DETAILS_PANE_H


#include <Entry.h>
#include <ObjectList.h>
#include <View.h>


class BGroupLayout;


namespace BPrivate {


class BPoseView;
class Model;


// The pane at the right of a folder window: a large picture of what is
// selected (its thumbnail or icon), its name, kind, size, dates and place,
// and the attributes its type shows, editable where they are; for several
// items how many, how big together and of which kinds; with nothing
// selected, the folder itself.
class DetailsPane : public BView {
public:
								DetailsPane();
	virtual						~DetailsPane();

	virtual	void				MessageReceived(BMessage* message);

			// Shows what is selected in poseView, if that changed
			void				Update(BPoseView* poseView, bool force = false);

private:
			void				_Rebuild(BPoseView* poseView);
			void				_AddRow(const char* label, const char* value);
			void				_AddAttributes(Model* model);
			void				_Clear();

			BGroupLayout*		fContent;
			BView*				fContentView;
			BView*				fIconView;
			BObjectList<entry_ref, true> fShown;
			entry_ref			fFolder;
			Model*				fModel;
};


} // namespace BPrivate

using namespace BPrivate;


#endif	// _TRACKER_DETAILS_PANE_H
