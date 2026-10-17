/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "SourceView.h"

#include <Bitmap.h>
#include <Catalog.h>
#include <ColumnListView.h>
#include <ColumnTypes.h>
#include <ControlLook.h>
#include <GroupLayout.h>
#include <LayoutBuilder.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <PopUpMenu.h>
#include <MessageRunner.h>
#include <StringView.h>
#include <Window.h>

#include <math.h>
#include <stdio.h>

#include <algorithm>

#include "AudioPreview.h"
#include "MediaProbe.h"
#include "Messages.h"
#include "airtime/AirTimeMessages.h"
#include "airtime/ControlBar.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "Source"


static const uint32 kMsgPreviewPulse = 'pvpl';
static const bigtime_t kSkipTime = 10 * 1000000LL;


/*!	A picture from the source, letterboxed into the view; for a song its
	album artwork, in a square. */
class PreviewView : public BView {
public:
	PreviewView()
		:
		BView("preview", B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE),
		fBitmap(NULL),
		fSquare(false)
	{
		SetSquare(false);
		SetViewColor(B_TRANSPARENT_COLOR);
	}

	void SetSquare(bool square)
	{
		fSquare = square;
		float width = be_control_look->DefaultLabelSpacing() * 40;
		float height = width * 9 / 16;
		if (square) {
			SetExplicitMinSize(BSize(height, height));
			SetExplicitMaxSize(BSize(height * 1.5f, height * 1.5f));
		} else {
			SetExplicitMinSize(BSize(width, height));
			SetExplicitMaxSize(BSize(width * 1.5f, height * 1.5f));
		}
		InvalidateLayout();
		Invalidate();
	}

	~PreviewView()
	{
		delete fBitmap;
	}

	void SetBitmap(BBitmap* bitmap)
	{
		delete fBitmap;
		fBitmap = bitmap;
		Invalidate();
	}

	virtual void Draw(BRect updateRect)
	{
		BRect bounds = Bounds();
		SetHighColor(20, 20, 22);
		FillRect(bounds);
		if (fBitmap == NULL) {
			SetHighColor(110, 110, 115);
			font_height height;
			GetFontHeight(&height);
			float y = (bounds.Height() + height.ascent) / 2;
			if (fSquare) {
				// a note where the cover would be
				BFont font;
				GetFont(&font);
				BFont big(font);
				big.SetSize(bounds.Height() * 0.4f);
				SetFont(&big);
				const char* note = "♫";
				font_height bigHeight;
				big.GetHeight(&bigHeight);
				DrawString(note, BPoint((bounds.Width()
					- big.StringWidth(note)) / 2,
					bounds.Height() * 0.55f));
				SetFont(&font);
				y = bounds.Height() * 0.55f + bigHeight.descent
					+ height.ascent + 4;
			}
			const char* text = fSquare ? B_TRANSLATE("No artwork")
				: B_TRANSLATE("No picture");
			float width = StringWidth(text);
			DrawString(text, BPoint((bounds.Width() - width) / 2, y));
			return;
		}
		BRect source = fBitmap->Bounds();
		float scale = min_c(bounds.Width() / source.Width(),
			bounds.Height() / source.Height());
		float width = source.Width() * scale;
		float height = source.Height() * scale;
		BRect target(0, 0, width, height);
		target.OffsetTo((bounds.Width() - width) / 2,
			(bounds.Height() - height) / 2);
		DrawBitmap(fBitmap, source, target, B_FILTER_BITMAP_BILINEAR);
	}

private:
	BBitmap*	fBitmap;
	bool		fSquare;
};


BString
format_level(AVCodecID codec, int level)
{
	BString text;
	if (level <= 0)
		return text;
	switch (codec) {
		case AV_CODEC_ID_H264:
			if (level == 9)
				text = "1b";
			else
				text.SetToFormat("%d.%d", level / 10, level % 10);
			break;
		case AV_CODEC_ID_HEVC:
			if (level % 30 == 0)
				text.SetToFormat("%d", level / 30);
			else
				text.SetToFormat("%d.%d", level / 30, level % 30 / 3);
			break;
		case AV_CODEC_ID_AV1:
			text.SetToFormat("%d.%d", 2 + level / 4, level % 4);
			break;
		default:
			text.SetToFormat("%d", level);
			break;
	}
	return text;
}


