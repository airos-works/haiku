/*
 * Copyright 2010-2017, Haiku, Inc. All Rights Reserved.
 * Copyright 2008-2009, Pier Luigi Fiorini. All Rights Reserved.
 * Copyright 2004-2008, Michael Davidson. All Rights Reserved.
 * Copyright 2004-2007, Mikael Eiman. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _NOTIFICATION_VIEW_H
#define _NOTIFICATION_VIEW_H

#include <list>

#include <Message.h>
#include <Messenger.h>
#include <String.h>
#include <View.h>


class AppGroupView;
class BBitmap;
class BMessageRunner;
class BNotification;

const uint32 kRemoveView = 'ReVi';
const uint32 kNotificationButton = 'NtBt';


class NotificationView : public BView {
public:
								NotificationView(BNotification* notification, bigtime_t timeout,
									bool disableTimeout = false,
									const BMessage* request = NULL);
	virtual						~NotificationView();

	virtual	void 				AttachedToWindow();
	virtual	void 				MessageReceived(BMessage* message);
	virtual	void				Draw(BRect updateRect);
	virtual	void				MouseDown(BPoint point);

/*
	virtual	BSize				MinSize();
	virtual	BSize				MaxSize();
	virtual	BSize 				PreferredSize();
*/

	virtual	BHandler*			ResolveSpecifier(BMessage* msg, int32 index,
									BMessage* specifier, int32 form,
									const char* property);
	virtual	status_t			GetSupportedSuites(BMessage* msg);

			void 				SetText(float newMaxWidth = -1);
			void				SetPreviewModeOn(bool enabled);

			const char*			MessageID() const;
			int					ProgressPercent();

			//! Replaced by a notification with the same ID: not closed.
			void				SetReplaced()	{ fReplaced = true; }

private:
			void				_CalculateSize();
			bool				_Reply(const BMessage& message,
									int32 button = -1);
			void				_DrawCloseButton(const BRect& updateRect);

			struct LineInfo {
				BFont	font;
				BString	text;
				BPoint	location;
			};

			typedef std::list<LineInfo*> LineInfoList;

			BNotification*		fNotification;
			bigtime_t			fTimeout;
			int32				fIconSize;
			bool				fDisableTimeout;

			AppGroupView*		fGroupView;
			BMessageRunner*		fRunner;

			BBitmap*			fBitmap;
			LineInfoList		fLines;
			float				fHeight;
			rgb_color			fStripeColor;
			bool				fCloseClicked;
			bool				fPreviewModeOn;

			// what the sender asked to hear (see the request fields in
			// NotificationView.cpp)
			BMessenger			fReplyTarget;
			BMessage			fActivatedMessage;
			BMessage			fClosedMessage;
			BView*				fButtons;
			bool				fReplaced;
};

#endif	// _NOTIFICATION_VIEW_H
