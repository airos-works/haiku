/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "OutputView.h"

#include <Button.h>
#include <Catalog.h>
#include <CheckBox.h>
#include <ControlLook.h>
#include <LayoutBuilder.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <PopUpMenu.h>
#include <RadioButton.h>
#include <SeparatorView.h>
#include <Slider.h>
#include <StringView.h>
#include <TabView.h>
#include <TextControl.h>
#include <Window.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>

#include "HardwareCaps.h"
#include "LavcVideoEncoder.h"
#include "MediaProbe.h"
#include "Messages.h"
#include "Presets.h"
#include "SoftwareEncoders.h"
#include "Transcoder.h"
#include "VideoEncoder.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "Output"


static const uint32 kMsgBadgeChanged = 'bdch';


// #pragma mark - EncoderDecision


EncoderDecision
decide_encoder(const OutputSettings& settings, const SourceInfo* info)
{
	EncoderDecision decision;
	decision.kind = kBadgeNeutral;
	decision.cpuFallback = false;
	decision.impossible = false;

	video_codec codec = settings.videoCodec;
	if (!container_info(settings.container).video || codec == kVideoNone
		|| (info != NULL && !info->HasVideo())) {
		decision.text = B_TRANSLATE("Sound only: no video is encoded.");
		return decision;
	}
	if (codec == kVideoCopy) {
		decision.text = B_TRANSLATE("The video is copied as it is: nothing is "
			"encoded, so it is quick.");
		return decision;
	}

	int width = 1920, height = 1080;
	if (info != NULL && info->HasVideo())
		Transcoder::OutputSize(*info, settings, &width, &height);

	HardwareInventory& inventory = HardwareInventory::Default();
	if (!inventory.IsProbed())
		inventory.Probe();
	const char* name = video_codec_info(codec).name;

	if (settings.encoderPreference == kPreferHardware) {
		const GpuDevice* device = inventory.EncoderFor(codec, width, height,
			settings.gpu.String());
		if (device != NULL) {
			decision.kind = kBadgeHardware;
			decision.text.SetToFormat(B_TRANSLATE("%s is encoded on the GPU: "
				"%s."), name, device->name.String());
			decision.detail.SetToFormat("%s, %s", device->engine.String(),
				backend_name(device->backend));
			return decision;
		}
		decision.cpuFallback = true;
		decision.kind = kBadgeWarning;
		if (inventory.HasHardwareEncoder()) {
			decision.text.SetToFormat(B_TRANSLATE("No GPU here can encode %s: "
				"it will be encoded on the CPU, which takes longer and uses "
				"more power."), name);
		} else {
			decision.text.SetToFormat(B_TRANSLATE("This computer has no GPU "
				"video encoder: %s will be encoded on the CPU, which takes "
				"longer and uses more power."), name);
		}
		decision.detail = inventory.WhyNotHardware(codec, width, height);
	} else {
		decision.text.SetToFormat(B_TRANSLATE("%s is encoded on the CPU, as "
			"chosen under Video."), name);
	}

	BString missing;
	if (!SoftwareEncoders::IsAvailable(codec, &missing)
		&& !LavcVideoEncoder::IsAvailable(codec)) {
		decision.kind = kBadgeError;
		decision.impossible = true;
		if (!missing.IsEmpty() && SoftwareEncoders::Installable()) {
			decision.text.SetToFormat(B_TRANSLATE("%s cannot be encoded here: "
				"install the %s package."), name, missing.String());
		} else if (!missing.IsEmpty()) {
			decision.text.SetToFormat(B_TRANSLATE("%s cannot be encoded on "
				"this computer: there is no %s for its processor yet."), name,
				missing.String());
		} else {
			decision.text.SetToFormat(B_TRANSLATE("Nothing here can encode "
				"%s."), name);
		}
	}
	return decision;
}


// #pragma mark - EncoderBadge


class EncoderBadge : public BView {
public:
	EncoderBadge()
		:
		BView("encoder badge", B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE),
		fKind(kBadgeNeutral)
	{
		SetViewColor(B_TRANSPARENT_COLOR);
		font_height height;
		be_plain_font->GetHeight(&height);
		float line = ceilf(height.ascent + height.descent + height.leading);
		SetExplicitMinSize(BSize(100, line * 2 + 12));
		SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, line * 2 + 12));
	}

	void Set(const EncoderDecision& decision)
	{
		fKind = decision.kind;
		fText = decision.text;
		SetToolTip(decision.detail.IsEmpty() ? NULL : decision.detail.String());
		Invalidate();
	}

	virtual void Draw(BRect updateRect)
	{
		BRect bounds = Bounds();
		rgb_color panel = ui_color(B_PANEL_BACKGROUND_COLOR);
		SetHighColor(panel);
		FillRect(bounds);

		rgb_color tint, text = ui_color(B_PANEL_TEXT_COLOR);
		const char* mark;
		switch (fKind) {
			case kBadgeHardware:
				tint = (rgb_color){ 52, 168, 83, 255 };
				mark = "⚡";
				break;
			case kBadgeWarning:
				tint = (rgb_color){ 242, 153, 0, 255 };
				mark = "⚠";
				break;
			case kBadgeError:
				tint = (rgb_color){ 217, 48, 37, 255 };
				mark = "✕";
				break;
			default:
				tint = tint_color(panel, B_DARKEN_2_TINT);
				mark = "ℹ";
				break;
		}
		rgb_color fill = panel;
		float mix = panel.IsLight() ? 0.16f : 0.28f;
		fill.red = (uint8)(panel.red * (1 - mix) + tint.red * mix);
		fill.green = (uint8)(panel.green * (1 - mix) + tint.green * mix);
		fill.blue = (uint8)(panel.blue * (1 - mix) + tint.blue * mix);
		BRect box = bounds.InsetByCopy(1, 1);
		SetHighColor(fill);
		FillRoundRect(box, 5, 5);
		SetHighColor(tint);
		StrokeRoundRect(box, 5, 5);

		font_height height;
		GetFontHeight(&height);
		float line = ceilf(height.ascent + height.descent + height.leading);
		float x = box.left + 8;
		BFont font;
		GetFont(&font);
		BFont big(font);
		big.SetSize(font.Size() * 1.3f);
		SetFont(&big);
		SetHighColor(tint);
		DrawString(mark, BPoint(x, box.top + 4 + height.ascent * 1.15f));
		x += big.StringWidth("⚡") + 8;
		SetFont(&font);
		SetHighColor(text);

		// two lines at most, words wrapped
		float width = box.right - 8 - x;
		BString rest = fText;
		float y = box.top + 4 + height.ascent;
		if (rest.Length() > 0 && StringWidth(rest.String()) <= width) {
			y = box.top + (box.Height() - line) / 2 + height.ascent;
			DrawString(rest.String(), BPoint(x, y));
			return;
		}
		for (int lineNumber = 0; lineNumber < 2 && !rest.IsEmpty();
				lineNumber++) {
			BString piece = rest;
			if (lineNumber == 0) {
				int32 cut = rest.Length();
				while (cut > 0 && StringWidth(rest.String(), cut) > width) {
					int32 space = rest.FindLast(' ', cut - 1);
					if (space <= 0)
						break;
					cut = space;
				}
				piece.Truncate(cut);
				rest.Remove(0, min_c(cut + 1, rest.Length()));
			} else {
				TruncateString(&piece, B_TRUNCATE_END, width);
				rest = "";
			}
			DrawString(piece.String(), BPoint(x, y));
			y += line;
		}
	}