BString
format_frame_rate(AVRational rate)
{
	BString text;
	if (rate.num <= 0 || rate.den <= 0)
		return text;
	double fps = av_q2d(rate);
	if (fabs(fps - lround(fps)) < 0.001)
		text.SetToFormat("%ld fps", lround(fps));
	else
		text.SetToFormat("%.3f fps", fps);
	return text;
}


static BString
aspect_text(int width, int height, AVRational sampleAspect)
{
	double sar = sampleAspect.num > 0 && sampleAspect.den > 0
		? av_q2d(sampleAspect) : 1.0;
	double display = (double)width * sar / height;
	static const struct {
		double		value;
		const char*	name;
	} kAspects[] = {
		{ 4.0 / 3, "4:3" }, { 16.0 / 9, "16:9" }, { 1.85, "1.85:1" },
		{ 2.0, "2:1" }, { 2.39, "2.39:1" }, { 2.35, "2.35:1" },
		{ 21.0 / 9, "21:9" }, { 1.0, "1:1" }, { 9.0 / 16, "9:16" },
		{ 3.0 / 2, "3:2" }, { 5.0 / 4, "5:4" },
	};
	for (size_t i = 0; i < sizeof(kAspects) / sizeof(kAspects[0]); i++) {
		if (fabs(display - kAspects[i].value) < 0.012)
			return BString(kAspects[i].name);
	}
	BString text;
	text.SetToFormat("%.2f:1", display);
	return text;
}


SourceView::SourceView()
	:
	BView("source", B_WILL_DRAW),
	fControls(NULL),
	fItemID(-1),
	fAudioSource(false),
	fAudioDuration(0),
	fPlayer(NULL),
	fPulse(NULL),
	fVolume(1.0f),
	fMuted(false),
	fScrubbing(false),
	fScrubWasPlaying(false),
	fScanHeld(false),
	fScanWasPlaying(false)
{
	SetViewUIColor(B_PANEL_BACKGROUND_COLOR);

	fPreview = new PreviewView();
	fTitle = new BStringView("title", "");
	BFont bold(be_bold_font);
	bold.SetSize(bold.Size() * 1.2f);
	fTitle->SetFont(&bold);
	fTitle->SetExplicitMinSize(BSize(50, B_SIZE_UNSET));
	fTitle->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
	fSummary = new BStringView("summary", "");
	fSummary->SetExplicitMinSize(BSize(50, B_SIZE_UNSET));
	fSummary->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
	fExtra = new BStringView("extra", "");
	fExtra->SetExplicitMinSize(BSize(50, B_SIZE_UNSET));
	fExtra->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
	// a disc's titles, as HandBrake lists them
	fDiscTitles = new BMenuField("disc titles", B_TRANSLATE("Title:"),
		new BPopUpMenu(""));
	fDiscTitles->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));

	fTracks = new BColumnListView("tracks", B_NAVIGABLE, B_FANCY_BORDER, true);
	float spacing = be_control_look->DefaultLabelSpacing();
	fTracks->AddColumn(new BStringColumn(B_TRANSLATE("Track"),
		be_plain_font->StringWidth(B_TRANSLATE("Subtitles 10")) + spacing * 4,
		spacing * 5, spacing * 30, B_TRUNCATE_END), 0);
	fTracks->AddColumn(new BStringColumn(B_TRANSLATE("Format"), spacing * 22,
		spacing * 8, spacing * 60, B_TRUNCATE_END), 1);
	fTracks->AddColumn(new BStringColumn(B_TRANSLATE("Details"), spacing * 50,
		spacing * 10, spacing * 160, B_TRUNCATE_END), 2);
	fTracks->AddColumn(new BStringColumn(B_TRANSLATE("Language"),
		spacing * 12, spacing * 6, spacing * 40, B_TRUNCATE_END), 3);
	fTracks->SetSortingEnabled(false);
	fTracks->SetExplicitMinSize(BSize(B_SIZE_UNSET, spacing * 26));

	BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_SMALL_SPACING)
		.AddGroup(B_HORIZONTAL, B_USE_DEFAULT_SPACING)
			.Add(fPreview, 0)
			.AddGroup(B_VERTICAL, B_USE_SMALL_SPACING, 1)
				.Add(fTitle)
				.Add(fSummary)
				.Add(fExtra)
				.Add(fDiscTitles)
				.AddGlue()
			.End()
		.End()
		.AddGroup(B_VERTICAL, 0)
			.GetLayout(&fControlsSlot)
		.End()
		.Add(fTracks, 1);

	SetSource(NULL, -1);
}


