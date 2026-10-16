/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef PAGES_H
#define PAGES_H


#include <MediaNode.h>
#include <ObjectList.h>
#include <String.h>
#include <View.h>

#include <vector>

#include "Glyphs.h"
#include "MediaModel.h"


class BadgeView;
class BCheckBox;
class BGroupView;
class BListView;
class BMenuField;
class BStringView;
class BTextView;
class Card;
class CameraPreview;
class LevelMeter;
class PageViewport;
class ParameterForm;
class TitleView;


/*!	A page of the window: a header with a badge, a title and a line of
	explanation, then content that scrolls.
*/
class Page : public BView {
public:
								Page(const char* key, glyph_kind glyph,
									rgb_color color, const char* title,
									const char* subtitle,
									const MediaModel* model);

			const BString&		Key() const { return fKey; }

	//! Fills the content; called once the page is in the window.
	virtual	void				Build() = 0;
	//! The devices or the defaults changed.
	virtual	void				ModelChanged();

			void				Rebuild();

			float				ScrollPosition() const;
			void				SetScrollPosition(float position);

	virtual	void				AttachedToWindow();

protected:
			const MediaModel*	Model() const { return fModel; }
			BGroupView*			Content() const { return fContent; }

			void				SetTitle(const char* title);
			void				SetSubtitle(const char* subtitle);

			void				AddTitle(const char* title);
			Card*				AddCard();
			void				AddView(BView* view);
			void				AddLink(const char* label, const char* key);
			void				AddDeviceChoices(Card* card, uint32 role);

private:
			void				_Clear();

			BString				fKey;
			const MediaModel*	fModel;
			BadgeView*			fBadge;
			TitleView*			fTitle;
			TitleView*			fSubtitle;
			BGroupView*			fContent;
			PageViewport*		fViewport;
			bool				fBuilt;
};


class OutputPage : public Page {
public:
								OutputPage(const MediaModel* model);
	virtual						~OutputPage();

	virtual	void				Build();
	virtual	void				AttachedToWindow();
	virtual	void				MessageReceived(BMessage* message);

private:
			BMenuField*			_MakeChannelMenu();

			BCheckBox*			fDeskbarCheck;
};


class InputPage : public Page {
public:
								InputPage(const MediaModel* model);

	virtual	void				Build();
};


class CameraPage : public Page {
public:
								CameraPage(const MediaModel* model);
	virtual						~CameraPage();

	virtual	void				Build();
	virtual	void				MessageReceived(BMessage* message);

private:
			void				_ReleaseCamera();

			media_node			fCamera;
			bool				fHasCamera;
			CameraPreview*		fPreview;
};


class MixerPage : public Page {
public:
								MixerPage(const MediaModel* model);
	virtual						~MixerPage();

	virtual	void				Build();
	virtual	void				ModelChanged();
	virtual	void				AttachedToWindow();
	virtual	void				DetachedFromWindow();
	virtual	void				MessageReceived(BMessage* message);

private:
			media_node			fMixer;
			bool				fHasMixer;
};


class MidiPage : public Page {
public:
								MidiPage(const MediaModel* model);

	virtual	void				Build();
	virtual	void				ModelChanged();
	virtual	void				AttachedToWindow();
	virtual	void				DetachedFromWindow();
	virtual	void				MessageReceived(BMessage* message);

private:
			void				_FindSoundFonts(std::vector<BString>& paths);
			BString				_ActiveSoundFont();
			void				_SetActiveSoundFont(const char* path);
			void				_Watch();
};


class DevicePage : public Page {
public:
								DevicePage(const DeviceEntry& device,
									const MediaModel* model);
	virtual						~DevicePage();

	virtual	void				Build();
	virtual	void				ModelChanged();
	virtual	void				AttachedToWindow();
	virtual	void				DetachedFromWindow();
	virtual	void				MessageReceived(BMessage* message);

private:
	struct Part {
		NodeEntry		entry;
		media_node		node;
		bool			acquired;
		BGroupView*		holder;
		CameraPreview*	preview;
	};

			void				_FillUseCard();
			void				_MakeForm(Part& part);
			void				_ReleaseNodes();

			BString				fName;
			std::vector<Part>	fParts;
			Card*				fUseCard;
			BGroupView*			fUseHolder;
			bool				fWatching;
};


//! A page that only says something, like "Media services are not running".
class MessagePage : public Page {
public:
								MessagePage(const char* key, glyph_kind glyph,
									const char* title, const char* text,
									const char* buttonLabel,
									BMessage* buttonMessage,
									const MediaModel* model);
	virtual						~MessagePage();

	virtual	void				Build();
			void				SetText(const char* text);

private:
			BString				fText;
			BString				fButtonLabel;
			BMessage*			fButtonMessage;
			BTextView*			fTextView;
};


#endif	// PAGES_H