private:
	badge_kind	fKind;
	BString		fText;
};


// #pragma mark - menu helpers


static BMenuField*
make_field(const char* name, const char* label)
{
	BPopUpMenu* menu = new BPopUpMenu("");
	menu->SetLabelFromMarked(true);
	return new BMenuField(name, label, menu);
}


static BMenuItem*
add_item(BMenuField* field, const char* label, int32 value,
	bool enabled = true, uint32 what = kMsgSettingsChanged)
{
	BMessage* message = new BMessage(what);
	message->AddInt32("value", value);
	BMenuItem* item = new BMenuItem(label, message);
	item->SetEnabled(enabled);
	field->Menu()->AddItem(item);
	return item;
}


static int32
field_value(BMenuField* field, int32 fallback)
{
	BMenuItem* item = field->Menu()->FindMarked();
	int32 value;
	if (item == NULL || item->Message() == NULL
		|| item->Message()->FindInt32("value", &value) != B_OK)
		return fallback;
	return value;
}


static void
select_value(BMenuField* field, int32 value)
{
	BMenu* menu = field->Menu();
	for (int32 i = 0; i < menu->CountItems(); i++) {
		BMenuItem* item = menu->ItemAt(i);
		int32 itemValue;
		if (item->Message() != NULL
			&& item->Message()->FindInt32("value", &itemValue) == B_OK
			&& itemValue == value) {
			item->SetMarked(true);
			return;
		}
	}
	for (int32 i = 0; i < menu->CountItems(); i++) {
		BMenuItem* item = menu->ItemAt(i);
		if (item->Message() != NULL && item->IsEnabled()) {
			item->SetMarked(true);
			return;
		}
	}
}


static BString
format_time(bigtime_t time)
{
	if (time <= 0)
		return BString();
	int64 seconds = time / 1000000;
	BString text;
	text.SetToFormat("%" B_PRId64 ":%02" B_PRId64 ":%02" B_PRId64,
		seconds / 3600, seconds / 60 % 60, seconds % 60);
	return text;
}


static bigtime_t
parse_time(const char* text)
{
	if (text == NULL || text[0] == '\0')
		return 0;
	double parts[3] = { 0, 0, 0 };
	int count = 0;
	const char* p = text;
	while (*p != '\0' && count < 3) {
		parts[count++] = strtod(p, (char**)&p);
		if (*p == ':')
			p++;
		else
			break;
	}
	double seconds = 0;
	for (int i = 0; i < count; i++)
		seconds = seconds * 60 + parts[i];
	return (bigtime_t)(seconds * 1000000);
}


// #pragma mark - OutputView