SourceView::~SourceView()
{
	delete fPulse;
	delete fPlayer;
}


void
SourceView::AttachedToWindow()
{
	BView::AttachedToWindow();
	if (fControls == NULL) {
		// airTime's controller; its messages come here
		float height = airtime::ControlBar::PreferredHeight(false);
		fControls = new airtime::ControlBar(BRect(0, 0, 400, height - 1),
			false, BMessenger(this));
		fControls->SetExplicitMinSize(BSize(
			airtime::ControlBar::MinimumWidth(false), height));
		fControls->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, height));
		fControlsSlot->AddView(fControls);
		fControls->SetVolume(fVolume, fMuted);
		if (!fAudioSource)
			fControls->Hide();
		_UpdateControls();
	}
}


void
SourceView::DetachedFromWindow()
{
	_StopPlaying();
	BView::DetachedFromWindow();
}


void
SourceView::MessageReceived(BMessage* message)
{
	using namespace airtime;
	switch (message->what) {
		case kMsgTogglePlay:
			if (fPlayer != NULL && fPlayer->IsPlaying()) {
				fPlayer->Pause();
				_UpdateControls();
			} else if (_OpenPlayer()) {
				fPlayer->Play();
				_UpdateControls();
			}
			break;

		case kMsgGoToStart:
			_Seek(0);
			break;
		case kMsgGoToEnd:
			_Seek(fAudioDuration);
			break;

		case kMsgScanPress:
			fScanHeld = true;
			fScanWasPlaying = fPlayer != NULL && fPlayer->IsPlaying();
			break;
		case kMsgScanRelease:
		{
			if (!fScanHeld)
				break;
			fScanHeld = false;
			// a click skips ten seconds, holding skips further
			int32 direction = message->GetInt32("direction", 1);
			bigtime_t held = message->GetInt64("held", 0);
			bigtime_t distance = std::max(kSkipTime, held * 4);
			bigtime_t now = fPlayer != NULL ? fPlayer->Position() : 0;
			_Seek(now + direction * distance);
			break;
		}

		case kMsgSeekTo:
		{
			bigtime_t time = message->GetInt64("time", 0);
			bool final = message->GetBool("final", true);
			if (!_OpenPlayer())
				break;
			if (!final && !fScrubbing) {
				fScrubbing = true;
				fScrubWasPlaying = fPlayer->IsPlaying();
				fPlayer->Pause();
			}
			fPlayer->Seek(time);
			if (final && fScrubbing) {
				fScrubbing = false;
				if (fScrubWasPlaying)
					fPlayer->Play();
			}
			_UpdateControls();
			break;
		}

		case kMsgSetVolume:
			fVolume = message->GetFloat("volume", 1.0f);
			fMuted = false;
			if (fPlayer != NULL) {
				fPlayer->SetVolume(fVolume);
				fPlayer->SetMuted(false);
			}
			if (fControls != NULL)
				fControls->SetVolume(fVolume, false);
			break;
		case kMsgToggleMute:
			fMuted = !fMuted;
			if (fPlayer != NULL)
				fPlayer->SetMuted(fMuted);
			if (fControls != NULL)
				fControls->SetVolume(fVolume, fMuted);
			break;

		case kMsgPreviewPulse:
			if (fPlayer != NULL && fPlayer->IsPlaying())
				fPlayer->AtEnd();
			_UpdateControls();
			break;

		case kMsgToggleTimeDisplay:
		case kMsgControlsActivity:
			break;

		default:
			BView::MessageReceived(message);
			break;
	}
}


