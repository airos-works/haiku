/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef QUICK_LOOK_GENERIC_PREVIEW_H
#define QUICK_LOOK_GENERIC_PREVIEW_H


#include <Entry.h>
#include <String.h>

#include <add-ons/quick_look/QuickLook.h>


class BBitmap;


/*!	What is shown when no add-on previews a file: its icon, name, kind,
	size and date, as the Finder shows for a file it cannot open. */
class GenericPreview : public BQuickLookPreview {
public:
								GenericPreview(const entry_ref& ref,
									const char* mimeType, const char* note);
	virtual						~GenericPreview();

	virtual	BSize				PreferredContentSize();
	virtual	BString				Summary();

	virtual	void				AttachedToWindow();
	virtual	void				Draw(BRect updateRect);

private:
			void				_Describe(const entry_ref& ref,
									const char* mimeType);

			BBitmap*			fIcon;
			BString				fName;
			BString				fKind;
			BString				fDetail;
			BString				fDate;
			BString				fNote;
};


#endif	// QUICK_LOOK_GENERIC_PREVIEW_H