OutputView::OutputView()
	:
	BView("output", B_WILL_DRAW),
	fSoundMode(false),
	fHaveSource(false),
	fUpdating(false)
{
	SetViewUIColor(B_PANEL_BACKGROUND_COLOR);

	fPreset = make_field("preset", B_TRANSLATE("Preset:"));
	_BuildPresetMenu();

	// Summary
	fContainer = make_field("container", B_TRANSLATE("Format:"));
	for (int i = 0; i < kContainerCount; i++) {
		add_item(fContainer, container_info((container_kind)i).name, i);
		if (i == kContainerMOV)
			fContainer->Menu()->AddSeparatorItem();
	}
	fVideo = make_field("video", B_TRANSLATE("Video:"));
	fSize = make_field("size", B_TRANSLATE("Size:"));
	add_item(fSize, B_TRANSLATE("Same as the source"), 0);
	add_item(fSize, B_TRANSLATE("2160p (4K)"), 2160);
	add_item(fSize, B_TRANSLATE("1440p"), 1440);
	add_item(fSize, B_TRANSLATE("1080p (Full HD)"), 1080);
	add_item(fSize, B_TRANSLATE("720p (HD)"), 720);
	add_item(fSize, B_TRANSLATE("576p (PAL)"), 576);
	add_item(fSize, B_TRANSLATE("480p (NTSC)"), 480);
	add_item(fSize, B_TRANSLATE("360p"), 360);

	fQuality = new BSlider("quality", B_TRANSLATE("Quality:"),
		new BMessage(kMsgSettingsChanged), 0, 100, B_HORIZONTAL);
	fQuality->SetModificationMessage(new BMessage(kMsgSettingsChanged));
	fQuality->SetHashMarks(B_HASH_MARKS_BOTTOM);
	fQuality->SetHashMarkCount(11);
	fQuality->SetLimitLabels(B_TRANSLATE("Smaller file"),
		B_TRANSLATE("Better picture"));
	fQualityLabel = new BStringView("quality label", "");
	fQualityLabel->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
	fQualityLabel->SetExplicitMinSize(BSize(
		be_plain_font->StringWidth("Constant QP 99 · 999 kbit/s"),
		B_SIZE_UNSET));

	fAudio = make_field("audio", B_TRANSLATE("Audio:"));
	fAudioBitrate = make_field("audio bitrate", NULL);
	static const int kBitrates[] = { 64, 96, 128, 160, 192, 224, 256, 320,
		384, 448, 512, 640 };
	for (size_t i = 0; i < sizeof(kBitrates) / sizeof(kBitrates[0]); i++) {
		BString label;
		label.SetToFormat(B_TRANSLATE("%d kbit/s"), kBitrates[i]);
		add_item(fAudioBitrate, label.String(), kBitrates[i]);
	}
	fSubtitles = make_field("subtitles", B_TRANSLATE("Subtitles:"));
	for (int i = 0; i < kSubtitleModeCount; i++)
		add_item(fSubtitles, subtitle_mode_name((subtitle_mode)i), i);

	// Video
	fEncoder = make_field("encoder", B_TRANSLATE("Encoder:"));
	fSpeed = make_field("speed", B_TRANSLATE("Speed:"));
	for (int i = 0; i < kSpeedCount; i++)
		add_item(fSpeed, speed_name((speed_preset)i), i);
	fConstantQuality = new BRadioButton("constant quality",
		B_TRANSLATE("Constant quality (the slider on Summary)"),
		new BMessage(kMsgSettingsChanged));
	fAverageBitrate = new BRadioButton("average bitrate",
		B_TRANSLATE("Average bit rate:"), new BMessage(kMsgSettingsChanged));
	fBitrate = new BTextControl("bitrate", NULL, "6000",
		new BMessage(kMsgSettingsChanged));
	fBitrate->SetModificationMessage(new BMessage(kMsgSettingsChanged));
	BStringView* kbits = new BStringView("kbits", B_TRANSLATE("kbit/s"));
	fFrameRate = make_field("frame rate", B_TRANSLATE("Frame rate:"));
	add_item(fFrameRate, B_TRANSLATE("Same as the source"), 0);
	static const struct {
		int			value;
		const char*	label;
	} kRates[] = {
		{ 23976, "23.976" }, { 24000, "24" }, { 25000, "25" },
		{ 29970, "29.97" }, { 30000, "30" }, { 50000, "50" },
		{ 59940, "59.94" }, { 60000, "60" },
	};
	for (size_t i = 0; i < sizeof(kRates) / sizeof(kRates[0]); i++)
		add_item(fFrameRate, kRates[i].label, kRates[i].value);
	fConstantRate = new BCheckBox("constant rate",
		B_TRANSLATE("Constant (otherwise at most this rate)"),
		new BMessage(kMsgSettingsChanged));

	// Filters
	fDeinterlace = make_field("deinterlace", B_TRANSLATE("Deinterlace:"));
	add_item(fDeinterlace, B_TRANSLATE("Off"), kDeinterlaceOff);
	add_item(fDeinterlace, B_TRANSLATE("Interlaced pictures"),
		kDeinterlaceAuto);
	add_item(fDeinterlace, B_TRANSLATE("Every picture"), kDeinterlaceAlways);
	const char* strengths[] = { B_TRANSLATE("Off"), B_TRANSLATE("Light"),
		B_TRANSLATE("Medium"), B_TRANSLATE("Strong") };
	fDenoise = make_field("denoise", B_TRANSLATE("Denoise:"));
	fSharpen = make_field("sharpen", B_TRANSLATE("Sharpen:"));
	for (int i = 0; i < 4; i++) {
		add_item(fDenoise, strengths[i], i);
		add_item(fSharpen, strengths[i], i);
	}
	fRotate = make_field("rotate", B_TRANSLATE("Turn:"));
	add_item(fRotate, B_TRANSLATE("No"), 0);
	add_item(fRotate, B_TRANSLATE("90° clockwise"), 90);
	add_item(fRotate, B_TRANSLATE("180°"), 180);
	add_item(fRotate, B_TRANSLATE("90° counterclockwise"), 270);
	fGrayscale = new BCheckBox("grayscale", B_TRANSLATE("Black and white"),
		new BMessage(kMsgSettingsChanged));
	fAutoCrop = new BCheckBox("crop", B_TRANSLATE("Cut off black borders"),
		new BMessage(kMsgSettingsChanged));
	fFlip = new BCheckBox("flip", B_TRANSLATE("Mirror left to right"),
		new BMessage(kMsgSettingsChanged));

	// Audio
	fMixdown = make_field("mixdown", B_TRANSLATE("Channels:"));
	for (int i = 0; i < kMixdownCount; i++)
		add_item(fMixdown, mixdown_name((audio_mixdown)i), i);
	fSampleRate = make_field("sample rate", B_TRANSLATE("Sample rate:"));
	add_item(fSampleRate, B_TRANSLATE("Same as the source"), 0);
	add_item(fSampleRate, "44.1 kHz", 44100);
	add_item(fSampleRate, "48 kHz", 48000);
	add_item(fSampleRate, "96 kHz", 96000);
	fTracks = make_field("tracks", B_TRANSLATE("Tracks:"));
	add_item(fTracks, B_TRANSLATE("The first track"), kAudioTracksFirst);
	add_item(fTracks, B_TRANSLATE("Every track"), kAudioTracksAll);
	add_item(fTracks, B_TRANSLATE("The default track"), kAudioTracksDefault);
	fPassthrough = new BCheckBox("passthrough",
		B_TRANSLATE("Copy tracks that are already in this format"),
		new BMessage(kMsgSettingsChanged));
	fChapters = new BCheckBox("chapters", B_TRANSLATE("Keep the chapters"),
		new BMessage(kMsgSettingsChanged));
	fMetadata = new BCheckBox("metadata",
		B_TRANSLATE("Keep the title and other tags"),
		new BMessage(kMsgSettingsChanged));

	// Range
	fStart = new BTextControl("start", B_TRANSLATE("From:"), "",
		new BMessage(kMsgSettingsChanged));
	fEnd = new BTextControl("end", B_TRANSLATE("To:"), "",
		new BMessage(kMsgSettingsChanged));
	BStringView* rangeHint = new BStringView("range hint",
		B_TRANSLATE("Hours:minutes:seconds; empty for the start and the end."));

	// A song's summary: the format, the sound and where it goes
	fSoundFormat = make_field("sound format", B_TRANSLATE("Format:"));
	for (int i = 0; i < kContainerCount; i++) {
		const ContainerInfo& info = container_info((container_kind)i);
		if (info.video)
			continue;
		const char* name = info.name;
		if (strncmp(name, "Audio: ", 7) == 0)
			name += 7;
		add_item(fSoundFormat, name, i);
	}
	fSoundCodec = make_field("sound codec", B_TRANSLATE("Sound:"));
	fSoundBitrate = make_field("sound bitrate", B_TRANSLATE("Bit rate:"));
	static const int kSoundBitrates[] = { 64, 96, 128, 160, 192, 224, 256,
		320 };
	for (size_t i = 0; i < sizeof(kSoundBitrates) / sizeof(kSoundBitrates[0]);
			i++) {
		BString label;
		label.SetToFormat(B_TRANSLATE("%d kbit/s"), kSoundBitrates[i]);
		add_item(fSoundBitrate, label.String(), kSoundBitrates[i]);
	}
	fSoundChannels = make_field("sound channels", B_TRANSLATE("Channels:"));
	for (int i = 0; i < kMixdownCount; i++)
		add_item(fSoundChannels, mixdown_name((audio_mixdown)i), i);
	fSoundRate = make_field("sound rate", B_TRANSLATE("Sample rate:"));
	add_item(fSoundRate, B_TRANSLATE("Same as the source"), 0);
	add_item(fSoundRate, "44.1 kHz", 44100);
	add_item(fSoundRate, "48 kHz", 48000);
	add_item(fSoundRate, "96 kHz", 96000);
	fSoundTags = new BCheckBox("sound tags",
		B_TRANSLATE("Keep the tags, the album artwork and the chapters"),
		new BMessage(kMsgSettingsChanged));
	fSoundCopy = new BCheckBox("sound copy",
		B_TRANSLATE("Copy the sound when it is already in this format"),
		new BMessage(kMsgSettingsChanged));
	fSoundStart = new BTextControl("sound start", B_TRANSLATE("From:"), "",
		new BMessage(kMsgSettingsChanged));
	fSoundEnd = new BTextControl("sound end", B_TRANSLATE("To:"), "",
		new BMessage(kMsgSettingsChanged));
	BStringView* soundRangeHint = new BStringView("sound range hint",
		B_TRANSLATE("Hours:minutes:seconds; empty for the start and the end."));

	fBadge = new EncoderBadge();
	fFolderView = new BStringView("folder", "");
	fFolderView->SetExplicitMinSize(BSize(50, B_SIZE_UNSET));
	fFolderView->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
	fFolderButton = new BButton("choose folder", B_TRANSLATE("Choose…"),
		new BMessage(kMsgChooseFolder));
	fNextToSource = new BCheckBox("next to source",
		B_TRANSLATE("Next to each source"), new BMessage(kMsgSameFolder));

	float spacing = be_control_look->DefaultItemSpacing();

	BView* summary = new BView(B_TRANSLATE("Summary"), 0);
	BLayoutBuilder::Grid<>(summary, B_USE_DEFAULT_SPACING,
			B_USE_SMALL_SPACING)
		.SetInsets(B_USE_DEFAULT_SPACING)
		.AddMenuField(fContainer, 0, 0)
		.AddMenuField(fVideo, 0, 1)
		.AddMenuField(fSize, 0, 2)
		.Add(fQuality, 0, 3, 2)
		.Add(fQualityLabel, 1, 4)
		.Add(fAudio->CreateLabelLayoutItem(), 0, 5)
		.AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING, 1, 5)
			.Add(fAudio->CreateMenuBarLayoutItem())
			.Add(fAudioBitrate)
		.End()
		.AddMenuField(fSubtitles, 0, 6)
		.AddGlue(2, 0)
		.AddGlue(0, 7, 3);

	BView* video = new BView(B_TRANSLATE("Video"), 0);
	BLayoutBuilder::Group<>(video, B_VERTICAL, B_USE_SMALL_SPACING)
		.SetInsets(B_USE_DEFAULT_SPACING)
		.AddGrid(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING)
			.AddMenuField(fEncoder, 0, 0)
			.AddMenuField(fSpeed, 0, 1)
			.AddMenuField(fFrameRate, 0, 2)
			.Add(fConstantRate, 1, 3)
			.AddGlue(2, 0)
		.End()
		.AddStrut(spacing)
		.Add(fConstantQuality)
		.AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING)
			.Add(fAverageBitrate)
			.Add(fBitrate)
			.Add(kbits)
			.AddGlue()
		.End()
		.AddGlue();

	BView* filters = new BView(B_TRANSLATE("Filters"), 0);
	BLayoutBuilder::Grid<>(filters, B_USE_DEFAULT_SPACING,
			B_USE_SMALL_SPACING)
		.SetInsets(B_USE_DEFAULT_SPACING)
		.AddMenuField(fDeinterlace, 0, 0)
		.AddMenuField(fDenoise, 0, 1)
		.AddMenuField(fSharpen, 0, 2)
		.AddMenuField(fRotate, 0, 3)
		.Add(fAutoCrop, 1, 4)
		.Add(fFlip, 1, 5)
		.Add(fGrayscale, 1, 6)
		.AddGlue(2, 0)
		.AddGlue(0, 7, 3);

	BView* audio = new BView(B_TRANSLATE("Audio"), 0);
	BLayoutBuilder::Grid<>(audio, B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING)
		.SetInsets(B_USE_DEFAULT_SPACING)
		.AddMenuField(fTracks, 0, 0)
		.AddMenuField(fMixdown, 0, 1)
		.AddMenuField(fSampleRate, 0, 2)
		.Add(fPassthrough, 1, 3)
		.Add(fChapters, 1, 4)
		.Add(fMetadata, 1, 5)
		.AddGlue(2, 0)
		.AddGlue(0, 6, 3);

	BView* range = new BView(B_TRANSLATE("Range"), 0);
	BLayoutBuilder::Group<>(range, B_VERTICAL, B_USE_SMALL_SPACING)
		.SetInsets(B_USE_DEFAULT_SPACING)
		.AddGrid(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING)
			.AddTextControl(fStart, 0, 0)
			.AddTextControl(fEnd, 0, 1)
			.AddGlue(2, 0)
		.End()
		.Add(rangeHint)
		.AddGlue();

	fTabs = new BTabView("tabs", B_WIDTH_FROM_LABEL);
	fTabs->AddTab(summary);
	fTabs->AddTab(video);
	fTabs->AddTab(filters);
	fTabs->AddTab(audio);
	fTabs->AddTab(range);
	fTabs->SetBorder(B_NO_BORDER);
	fTabs->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));

	// for a song: no Video or Filters, and its sound on the summary
	BView* soundSummary = new BView(B_TRANSLATE("Summary"), 0);
	BLayoutBuilder::Grid<>(soundSummary, B_USE_DEFAULT_SPACING,
			B_USE_SMALL_SPACING)
		.SetInsets(B_USE_DEFAULT_SPACING)
		.AddMenuField(fSoundFormat, 0, 0)
		.AddMenuField(fSoundCodec, 0, 1)
		.AddMenuField(fSoundBitrate, 0, 2)
		.AddMenuField(fSoundChannels, 0, 3)
		.AddMenuField(fSoundRate, 0, 4)
		.Add(fSoundTags, 1, 5)
		.Add(fSoundCopy, 1, 6)
		.AddGlue(2, 0)
		.AddGlue(0, 7, 3);
	BView* soundRange = new BView(B_TRANSLATE("Range"), 0);
	BLayoutBuilder::Group<>(soundRange, B_VERTICAL, B_USE_SMALL_SPACING)
		.SetInsets(B_USE_DEFAULT_SPACING)
		.AddGrid(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING)
			.AddTextControl(fSoundStart, 0, 0)
			.AddTextControl(fSoundEnd, 0, 1)
			.AddGlue(2, 0)
		.End()
		.Add(soundRangeHint)
		.AddGlue();
	fSoundTabs = new BTabView("sound tabs", B_WIDTH_FROM_LABEL);
	fSoundTabs->AddTab(soundSummary);
	fSoundTabs->AddTab(soundRange);
	fSoundTabs->SetBorder(B_NO_BORDER);
	fSoundTabs->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));
	fSoundTabs->Hide();

	BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_SMALL_SPACING)
		.AddGrid(B_USE_DEFAULT_SPACING, 0)
			.AddMenuField(fPreset, 0, 0)
			.AddGlue(2, 0)
		.End()
		.Add(fTabs, 1)
		.Add(fSoundTabs, 1)
		.Add(fBadge)
		.AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING)
			.Add(new BStringView("save to", B_TRANSLATE("Save to:")))
			.Add(fFolderView, 1)
			.Add(fNextToSource)
			.Add(fFolderButton)
		.End();

	SetSettings(OutputSettings(), NULL);
}