/*!	The sound card is only taken when the song is played. */
bool
SourceView::_OpenPlayer()
{
	if (!fAudioSource || fAudioPath.IsEmpty())
		return false;
	if (fPlayer != NULL && fPlayer->IsOpen())
		return true;
	if (fPlayer == NULL)
		fPlayer = new AudioPreview();
	BString error;
	if (fPlayer->Open(fAudioPath.String(), &error) != B_OK) {
		fprintf(stderr, "MediaConverter: cannot play %s: %s\n",
			fAudioPath.String(), error.String());
		return false;
	}
	fPlayer->SetVolume(fVolume);
	fPlayer->SetMuted(fMuted);
	if (fPulse == NULL) {
		BMessage pulse(kMsgPreviewPulse);
		fPulse = new BMessageRunner(BMessenger(this), &pulse, 100000);
	}
	return true;
}


void
SourceView::_Seek(bigtime_t time)
{
	if (!_OpenPlayer())
		return;
	time = std::max((bigtime_t)0, std::min(time, fAudioDuration));
	fPlayer->Seek(time);
	_UpdateControls();
}


void
SourceView::_StopPlaying()
{
	delete fPulse;
	fPulse = NULL;
	if (fPlayer != NULL)
		fPlayer->Close();
	_UpdateControls();
}


void
SourceView::_UpdateControls()
{
	if (fControls == NULL)
		return;
	bool open = fPlayer != NULL && fPlayer->IsOpen();
	fControls->SetPosition(open ? fPlayer->Position() : 0, fAudioDuration);
	fControls->SetPlaying(open && fPlayer->IsPlaying());
}


void
SourceView::SetSource(const SourceInfo* info, int32 itemID)
{
	// a song plays with airTime's controller, under its artwork
	bool audio = info != NULL && info->IsSoundOnly();
	if (info == NULL || info->path != fAudioPath || !audio)
		_StopPlaying();
	fAudioSource = audio;
	fAudioPath = audio ? info->path : BString();
	fAudioDuration = audio ? info->duration : 0;
	fPreview->SetSquare(audio);
	if (fControls != NULL) {
		if (audio && fControls->IsHidden())
			fControls->Show();
		else if (!audio && !fControls->IsHidden())
			fControls->Hide();
		if (audio) {
			fControls->SetMedia(true, false, true);
			BString title = info->Metadata("title");
			fControls->SetTitle(title.IsEmpty() ? info->name.String()
				: title.String());
		}
		_UpdateControls();
	}

	fItemID = itemID;
	fTracks->Clear();
	_UpdateDiscTitles(info);
	if (info == NULL) {
		fTitle->SetText(B_TRANSLATE("No source"));
		fSummary->SetText(B_TRANSLATE("Add files with the + button or drop "
			"them on the list."));
		fExtra->SetText("");
		fPreview->SetBitmap(NULL);
		return;
	}

	BString title = info->Metadata("title");
	if (title.IsEmpty())
		title = info->name;
	fTitle->SetText(title.String());

	BString summary;
	if (audio) {
		// a song by its sound rather than its container: "FLAC · 24-bit ·
		// 96 kHz · stereo"
		const AudioTrackInfo& a = info->audio[0];
		summary = a.codecName;
		if (a.bitDepth > 0) {
			BString depth;
			depth.SetToFormat(" · %d-bit", a.bitDepth);
			summary << depth;
		}
		if (a.sampleRate > 0) {
			BString rate;
			rate.SetToFormat(" · %g kHz", a.sampleRate / 1000.0);
			summary << rate;
		}
		summary << " · " << (a.channels == 1 ? B_TRANSLATE("mono")
			: a.channels == 2 ? B_TRANSLATE("stereo")
			: a.layout.IsEmpty() ? B_TRANSLATE("surround") : a.layout.String());
		summary << " · " << MediaProbe::FormatDuration(info->duration)
			<< " · " << MediaProbe::FormatSize(info->fileSize);
	} else {
		summary.SetToFormat("%s · %s · %s", info->containerName.String(),
			MediaProbe::FormatDuration(info->duration).String(),
			MediaProbe::FormatSize(info->fileSize).String());
	}
	BString rate = MediaProbe::FormatBitRate(info->bitRate);
	if (!rate.IsEmpty())
		summary << " · " << rate;
	fSummary->SetText(summary.String());

	BString extra;
	if (!info->chapters.empty()) {
		BString chapters;
		chapters.SetToFormat(B_TRANSLATE("%d chapters"),
			(int)info->chapters.size());
		extra << chapters;
	}
	const char* kKeys[] = { "artist", "album", "date", "genre", NULL };
	for (int i = 0; kKeys[i] != NULL; i++) {
		BString value = info->Metadata(kKeys[i]);
		if (value.IsEmpty())
			continue;
		if (!extra.IsEmpty())
			extra << " · ";
		extra << value;
	}
	if (title != info->name) {
		if (!extra.IsEmpty())
			extra << " · ";
		extra << info->name;
	}
	fExtra->SetText(extra.String());
	_AddTracks(*info);
}


