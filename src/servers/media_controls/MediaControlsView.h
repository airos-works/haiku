/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef MEDIA_CONTROLS_VIEW_H
#define MEDIA_CONTROLS_VIEW_H


#include <Message.h>
#include <Messenger.h>
#include <View.h>


/*!	The Deskbar replicant: the icon in the tray. It lives in the Deskbar
	team; the server adds it while there is a player and removes it after.
*/
class MediaControlsView : public BView {
public:
								MediaControlsView(BRect frame);
								MediaControlsView(BMessage* archive);
	virtual						~MediaControlsView();

	static	MediaControlsView*	Instantiate(BMessage* archive);
	virtual	status_t			Archive(BMessage* archive,
									bool deep = true) const;

	virtual	void				AttachedToWindow();
	virtual	void				DetachedFromWindow();
	virtual	void				Draw(BRect updateRect);
	virtual	void				MouseDown(BPoint where);
	virtual	void				MessageReceived(BMessage* message);

private:
			void				_UpdateToolTip();
			void				_ShowMenu(BPoint where);
			void				_TogglePopup();

private:
			BMessage			fState;
			BMessenger			fPopup;
};


#endif	// MEDIA_CONTROLS_VIEW_H