void
OutputView::AttachedToWindow()
{
	BView::AttachedToWindow();
	BView* controls[] = { fPreset, fContainer, fVideo, fSize, fAudio,
		fAudioBitrate, fSubtitles, fEncoder, fSpeed, fFrameRate, fDeinterlace,
		fDenoise, fSharpen, fRotate, fMixdown, fSampleRate, fTracks,
		fSoundFormat, fSoundCodec, fSoundBitrate, fSoundChannels, fSoundRate };
	for (size_t i = 0; i < sizeof(controls) / sizeof(controls[0]); i++)
		((BMenuField*)controls[i])->Menu()->SetTargetForItems(this);
	BControl* others[] = { fQuality, fConstantQuality, fAverageBitrate,
		fBitrate, fConstantRate, fGrayscale, fAutoCrop, fFlip, fPassthrough,
		fChapters, fMetadata, fStart, fEnd, fSoundTags, fSoundCopy,
		fSoundStart, fSoundEnd };
	for (size_t i = 0; i < sizeof(others) / sizeof(others[0]); i++)
		others[i]->SetTarget(this);
	fFolderButton->SetTarget(Window());
	fNextToSource->SetTarget(this);
}


/*!	A song sees the presets for songs, by format; a film sees its own and
	then those for songs, as "Sound only". */