/*!	A disc's titles in the Title menu; choosing one tells the window, which
	reads that title instead. */
void
SourceView::_UpdateDiscTitles(const SourceInfo* info)
{
	BMenu* menu = fDiscTitles->Menu();
	menu->RemoveItems(0, menu->CountItems(), true);
	if (info == NULL || !info->IsDisc() || info->discTitles.empty()) {
		if (!fDiscTitles->IsHidden())
			fDiscTitles->Hide();
		return;
	}
	for (size_t i = 0; i < info->discTitles.size(); i++) {
		const DiscTitle& title = info->discTitles[i];
		BString label;
		label.SetToFormat(B_TRANSLATE("%d · %s · %d chapters"),
			(int)title.number,
			MediaProbe::FormatDuration(title.duration).String(),
			(int)title.chapters);
		if (title.number == info->discMainTitle)
			label << B_TRANSLATE(" · main");
		BMessage* message = new BMessage(kMsgDiscTitleChosen);
		message->AddInt32("id", fItemID);
		message->AddInt32("title", title.number);
		BMenuItem* item = new BMenuItem(label.String(), message);
		item->SetMarked(title.number == info->discTitle);
		menu->AddItem(item);
	}
	if (Window() != NULL)
		menu->SetTargetForItems(Window());
	if (fDiscTitles->IsHidden())
		fDiscTitles->Show();
}


void
SourceView::SetPreview(BBitmap* bitmap, int32 itemID)
{
	if (itemID != fItemID) {
		delete bitmap;
		return;
	}
	fPreview->SetBitmap(bitmap);
}


