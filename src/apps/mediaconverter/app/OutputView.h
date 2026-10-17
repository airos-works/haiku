/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef OUTPUT_VIEW_H
#define OUTPUT_VIEW_H


#include <View.h>

#include "OutputSettings.h"
#include "SourceInfo.h"


class BButton;
class BCheckBox;
class BMenuField;
class BRadioButton;
class BSlider;
class BStringView;
class BTabView;
class BTextControl;
class EncoderBadge;


enum badge_kind {
	kBadgeNeutral = 0,
	kBadgeHardware,
	kBadgeWarning,
	kBadgeError,
};


/*!	Who will encode the video: the GPU, or - with the warning the user
	asked for - the CPU. */
struct EncoderDecision {
	badge_kind		kind;
	BString			text;
	BString			detail;			// for the tool tip
	bool			cpuFallback;	// a GPU was wanted and cannot
	bool			impossible;		// nothing here can encode it
};


EncoderDecision decide_encoder(const OutputSettings& settings,
	const SourceInfo* info);


/*!	What to make. Films and songs are set up apart - a song has no
	pictures to encode, and its presets are MP3, AAC and FLAC at a few
	rates - and each keeps its own settings: the view shows the ones for
	the kind of source selected. */
class OutputView : public BView {
public:
								OutputView();

	virtual	void				AttachedToWindow();
	virtual	void				MessageReceived(BMessage* message);

			// the settings of the kind the view shows now
			void				SetSettings(const OutputSettings& settings,
									const char* presetName);
			OutputSettings		Settings() const;
			BString				PresetName() const { return fPresetName; }

			// the settings for songs (sound) or for films
			void				SetSettingsFor(bool sound,
									const OutputSettings& settings,
									const char* presetName);
			OutputSettings		SettingsFor(bool sound) const;
			BString				PresetNameFor(bool sound) const;
			bool				SoundMode() const { return fSoundMode; }

			void				SetSource(const SourceInfo* info);
			void				SetEnabled(bool enabled);

			void				SetDestination(const char* folder,
									bool nextToSource);
			BString				DestinationFolder() const
									{ return fFolder; }
			bool				NextToSource() const;

			void				UpdateHardwareStatus();

private:
			void				_SetSoundMode(bool sound);
			void				_ReadSoundControls();
			void				_UpdateSoundSummary();
			void				_BuildPresetMenu();
			void				_BuildVideoMenu(container_kind container);
			void				_BuildAudioMenu(container_kind container);
			void				_BuildEncoderMenu(video_codec codec);
			void				_UpdateEnabling();
			void				_UpdateQualityLabel();
			void				_Changed();

			BMenuField*			fPreset;
			BTabView*			fTabs;
			BTabView*			fSoundTabs;

			// a song's Summary and Range
			BMenuField*			fSoundFormat;
			BMenuField*			fSoundCodec;
			BMenuField*			fSoundBitrate;
			BMenuField*			fSoundChannels;
			BMenuField*			fSoundRate;
			BCheckBox*			fSoundTags;
			BCheckBox*			fSoundCopy;
			BTextControl*		fSoundStart;
			BTextControl*		fSoundEnd;

			BMenuField*			fContainer;
			BMenuField*			fVideo;
			BMenuField*			fSize;
			BSlider*			fQuality;
			BStringView*		fQualityLabel;
			BMenuField*			fAudio;
			BMenuField*			fAudioBitrate;
			BMenuField*			fSubtitles;

			BMenuField*			fEncoder;
			BMenuField*			fSpeed;
			BRadioButton*		fConstantQuality;
			BRadioButton*		fAverageBitrate;
			BTextControl*		fBitrate;
			BMenuField*			fFrameRate;
			BCheckBox*			fConstantRate;

			BMenuField*			fDeinterlace;
			BMenuField*			fDenoise;
			BMenuField*			fSharpen;
			BMenuField*			fRotate;
			BCheckBox*			fGrayscale;
			BCheckBox*			fAutoCrop;
			BCheckBox*			fFlip;

			BMenuField*			fMixdown;
			BMenuField*			fSampleRate;
			BMenuField*			fTracks;
			BCheckBox*			fPassthrough;
			BCheckBox*			fChapters;
			BCheckBox*			fMetadata;

			BTextControl*		fStart;
			BTextControl*		fEnd;

			EncoderBadge*		fBadge;
			BStringView*		fFolderView;
			BButton*			fFolderButton;
			BCheckBox*			fNextToSource;

			OutputSettings		fSettings;
			BString				fPresetName;
			bool				fSoundMode;
			OutputSettings		fOtherSettings;		// of the kind not shown
			BString				fOtherPreset;
			BString				fFolder;
			SourceInfo			fSource;
			bool				fHaveSource;
			bool				fUpdating;
};


#endif	// OUTPUT_VIEW_H