void
OutputView::_BuildPresetMenu()
{
	BMenu* menu = fPreset->Menu();
	menu->RemoveItems(0, menu->CountItems(), true);
	add_item(fPreset, B_TRANSLATE("Custom"), -1, true, kMsgPresetChosen);
	for (int pass = 0; pass < 2; pass++) {
		bool sound = pass == 1;
		if (fSoundMode && !sound)
			continue;
		const char* category = NULL;
		for (int32 i = 0; i < Presets::Count(); i++) {
			const Preset& preset = Presets::At(i);
			if (preset.sound != sound)
				continue;
			bool newGroup = category == NULL
				|| (fSoundMode && strcmp(category, preset.category) != 0)
				|| (!sound && strcmp(category, preset.category) != 0);
			if (newGroup)
				menu->AddSeparatorItem();
			category = preset.category;
			BString label;
			if (fSoundMode)
				label = preset.name;
			else if (sound)
				label.SetToFormat(B_TRANSLATE("Sound only: %s"), preset.name);
			else
				label.SetToFormat("%s: %s", preset.category, preset.name);
			add_item(fPreset, label.String(), i, true, kMsgPresetChosen);
		}
	}
	if (Window() != NULL)
		menu->SetTargetForItems(this);
}


void
OutputView::_BuildVideoMenu(container_kind container)
{
	BMenu* menu = fVideo->Menu();
	menu->RemoveItems(0, menu->CountItems(), true);
	HardwareInventory& inventory = HardwareInventory::Default();
	if (!inventory.IsProbed())
		inventory.Probe();
	if (!container_info(container).video) {
		add_item(fVideo, video_codec_info(kVideoNone).name, kVideoNone);
	} else {
		for (int i = 0; i < kVideoCopy; i++) {
			video_codec codec = (video_codec)i;
			if (!container_holds_video(container, codec))
				continue;
			BString label = video_codec_info(codec).name;
			if (inventory.EncoderFor(codec, 1920, 1080) != NULL)
				label << B_TRANSLATE(" · GPU");
			add_item(fVideo, label.String(), i);
		}
		menu->AddSeparatorItem();
		add_item(fVideo, video_codec_info(kVideoCopy).name, kVideoCopy);
		add_item(fVideo, video_codec_info(kVideoNone).name, kVideoNone);
	}
	if (Window() != NULL)
		menu->SetTargetForItems(this);
}


static void
fill_audio_menu(BMenuField* field, container_kind container)
{
	BMenu* menu = field->Menu();
	menu->RemoveItems(0, menu->CountItems(), true);
	for (int i = 0; i < kAudioCopy; i++) {
		audio_codec codec = (audio_codec)i;
		if (container_holds_audio(container, codec))
			add_item(field, audio_codec_info(codec).name, i);
	}
	menu->AddSeparatorItem();
	add_item(field, audio_codec_info(kAudioCopy).name, kAudioCopy);
	if (container_info(container).video)
		add_item(field, audio_codec_info(kAudioNone).name, kAudioNone);
}


void
OutputView::_BuildAudioMenu(container_kind container)
{
	fill_audio_menu(fAudio, container);
	fill_audio_menu(fSoundCodec, container);
	if (Window() != NULL) {
		fAudio->Menu()->SetTargetForItems(this);
		fSoundCodec->Menu()->SetTargetForItems(this);
	}
}


void
OutputView::_BuildEncoderMenu(video_codec codec)
{
	BMenu* menu = fEncoder->Menu();
	menu->RemoveItems(0, menu->CountItems(), true);
	HardwareInventory& inventory = HardwareInventory::Default();
	BMenuItem* automatic = add_item(fEncoder,
		B_TRANSLATE("Automatic: the GPU when it can"), 0);
	(void)automatic;
	int width = 1920, height = 1080;
	if (fHaveSource)
		Transcoder::OutputSize(fSource, fSettings, &width, &height);
	for (int32 i = 0; i < inventory.CountDevices(); i++) {
		const GpuDevice& device = inventory.DeviceAt(i);
		if (device.encode.empty())
			continue;
		BString label = device.name;
		if (!device.ready)
			label << " (" << B_TRANSLATE("unavailable") << ")";
		BMenuItem* item = add_item(fEncoder, label.String(), i + 1,
			device.CanEncode(codec, width, height));
		item->Message()->AddString("gpu", device.id);
	}
	BString cpu = B_TRANSLATE("The CPU");
	const VideoCodecInfo& info = video_codec_info(codec);
	if (info.software == kSoftwareX264)
		cpu << " (x264)";
	else if (info.software == kSoftwareX265)
		cpu << " (x265)";
	else if (info.software == kSoftwareSvtAv1)
		cpu << " (SVT-AV1)";
	add_item(fEncoder, cpu.String(), -1);
	if (Window() != NULL)
		menu->SetTargetForItems(this);
}


