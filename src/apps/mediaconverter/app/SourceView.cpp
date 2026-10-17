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
#include <LayoutBuilder.h>
#include <StringView.h>

#include <math.h>
#include <stdio.h>

#include "MediaProbe.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "Source"


/*!	A picture from the source, letterboxed into the view. */
class PreviewView : public BView {
public:
	PreviewView()
		:
		BView("preview", B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE),
		fBitmap(NULL)
	{
		float width = be_control_look->DefaultLabelSpacing() * 40;
		SetExplicitMinSize(BSize(width, width * 9 / 16));
		SetExplicitMaxSize(BSize(width * 1.5f, width * 1.5f * 9 / 16));
		SetViewColor(B_TRANSPARENT_COLOR);
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
			const char* text = B_TRANSLATE("No picture");
			float width = StringWidth(text);
			font_height height;
			GetFontHeight(&height);
			DrawString(text, BPoint((bounds.Width() - width) / 2,
				(bounds.Height() + height.ascent) / 2));
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
	fItemID(-1)
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

	fTracks = new BColumnListView("tracks", B_NAVIGABLE, B_FANCY_BORDER, true);
	float spacing = be_control_look->DefaultLabelSpacing();
	fTracks->AddColumn(new BStringColumn(B_TRANSLATE("Track"), spacing * 9,
		spacing * 5, spacing * 20, B_TRUNCATE_END), 0);
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
				.AddGlue()
			.End()
		.End()
		.Add(fTracks, 1);

	SetSource(NULL, -1);
}


SourceView::~SourceView()
{
}


void
SourceView::SetSource(const SourceInfo* info, int32 itemID)
{
	fItemID = itemID;
	fTracks->Clear();
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
	summary.SetToFormat("%s · %s · %s", info->containerName.String(),
		MediaProbe::FormatDuration(info->duration).String(),
		MediaProbe::FormatSize(info->fileSize).String());
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
