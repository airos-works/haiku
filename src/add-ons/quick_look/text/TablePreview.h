/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef TABLE_PREVIEW_H
#define TABLE_PREVIEW_H


#include <vector>

#include <String.h>

#include <add-ons/quick_look/QuickLook.h>


class BColumnListView;


/*!	Comma (or semicolon, or tab) separated values shown as a table, the
	first row as the column titles. */
class TablePreview : public BQuickLookPreview {
public:
								TablePreview(const BString& text,
									bool truncated);
	virtual						~TablePreview();

	virtual	BSize				PreferredContentSize();
	virtual	BString				Summary();
	virtual	void				AttachedToWindow();
	virtual	void				FrameResized(float width, float height);

	static	char				GuessDelimiter(const BString& text);

private:
			void				_Parse(const BString& text, char delimiter);
			void				_Fill();

			std::vector<std::vector<BString> > fRows;
			int32				fColumnCount;
			bool				fTruncated;
			char				fDelimiter;
			BColumnListView*	fList;
			std::vector<float>	fWidths;
};


#endif	// TABLE_PREVIEW_H