void
OutputView::SetSettings(const OutputSettings& settings,
	const char* presetName)
{
	fUpdating = true;
	fSettings = settings;
	fSettings.Validate();
	fPresetName = presetName != NULL ? presetName : "";
	int32 presetIndex = presetName != NULL ? Presets::IndexOf(presetName) : -1;
	select_value(fPreset, presetIndex);

	select_value(fContainer, fSettings.container);
	_BuildVideoMenu(fSettings.container);
	select_value(fVideo, fSettings.videoCodec);
	_BuildAudioMenu(fSettings.container);
	select_value(fAudio, fSettings.audioCodec);
	select_value(fSoundFormat, fSettings.container);
	select_value(fSoundCodec, fSettings.audioCodec);
	select_value(fSoundBitrate, fSettings.audioBitrate);
	select_value(fSoundChannels, fSettings.mixdown);
	select_value(fSoundRate, fSettings.sampleRate);
	fSoundTags->SetValue(fSettings.copyMetadata);
	fSoundCopy->SetValue(fSettings.passthroughWhenPossible);
	fSoundStart->SetText(format_time(fSettings.start).String());
	fSoundEnd->SetText(format_time(fSettings.end).String());
	select_value(fSize, fSettings.maxHeight);
	fQuality->SetValue(fSettings.quality);
	select_value(fAudioBitrate, fSettings.audioBitrate);
	select_value(fSubtitles, fSettings.subtitles);

	_BuildEncoderMenu(fSettings.videoCodec);
	if (fSettings.encoderPreference == kPreferSoftware)
		select_value(fEncoder, -1);
	else {
		int32 value = 0;
		HardwareInventory& inventory = HardwareInventory::Default();
		for (int32 i = 0; i < inventory.CountDevices(); i++) {
			if (inventory.DeviceAt(i).id == fSettings.gpu)
				value = i + 1;
		}
		select_value(fEncoder, value);
	}
	select_value(fSpeed, fSettings.speed);
	fConstantQuality->SetValue(fSettings.rateMode == kRateConstantQuality);
	fAverageBitrate->SetValue(fSettings.rateMode == kRateAverageBitrate);
	BString bitrate;
	bitrate << fSettings.videoBitrate;
	fBitrate->SetText(bitrate.String());
	select_value(fFrameRate, fSettings.frameRate);
	fConstantRate->SetValue(!fSettings.peakFrameRate);

	select_value(fDeinterlace, fSettings.deinterlace);
	select_value(fDenoise, fSettings.denoise);
	select_value(fSharpen, fSettings.sharpen);
	select_value(fRotate, fSettings.rotation);
	fGrayscale->SetValue(fSettings.grayscale);
	fAutoCrop->SetValue(fSettings.autoCrop);
	fFlip->SetValue(fSettings.flip);

	select_value(fMixdown, fSettings.mixdown);
	select_value(fSampleRate, fSettings.sampleRate);
	select_value(fTracks, fSettings.audioTracks);
	fPassthrough->SetValue(fSettings.passthroughWhenPossible);
	fChapters->SetValue(fSettings.copyChapters);
	fMetadata->SetValue(fSettings.copyMetadata);

	fStart->SetText(format_time(fSettings.start).String());
	fEnd->SetText(format_time(fSettings.end).String());

	_UpdateEnabling();
	_UpdateQualityLabel();
	UpdateHardwareStatus();
	fUpdating = false;
}


OutputSettings
OutputView::Settings() const
{
	return fSettings;
}


void
OutputView::SetSettingsFor(bool sound, const OutputSettings& settings,
	const char* presetName)
{
	if (sound == fSoundMode) {
		SetSettings(settings, presetName);
		return;
	}
	fOtherSettings = settings;
	fOtherSettings.Validate();
	fOtherPreset = presetName != NULL ? presetName : "";
}


OutputSettings
OutputView::SettingsFor(bool sound) const
{
	return sound == fSoundMode ? fSettings : fOtherSettings;
}


BString
OutputView::PresetNameFor(bool sound) const
{
	return sound == fSoundMode ? fPresetName : fOtherPreset;
}


/*!	Shows the other kind's settings and keeps the ones shown so far. */
void
OutputView::_SetSoundMode(bool sound)
{
	if (sound == fSoundMode)
		return;
	OutputSettings settings = fOtherSettings;
	BString preset = fOtherPreset;
	fOtherSettings = fSettings;
	fOtherPreset = fPresetName;
	fSoundMode = sound;
	_BuildPresetMenu();
	if (sound) {
		fTabs->Hide();
		fSoundTabs->Show();
	} else {
		fSoundTabs->Hide();
		fTabs->Show();
	}
	SetSettings(settings, preset.IsEmpty() ? NULL : preset.String());
}


void
OutputView::SetSource(const SourceInfo* info)
{
	fHaveSource = info != NULL;
	if (info != NULL) {
		fSource = *info;
		_SetSoundMode(info->IsSoundOnly());
	}
	_BuildEncoderMenu(fSettings.videoCodec);
	if (fSettings.encoderPreference == kPreferSoftware)
		select_value(fEncoder, -1);
	else {
		int32 value = 0;
		HardwareInventory& inventory = HardwareInventory::Default();
		for (int32 i = 0; i < inventory.CountDevices(); i++) {
			if (inventory.DeviceAt(i).id == fSettings.gpu)
				value = i + 1;
		}
		select_value(fEncoder, value);
	}
	UpdateHardwareStatus();
}


void
OutputView::SetEnabled(bool enabled)
{
	BMenuField* fields[] = { fPreset, fContainer, fVideo, fSize, fAudio,
		fAudioBitrate, fSubtitles, fEncoder, fSpeed, fFrameRate, fDeinterlace,
		fDenoise, fSharpen, fRotate, fMixdown, fSampleRate, fTracks,
		fSoundFormat, fSoundCodec, fSoundBitrate, fSoundChannels, fSoundRate };
	for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
		fields[i]->SetEnabled(enabled);
	BControl* others[] = { fQuality, fConstantQuality, fAverageBitrate,
		fBitrate, fConstantRate, fGrayscale, fAutoCrop, fFlip, fPassthrough,
		fChapters, fMetadata, fStart, fEnd, fSoundTags, fSoundCopy,
		fSoundStart, fSoundEnd, fFolderButton, fNextToSource };
	for (size_t i = 0; i < sizeof(others) / sizeof(others[0]); i++)
		others[i]->SetEnabled(enabled);
	if (enabled)
		_UpdateEnabling();
}


void
OutputView::SetDestination(const char* folder, bool nextToSource)
{
	fFolder = folder != NULL ? folder : "";
	fNextToSource->SetValue(nextToSource);
	fFolderView->SetText(nextToSource ? B_TRANSLATE("the source's folder")
		: fFolder.String());
	fFolderButton->SetEnabled(!nextToSource);
}


bool
OutputView::NextToSource() const
{
	return fNextToSource->Value() == B_CONTROL_ON;
}


void
OutputView::_UpdateEnabling()
{
	bool video = container_info(fSettings.container).video
		&& fSettings.videoCodec != kVideoNone;
	bool encoding = video && fSettings.videoCodec != kVideoCopy;
	fSize->SetEnabled(encoding);
	fQuality->SetEnabled(encoding
		&& fSettings.rateMode == kRateConstantQuality
		&& fSettings.videoCodec != kVideoFFV1);
	fEncoder->SetEnabled(encoding);
	fSpeed->SetEnabled(encoding);
	fConstantQuality->SetEnabled(encoding);
	fAverageBitrate->SetEnabled(encoding && fSettings.videoCodec != kVideoFFV1);
	fBitrate->SetEnabled(encoding && fSettings.rateMode == kRateAverageBitrate);
	fFrameRate->SetEnabled(encoding);
	fConstantRate->SetEnabled(encoding && fSettings.frameRate > 0);
	fDeinterlace->SetEnabled(encoding);
	fDenoise->SetEnabled(encoding);
	fSharpen->SetEnabled(encoding);
	fRotate->SetEnabled(encoding);
	fGrayscale->SetEnabled(encoding);
	fAutoCrop->SetEnabled(encoding);
	fFlip->SetEnabled(encoding);
	fSubtitles->SetEnabled(container_info(fSettings.container).video);

	bool audio = fSettings.audioCodec != kAudioNone
		&& fSettings.audioCodec != kAudioCopy;
	fAudioBitrate->SetEnabled(audio
		&& !audio_codec_info(fSettings.audioCodec).lossless);
	fSoundBitrate->SetEnabled(audio
		&& !audio_codec_info(fSettings.audioCodec).lossless);
	fSoundChannels->SetEnabled(audio);
	fSoundRate->SetEnabled(audio);
	fSoundCopy->SetEnabled(audio);
	fMixdown->SetEnabled(audio);
	fSampleRate->SetEnabled(audio);
	fPassthrough->SetEnabled(audio);
	fTracks->SetEnabled(fSettings.audioCodec != kAudioNone
		&& container_info(fSettings.container).video);
}