void
SourceView::_AddTracks(const SourceInfo& info)
{
	int number = 1;
	for (size_t i = 0; i < info.video.size(); i++, number++) {
		const VideoTrackInfo& v = info.video[i];
		BRow* row = new BRow();
		BString track;
		track.SetToFormat(B_TRANSLATE("Video %d"), (int)i + 1);
		BString format = v.codecName;
		if (!v.profile.IsEmpty())
			format << " " << v.profile;
		BString level = format_level(v.codec, v.level);
		if (!level.IsEmpty())
			format << " " << level;
		BString details;
		details.SetToFormat("%d×%d (%s)", v.width, v.height,
			aspect_text(v.width, v.height, v.sampleAspect).String());
		BString rate = format_frame_rate(v.frameRate);
		if (!rate.IsEmpty()) {
			details << " · " << rate;
			if (v.variableFrameRate)
				details << B_TRANSLATE(" (variable)");
		}
		BString depth;
		depth.SetToFormat(" · %d-bit %s", v.bitDepth, v.chroma.String());
		details << depth;
		details << " · " << v.dynamicRange;
		if (!v.colorPrimaries.IsEmpty())
			details << " " << v.colorPrimaries;
		if (v.interlaced)
			details << B_TRANSLATE(" · interlaced");
		if (v.rotation != 0) {
			BString rotation;
			rotation.SetToFormat(B_TRANSLATE(" · turned %d°"), v.rotation);
			details << rotation;
		}
		BString bitrate = MediaProbe::FormatBitRate(v.bitRate);
		if (!bitrate.IsEmpty())
			details << " · " << bitrate;
		row->SetField(new BStringField(track.String()), 0);
		row->SetField(new BStringField(format.String()), 1);
		row->SetField(new BStringField(details.String()), 2);
		row->SetField(new BStringField(v.title.IsEmpty()
			? "" : v.title.String()), 3);
		fTracks->AddRow(row);
	}

	for (size_t i = 0; i < info.audio.size(); i++) {
		const AudioTrackInfo& a = info.audio[i];
		BRow* row = new BRow();
		BString track;
		track.SetToFormat(B_TRANSLATE("Audio %d"), (int)i + 1);
		BString format = a.codecName;
		if (!a.profile.IsEmpty() && a.profile != "LC")
			format << " " << a.profile;
		BString details = a.layout.IsEmpty() ? BString() : a.layout;
		if (a.sampleRate > 0) {
			BString rate;
			rate.SetToFormat("%s%g kHz", details.IsEmpty() ? "" : " · ",
				a.sampleRate / 1000.0);
			details << rate;
		}
		if (a.bitDepth > 0) {
			BString depth;
			depth.SetToFormat(" · %d-bit", a.bitDepth);
			details << depth;
		}
		BString bitrate = MediaProbe::FormatBitRate(a.bitRate);
		if (!bitrate.IsEmpty())
			details << " · " << bitrate;
		if (a.isDefault)
			details << B_TRANSLATE(" · default");
		if (a.commentary)
			details << B_TRANSLATE(" · commentary");
		if (!a.title.IsEmpty())
			details << " · " << a.title;
		row->SetField(new BStringField(track.String()), 0);
		row->SetField(new BStringField(format.String()), 1);
		row->SetField(new BStringField(details.String()), 2);
		row->SetField(new BStringField(
			MediaProbe::LanguageName(a.language).String()), 3);
		fTracks->AddRow(row);
	}

	for (size_t i = 0; i < info.subtitles.size(); i++) {
		const SubtitleTrackInfo& s = info.subtitles[i];
		BRow* row = new BRow();
		BString track;
		track.SetToFormat(B_TRANSLATE("Subtitles %d"), (int)i + 1);
		BString details = s.text ? B_TRANSLATE("Text") : B_TRANSLATE("Pictures");
		if (s.forced)
			details << B_TRANSLATE(" · forced");
		if (s.isDefault)
			details << B_TRANSLATE(" · default");
		if (!s.title.IsEmpty())
			details << " · " << s.title;
		row->SetField(new BStringField(track.String()), 0);
		row->SetField(new BStringField(s.codecName.String()), 1);
		row->SetField(new BStringField(details.String()), 2);
		row->SetField(new BStringField(
			MediaProbe::LanguageName(s.language).String()), 3);
		fTracks->AddRow(row);
	}

	for (size_t i = 0; i < info.chapters.size(); i++) {
		const ChapterInfo& c = info.chapters[i];
		BRow* row = new BRow();
		BString track;
		track.SetToFormat(B_TRANSLATE("Chapter %d"), (int)i + 1);
		BString details;
		details.SetToFormat("%s – %s",
			MediaProbe::FormatDuration(c.start).String(),
			MediaProbe::FormatDuration(c.end).String());
		row->SetField(new BStringField(track.String()), 0);
		row->SetField(new BStringField(c.title.String()), 1);
		row->SetField(new BStringField(details.String()), 2);
		row->SetField(new BStringField(""), 3);
		fTracks->AddRow(row);
	}
}