void
OutputView::_UpdateQualityLabel()
{
	BString text;
	video_codec codec = fSettings.videoCodec;
	int32 q = fSettings.quality;
	if (codec == kVideoCopy || codec == kVideoNone
		|| !container_info(fSettings.container).video) {
		fQualityLabel->SetText("");
		return;
	}
	if (fSettings.rateMode == kRateAverageBitrate) {
		text.SetToFormat(B_TRANSLATE("%" B_PRId32 " kbit/s on average"),
			fSettings.videoBitrate);
		fQualityLabel->SetText(text.String());
		return;
	}
	EncoderDecision decision = decide_encoder(fSettings,
		fHaveSource ? &fSource : NULL);
	if (decision.kind == kBadgeHardware) {
		bool av1 = codec == kVideoAV1 || codec == kVideoAV1_10;
		if (av1)
			text.SetToFormat(B_TRANSLATE("Quantizer %d"),
				quality_to_crf(q, 60, 180));
		else
			text.SetToFormat(B_TRANSLATE("Constant QP %d"),
				quality_to_crf(q, 16, 36));
	} else {
		switch (video_codec_info(codec).software) {
			case kSoftwareX264:
			case kSoftwareX265:
				text.SetToFormat(B_TRANSLATE("RF %d"),
					quality_to_crf(q, 14, 34));
				break;
			case kSoftwareSvtAv1:
				text.SetToFormat(B_TRANSLATE("CRF %d"),
					quality_to_crf(q, 18, 48));
				break;
			default:
				if (codec == kVideoFFV1)
					text = B_TRANSLATE("Lossless");
				else if (codec == kVideoVP8 || codec == kVideoVP9
					|| codec == kVideoVP9_10)
					text.SetToFormat(B_TRANSLATE("CRF %d"),
						quality_to_crf(q, 15, 50));
				else if (codec == kVideoTheora)
					text.SetToFormat(B_TRANSLATE("Quality %d of 10"),
						(q + 5) / 10);
				else
					text.SetToFormat(B_TRANSLATE("Quantizer %d"),
						quality_to_crf(q, 2, 20));
				break;
		}
	}
	fQualityLabel->SetText(text.String());
}


void
OutputView::UpdateHardwareStatus()
{
	if (fSoundMode) {
		_UpdateSoundSummary();
		return;
	}
	EncoderDecision decision = decide_encoder(fSettings,
		fHaveSource ? &fSource : NULL);
	fBadge->Set(decision);
	_UpdateQualityLabel();
}


/*!	What a song becomes, and about how big it will be. */
void
OutputView::_UpdateSoundSummary()
{
	EncoderDecision decision;
	decision.kind = kBadgeNeutral;
	decision.cpuFallback = false;
	decision.impossible = false;

	const AudioTrackInfo* source = fHaveSource && !fSource.audio.empty()
		? &fSource.audio[0] : NULL;
	audio_codec codec = fSettings.audioCodec;
	const AudioCodecInfo& info = audio_codec_info(codec);
	BString what;
	if (codec == kAudioCopy) {
		what = B_TRANSLATE("The sound is copied as it is");
	} else {
		what = info.name;
		if (!info.lossless) {
			BString rate;
			rate.SetToFormat(B_TRANSLATE(", %d kbit/s"),
				(int)fSettings.audioBitrate);
			what << rate;
		}
		int channels = source != NULL ? source->channels : 2;
		switch (fSettings.mixdown) {
			case kMixdownMono: channels = 1; break;
			case kMixdownStereo: channels = 2; break;
			case kMixdown51: channels = 6; break;
			case kMixdown71: channels = 8; break;
			default: break;
		}
		channels = std::min(channels, info.maxChannels);
		what << ", " << (channels == 1 ? B_TRANSLATE("mono")
			: channels == 2 ? B_TRANSLATE("stereo")
			: B_TRANSLATE("surround"));
		int sampleRate = fSettings.sampleRate > 0 ? fSettings.sampleRate
			: source != NULL ? source->sampleRate : 0;
		if (sampleRate > 0) {
			BString rate;
			rate.SetToFormat(", %g kHz", sampleRate / 1000.0);
			what << rate;
		}
	}

	// about how big: the bit rate over the duration; lossless as FLAC
	// usually comes out, a little over half the size of the PCM
	bigtime_t duration = fHaveSource ? fSource.duration : 0;
	if (duration > 0 && fSettings.end > fSettings.start)
		duration = fSettings.end - fSettings.start;
	else if (duration > fSettings.start)
		duration -= fSettings.start;
	double bitsPerSecond = 0;
	if (codec == kAudioCopy && source != NULL)
		bitsPerSecond = source->bitRate;
	else if (!info.lossless)
		bitsPerSecond = fSettings.audioBitrate * 1000.0;
	else if (source != NULL) {
		int rate = fSettings.sampleRate > 0 ? fSettings.sampleRate
			: source->sampleRate;
		int bits = codec == kAudioFLAC16 || codec == kAudioALAC16
			|| codec == kAudioPCM16 || source->bitDepth == 0
			|| source->bitDepth <= 16 ? 16 : 24;
		int channels = std::min(source->channels, info.maxChannels);
		bitsPerSecond = (double)rate * bits * channels;
		if (codec != kAudioPCM16 && codec != kAudioPCM24)
			bitsPerSecond *= 0.6;
	}
	if (bitsPerSecond > 0 && duration > 0) {
		off_t bytes = (off_t)(bitsPerSecond * duration / 8000000.0);
		BString size;
		size.SetToFormat(B_TRANSLATE(" · about %s"),
			MediaProbe::FormatSize(bytes).String());
		what << size;
	}
	decision.text = what;
	fBadge->Set(decision);
}


/*!	A song's settings, from its Summary and Range. */
void
OutputView::_ReadSoundControls()
{
	container_kind container = (container_kind)field_value(fSoundFormat,
		fSettings.container);
	if (container != fSettings.container) {
		// the sound the new format takes
		fSettings.container = container;
		fSettings.audioCodec = (audio_codec)field_value(fSoundCodec,
			fSettings.audioCodec);
		fSettings.Validate();
		fUpdating = true;
		_BuildAudioMenu(container);
		select_value(fSoundCodec, fSettings.audioCodec);
		select_value(fAudio, fSettings.audioCodec);
		select_value(fContainer, container);
		fUpdating = false;
	}
	fSettings.audioCodec = (audio_codec)field_value(fSoundCodec,
		fSettings.audioCodec);
	fSettings.audioBitrate = field_value(fSoundBitrate, fSettings.audioBitrate);
	fSettings.mixdown = (audio_mixdown)field_value(fSoundChannels,
		fSettings.mixdown);
	fSettings.sampleRate = field_value(fSoundRate, 0);
	fSettings.copyMetadata = fSoundTags->Value() == B_CONTROL_ON;
	fSettings.copyChapters = fSettings.copyMetadata;
	fSettings.passthroughWhenPossible = fSoundCopy->Value() == B_CONTROL_ON;
	fSettings.start = parse_time(fSoundStart->Text());
	fSettings.end = parse_time(fSoundEnd->Text());
}


void
OutputView::_Changed()
{
	if (fUpdating)
		return;
	// any change makes the settings the user's own
	if (!fPresetName.IsEmpty()) {
		fPresetName = "";
		fUpdating = true;
		select_value(fPreset, -1);
		fUpdating = false;
	}
	BMessage changed(kMsgSettingsChanged);
	Window()->PostMessage(&changed);
}


void
OutputView::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgPresetChosen:
		{
			int32 index;
			if (message->FindInt32("value", &index) != B_OK || index < 0)
				break;
			const Preset& preset = Presets::At(index);
			OutputSettings settings = preset.settings;
			// From the command line a preset goes to the settings of its
			// kind; from the menu to the ones shown.
			bool sound = message->GetBool("by kind", false)
				? preset.sound : fSoundMode;
			OutputSettings current = SettingsFor(sound);
			// a preset says what to make, not where or which part
			settings.start = current.start;
			settings.end = current.end;
			settings.gpu = current.gpu;
			if (current.encoderPreference == kPreferSoftware)
				settings.encoderPreference = kPreferSoftware;
			SetSettingsFor(sound, settings, preset.name);
			BMessage changed(kMsgSettingsChanged);
			Window()->PostMessage(&changed);
			break;
		}

		case kMsgSameFolder:
			SetDestination(fFolder.String(), NextToSource());
			Window()->PostMessage(kMsgSettingsChanged);
			break;

		case kMsgSettingsChanged:
		{
			if (fUpdating)
				break;
			OutputSettings old = fSettings;
			if (fSoundMode) {
				_ReadSoundControls();
				_UpdateEnabling();
				UpdateHardwareStatus();
				if (!(old == fSettings))
					_Changed();
				break;
			}
			fSettings.container = (container_kind)field_value(fContainer,
				fSettings.container);
			if (fSettings.container != old.container) {
				// the formats the new container takes
				fSettings.Validate();
				fUpdating = true;
				_BuildVideoMenu(fSettings.container);
				_BuildAudioMenu(fSettings.container);
				select_value(fVideo, fSettings.videoCodec);
				select_value(fAudio, fSettings.audioCodec);
				fUpdating = false;
			}
			fSettings.videoCodec = (video_codec)field_value(fVideo,
				fSettings.videoCodec);
			fSettings.audioCodec = (audio_codec)field_value(fAudio,
				fSettings.audioCodec);
			if (!container_holds_video(fSettings.container,
					fSettings.videoCodec)
				|| !container_holds_audio(fSettings.container,
					fSettings.audioCodec)) {
				fSettings.Validate();
				fUpdating = true;
				select_value(fVideo, fSettings.videoCodec);
				select_value(fAudio, fSettings.audioCodec);
				fUpdating = false;
			}
			fSettings.maxHeight = field_value(fSize, 0);
			fSettings.quality = fQuality->Value();
			fSettings.audioBitrate = field_value(fAudioBitrate,
				fSettings.audioBitrate);
			fSettings.subtitles = (subtitle_mode)field_value(fSubtitles,
				fSettings.subtitles);

			if (fSettings.videoCodec != old.videoCodec) {
				fUpdating = true;
				int32 encoder = field_value(fEncoder, 0);
				_BuildEncoderMenu(fSettings.videoCodec);
				select_value(fEncoder, encoder);
				fUpdating = false;
			}
			int32 encoder = field_value(fEncoder, 0);
			if (encoder < 0) {
				fSettings.encoderPreference = kPreferSoftware;
				fSettings.gpu = "";
			} else {
				fSettings.encoderPreference = kPreferHardware;
				BMenuItem* item = fEncoder->Menu()->FindMarked();
				const char* gpu = NULL;
				if (encoder > 0 && item != NULL && item->Message() != NULL
					&& item->Message()->FindString("gpu", &gpu) == B_OK)
					fSettings.gpu = gpu;
				else
					fSettings.gpu = "";
			}
			fSettings.speed = (speed_preset)field_value(fSpeed,
				fSettings.speed);
			fSettings.rateMode = fAverageBitrate->Value() == B_CONTROL_ON
				? kRateAverageBitrate : kRateConstantQuality;
			int bitrate = atoi(fBitrate->Text());
			if (bitrate >= 100)
				fSettings.videoBitrate = bitrate;
			fSettings.frameRate = field_value(fFrameRate, 0);
			fSettings.peakFrameRate = fConstantRate->Value() != B_CONTROL_ON;

			fSettings.deinterlace = (deinterlace_mode)field_value(
				fDeinterlace, fSettings.deinterlace);
			fSettings.denoise = (filter_strength)field_value(fDenoise,
				fSettings.denoise);
			fSettings.sharpen = (filter_strength)field_value(fSharpen,
				fSettings.sharpen);
			fSettings.rotation = field_value(fRotate, 0);
			fSettings.grayscale = fGrayscale->Value() == B_CONTROL_ON;
			fSettings.autoCrop = fAutoCrop->Value() == B_CONTROL_ON;
			fSettings.flip = fFlip->Value() == B_CONTROL_ON;

			fSettings.mixdown = (audio_mixdown)field_value(fMixdown,
				fSettings.mixdown);
			fSettings.sampleRate = field_value(fSampleRate, 0);
			fSettings.audioTracks = (audio_track_selection)field_value(fTracks,
				fSettings.audioTracks);
			fSettings.passthroughWhenPossible
				= fPassthrough->Value() == B_CONTROL_ON;
			fSettings.copyChapters = fChapters->Value() == B_CONTROL_ON;
			fSettings.copyMetadata = fMetadata->Value() == B_CONTROL_ON;

			fSettings.start = parse_time(fStart->Text());
			fSettings.end = parse_time(fEnd->Text());

			_UpdateEnabling();
			UpdateHardwareStatus();
			if (!(old == fSettings))
				_Changed();
			break;
		}

		default:
			BView::MessageReceived(message);
			break;
	}
}
