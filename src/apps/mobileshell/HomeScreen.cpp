/*
 * Copyright 2026, air/OS.
 * Copyright 2026 Renku Mobile contributors.
 * Distributed under the terms of the MIT License.
 */


#include "HomeScreen.h"

#include "AppList.h"
#include "Glyphs.h"
#include "SystemStatus.h"
#include "Theme.h"

#include <Application.h>
#include <Bitmap.h>
#include <FindDirectory.h>
#include <GradientLinear.h>
#include <GradientRadial.h>
#include <MessageRunner.h>
#include <Node.h>
#include <Path.h>
#include <Region.h>
#include <Shape.h>
#include <TranslationUtils.h>

#include <be_apps/Tracker/Background.h>

#include <WindowPrivate.h>

#include <fs_attr.h>

#include <algorithm>
#include <math.h>
#include <time.h>


using Theme::S;
using Theme::F;


// Fingers wobble more than a mouse when they tap.
#define kTapSlop S(20)
#define kDrawerSwipe S(70)
// Slower than this when the finger lifts, the drawer stops with it; a flick
// then loses speed like this every second.
#define kFlingMinVelocity S(300)
static const float kFlingFriction = 0.06f;
static const bigtime_t kFlingInterval = 16000;


static rgb_color
with_alpha(rgb_color color, uint8 alpha)
{
	color.alpha = alpha;
	return color;
}


HomeView::HomeView(BRect frame, AppList* apps)
	:
	BView(frame, "home", B_FOLLOW_ALL, B_WILL_DRAW | B_FRAME_EVENTS),
	fApps(apps),
	fWallpaper(NULL),
	fBuffer(NULL),
	fBufferView(NULL),
	fClockTop(0),
	fCompactClock(false),
	fDrawerOpen(false),
	fSettingsOnly(false),
	fScroll(0),
	fScrollAtDown(0),
	fTracking(false),
	fMoved(false),
	fPressedCell(NULL),
	fVelocity(0),
	fLastTime(0),
	fFling(NULL),
	fFlingTime(0)
{
	SetViewColor(B_TRANSPARENT_COLOR);
}


HomeView::~HomeView()
{
	delete fFling;
	delete fWallpaper;
	delete fBuffer;
}


void
HomeView::AttachedToWindow()
{
	FrameResized(Bounds().Width(), Bounds().Height());
}


void
HomeView::FrameResized(float width, float height)
{
	fPressedCell = NULL;
	fTracking = false;
	_StopFling();

	delete fBuffer;
	fBuffer = new BBitmap(Bounds(), B_BITMAP_ACCEPTS_VIEWS, B_RGB32);
	fBufferView = new BView(Bounds(), "buffer", B_FOLLOW_NONE, 0);
	fBuffer->AddChild(fBufferView);

	_MakeWallpaper();
	_Layout();
	_Render();
}


void
HomeView::Draw(BRect updateRect)
{
	if (fBuffer != NULL)
		DrawBitmap(fBuffer, updateRect, updateRect);
}


void
HomeView::MouseDown(BPoint where)
{
	// A touch stops a flick, and only that.
	bool flinging = fFling != NULL;
	_StopFling();

	fTracking = true;
	fMoved = flinging;
	fDownPoint = where;
	fScrollAtDown = fScroll;
	fVelocity = 0;
	fLastPoint = where;
	fLastTime = system_time();
	fPressedCell = flinging ? NULL : _CellAt(where);
	SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
	if (fPressedCell != NULL)
		_Render();
}


void
HomeView::MouseMoved(BPoint where, uint32 transit, const BMessage* dragMessage)
{
	if (!fTracking)
		return;

	// Smooth the speed over the last few moves.
	bigtime_t now = system_time();
	if (now > fLastTime) {
		float velocity = (where.y - fLastPoint.y) * 1000000 / (now - fLastTime);
		fVelocity = fVelocity * 0.4f + velocity * 0.6f;
	}
	fLastPoint = where;
	fLastTime = now;

	float dy = where.y - fDownPoint.y;
	if (!fMoved && fabsf(dy) < kTapSlop && fabsf(where.x - fDownPoint.x) < kTapSlop)
		return;

	if (!fMoved) {
		fMoved = true;
		fPressedCell = NULL;
	}

	if (!fDrawerOpen) {
		if (dy < -kDrawerSwipe) {
			fTracking = false;
			_SetDrawer(true);
		}
		return;
	}

	// In the drawer a drag scrolls; pulling down from the top closes it.
	if (fScrollAtDown <= 0 && dy > kDrawerSwipe) {
		fTracking = false;
		_SetDrawer(false);
		return;
	}
	float scroll = std::clamp(fScrollAtDown - dy, 0.0f, _MaxScroll());
	if (scroll != fScroll) {
		fScroll = scroll;
		_Render();
	}
}


void
HomeView::MouseUp(BPoint where)
{
	const Cell* pressed = fPressedCell;
	bool tap = fTracking && !fMoved;
	fTracking = false;
	fPressedCell = NULL;

	if (!tap) {
		// A finger that rested before lifting doesn't flick.
		if (fDrawerOpen && system_time() - fLastTime < 100000
			&& fabsf(fVelocity) > kFlingMinVelocity) {
			fFlingTime = system_time();
			BMessage tick(kMsgAnimate);
			fFling = new BMessageRunner(BMessenger(this), &tick,
				kFlingInterval);
		}
		_Render();
		return;
	}

	if (pressed != NULL && pressed == _CellAt(where)) {
		BMessage launch(kMsgLaunch);
		launch.AddRef("ref", &pressed->app->ref);
		be_app->PostMessage(&launch);
		if (fDrawerOpen)
			_SetDrawer(false);
		else
			_Render();
		return;
	}

	if (!fDrawerOpen && fHandle.Contains(where))
		_SetDrawer(true);
	else
		_Render();
}


void
HomeView::MessageReceived(BMessage* message)
{
	if (message->what == kMsgAnimate) {
		_Fling();
		return;
	}
	if (message->what == kMsgPulse) {
		// Redraw when the minute changes.
		static int lastMinute = -1;
		time_t now = time(NULL);
		struct tm local;
		localtime_r(&now, &local);
		if (local.tm_min != lastMinute) {
			lastMinute = local.tm_min;
			if (!fDrawerOpen)
				_Render();
		}
		return;
	}
	BView::MessageReceived(message);
}


void
HomeView::ShowHome()
{
	if (fDrawerOpen)
		_SetDrawer(false);
}


void
HomeView::ShowSettings()
{
	_SetDrawer(true, true);
}


/*!	The picture the desktop of this workspace shows, as Tracker keeps it
	on the desktop folder, or NULL when it has none (or none that can be
	read).
*/
BBitmap*
HomeView::_DesktopPicture() const
{
	BPath path;
	if (find_directory(B_DESKTOP_DIRECTORY, &path) != B_OK)
		return NULL;
	BNode desktop(path.Path());
	attr_info info;
	if (desktop.GetAttrInfo(B_BACKGROUND_INFO, &info) != B_OK
		|| info.size <= 0 || info.size > 64 * 1024) {
		return NULL;
	}
	char* buffer = new char[info.size];
	BMessage settings;
	bool read = desktop.ReadAttr(B_BACKGROUND_INFO, info.type, 0, buffer,
			info.size) == info.size
		&& settings.Unflatten(buffer) == B_OK;
	delete[] buffer;
	if (!read)
		return NULL;

	// the picture of this workspace; else the first one there is
	uint32 workspace = 1 << current_workspace();
	const char* picture = NULL;
	const char* candidate;
	for (int32 i = 0; settings.FindString(B_BACKGROUND_IMAGE, i, &candidate)
			== B_OK; i++) {
		if (candidate[0] == '\0')
			continue;
		uint32 workspaces = 0;
		settings.FindInt32(B_BACKGROUND_WORKSPACES, i, (int32*)&workspaces);
		if (picture == NULL || (workspaces & workspace) != 0)
			picture = candidate;
		if ((workspaces & workspace) != 0)
			break;
	}
	if (picture == NULL)
		return NULL;
	return BTranslationUtils::GetBitmap(picture);
}


void
HomeView::_MakeWallpaper()
{
	delete fWallpaper;
	fWallpaper = NULL;
	BRect bounds = Bounds();

	fWallpaper = new BBitmap(bounds, B_BITMAP_ACCEPTS_VIEWS, B_RGB32);
	BView* view = new BView(bounds, "wallpaper", B_FOLLOW_NONE, 0);
	fWallpaper->AddChild(view);
	fWallpaper->Lock();

	if (BBitmap* picture = _DesktopPicture()) {
		// scaled to fill the screen, the middle kept
		BRect source = picture->Bounds();
		float scale = std::max((bounds.Width() + 1) / (source.Width() + 1),
			(bounds.Height() + 1) / (source.Height() + 1));
		float width = (bounds.Width() + 1) / scale;
		float height = (bounds.Height() + 1) / scale;
		source.left = floorf((source.Width() + 1 - width) / 2);
		source.top = floorf((source.Height() + 1 - height) / 2);
		source.right = source.left + width - 1;
		source.bottom = source.top + height - 1;
		view->DrawBitmap(picture, source, bounds, B_FILTER_BITMAP_BILINEAR);
		delete picture;

		// a little darker towards the bottom, under the labels and the dock
		view->SetDrawingMode(B_OP_ALPHA);
		view->SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
		BGradientLinear shade(BPoint(0, 0), BPoint(0, bounds.bottom));
		shade.AddColor(make_color(0, 0, 0, 40), 0);
		shade.AddColor(make_color(0, 0, 0, 90), 255);
		view->FillRect(bounds, shade);
	} else {
		// The air/OS mark's own scene: sky above waves, a low sun.
		BGradientLinear sky(BPoint(0, 0), BPoint(0, bounds.bottom));
		sky.AddColor(make_color(26, 38, 46), 0);
		sky.AddColor(make_color(47, 82, 92), 130);
		sky.AddColor(make_color(120, 172, 190), 255);
		view->FillRect(bounds, sky);

		view->SetDrawingMode(B_OP_ALPHA);
		view->SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);

		BGradientRadial glow(BPoint(bounds.Width() * 0.7f,
			bounds.Height() * 0.5f), std::max(bounds.Width(), bounds.Height())
				* 0.5f);
		glow.AddColor(make_color(227, 162, 91, 120), 0);
		glow.AddColor(make_color(227, 162, 91, 0), 255);
		view->FillRect(bounds, glow);

		struct Ridge { float base, amplitude, phase; rgb_color color; };
		const Ridge ridges[] = {
			{0.62f, 0.07f, 0.4f, make_color(78, 141, 138, 90)},
			{0.72f, 0.06f, 2.1f, make_color(58, 108, 110, 140)},
			{0.82f, 0.05f, 4.0f, make_color(30, 50, 58, 190)}
		};
		for (const Ridge& ridge : ridges) {
			BShape shape;
			shape.MoveTo(BPoint(0, bounds.bottom));
			for (int x = 0; x <= 24; x++) {
				float fx = x / 24.0f;
				float y = ridge.base + ridge.amplitude
					* sinf(fx * 5.2f + ridge.phase)
					* (0.6f + 0.4f * sinf(fx * 11 + ridge.phase * 2));
				shape.LineTo(BPoint(fx * bounds.Width(), y * bounds.Height()));
			}
			shape.LineTo(BPoint(bounds.right, bounds.bottom));
			shape.Close();
			view->SetHighColor(ridge.color);
			view->FillShape(&shape);
		}
	}

	view->Sync();
	fWallpaper->Unlock();
}


float
HomeView::_DrawerTop() const
{
	return Theme::StatusBarHeight() + S(96);
}


void
HomeView::_Layout()
{
	BRect bounds = Bounds();
	int32 columns = Theme::GridColumns(bounds.Width());
	float cellWidth = floorf(bounds.Width() / columns);
	float cellHeight = Theme::CellHeight();
	float bottom = bounds.bottom - Theme::NavBarHeight();

	// The dock: as wide as its applications need on a wide screen, the
	// screen's width on a narrow one.
	const std::vector<AppEntry*>& dock = fApps->Dock();
	size_t dockCells = std::max<size_t>(4, dock.size());
	float dockWidth = std::min(bounds.Width() - 2 * S(24),
		dockCells * S(170));
	float dockLeft = floorf((bounds.Width() - dockWidth) / 2);
	fDock = BRect(dockLeft, bottom - S(148), dockLeft + dockWidth,
		bottom - S(24));
	fHandle = BRect(floorf(bounds.Width() / 2) - S(40), fDock.top - S(64),
		floorf(bounds.Width() / 2) + S(40), fDock.top - S(12));

	// The clock: the time above the date where there's the height for it
	// and two rows of favourites; on a low screen the date stands beside the
	// time.
	fClockTop = Theme::StatusBarHeight() + S(70);
	float gridTop = S(380);
	fCompactClock = fHandle.top - gridTop < 2 * cellHeight;
	if (fCompactClock) {
		fClockTop = Theme::StatusBarHeight() + S(28);
		gridTop = fClockTop + S(120);
	}

	fHomeCells.clear();
	const std::vector<AppEntry*>& favorites = fApps->Favorites();
	int32 rows = std::max((int32)1, (int32)((fHandle.top - gridTop)
		/ cellHeight));
	// a last row that isn't full stands in the middle
	int32 shown = std::min((int32)favorites.size(), rows * columns);
	for (int32 i = 0; i < shown; i++) {
		int32 column = i % columns;
		int32 row = i / columns;
		int32 inRow = std::min(columns, shown - row * columns);
		float left = floorf((bounds.Width() - inRow * cellWidth) / 2);
		BRect frame(0, 0, cellWidth - 1, cellHeight - 1);
		frame.OffsetTo(left + column * cellWidth, gridTop + row * cellHeight);
		fHomeCells.push_back({frame, favorites[i], true});
	}

	float dockCell = fDock.Width() / dockCells;
	float dockOffset = (dockCells - dock.size()) * dockCell / 2;
	for (size_t i = 0; i < dock.size(); i++) {
		BRect frame(fDock.left + dockOffset + i * dockCell, fDock.top,
			fDock.left + dockOffset + (i + 1) * dockCell - 1, fDock.bottom);
		fHomeCells.push_back({frame, dock[i], false});
	}

	fDrawerCells.clear();
	for (AppEntry* app : fApps->Apps()) {
		if (fSettingsOnly && !app->preference)
			continue;
		int32 i = fDrawerCells.size();
		int32 column = i % columns;
		int32 row = i / columns;
		BRect frame(0, 0, cellWidth - 1, cellHeight - 1);
		frame.OffsetTo(column * cellWidth, _DrawerTop() + row * cellHeight);
		fDrawerCells.push_back({frame, app, true});
	}
}


void
HomeView::_Render()
{
	if (fBuffer == NULL || !fBuffer->Lock())
		return;

	BView* view = fBufferView;
	BRect bounds = view->Bounds();
	view->SetDrawingMode(B_OP_COPY);
	view->DrawBitmap(fWallpaper, bounds);

	view->SetDrawingMode(B_OP_ALPHA);
	view->SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);

	if (!fDrawerOpen) {
		_RenderClock(view);

		view->SetHighColor(with_alpha(Theme::kPanel, 110));
		view->FillRoundRect(fDock, S(28), S(28));
		draw_glyph(view, kGlyphChevronUp, fHandle.InsetByCopy(S(14), S(6)),
			with_alpha(Theme::kText, 200));

		for (const Cell& cell : fHomeCells)
			_RenderCell(view, cell, 0);
	} else {
		view->SetHighColor(with_alpha(Theme::kPanel, 232));
		view->FillRect(bounds);

		BRect clip = bounds;
		clip.top = _DrawerTop() - S(12);
		clip.bottom -= Theme::NavBarHeight();
		BRegion region(clip);
		view->ConstrainClippingRegion(&region);
		for (const Cell& cell : fDrawerCells) {
			if (cell.frame.bottom - fScroll >= clip.top
				&& cell.frame.top - fScroll <= clip.bottom) {
				_RenderCell(view, cell, -fScroll);
			}
		}
		view->ConstrainClippingRegion(NULL);

		view->SetFont(be_bold_font);
		view->SetFontSize(F(26));
		view->SetHighColor(Theme::kText);
		view->DrawString(fSettingsOnly ? "Settings" : "Applications",
			BPoint(S(28), Theme::StatusBarHeight() + S(58)));

		// A grabber to pull the drawer closed.
		view->SetHighColor(with_alpha(Theme::kText, 120));
		view->FillRoundRect(BRect(floorf(bounds.Width() / 2) - S(32),
			Theme::StatusBarHeight() + S(12), floorf(bounds.Width() / 2)
				+ S(32), Theme::StatusBarHeight() + S(17)), 3, 3);
	}

	view->Sync();
	fBuffer->Unlock();
	Invalidate();
}


void
HomeView::_RenderCell(BView* view, const Cell& cell, float offset)
{
	BRect frame = cell.frame.OffsetByCopy(0, offset);
	float iconSize = Theme::IconSize();
	BRect icon(0, 0, iconSize - 1, iconSize - 1);
	float iconTop = cell.label ? frame.top + S(18)
		: frame.top + floorf((frame.Height() - iconSize) / 2);
	icon.OffsetTo(frame.left + floorf((frame.Width() - iconSize) / 2), iconTop);

	if (&cell == fPressedCell) {
		view->SetHighColor(with_alpha(Theme::kText, 50));
		view->FillRoundRect(icon.InsetByCopy(-S(12), -S(12)), Theme::Corner(),
			Theme::Corner());
	}

	if (cell.app->icon != NULL)
		view->DrawBitmap(cell.app->icon, icon);
	else {
		view->SetHighColor(Theme::kSurfaceRaised);
		view->FillRoundRect(icon, Theme::Corner(), Theme::Corner());
	}

	if (!cell.label)
		return;

	BFont font(be_plain_font);
	font.SetSize(F(16));
	font.SetFlags(B_FORCE_ANTIALIASING);
	view->SetFont(&font);
	BString label = cell.app->name;
	font.TruncateString(&label, B_TRUNCATE_END, frame.Width() - S(12));
	float width = font.StringWidth(label.String());
	BPoint where(frame.left + floorf((frame.Width() - width) / 2),
		icon.bottom + S(30));
	view->SetHighColor(0, 0, 0, 160);
	view->DrawString(label.String(), where + BPoint(1, 1));
	view->SetHighColor(Theme::kText);
	view->DrawString(label.String(), where);
}


void
HomeView::_RenderClock(BView* view)
{
	BString time = clock_text();
	BString date = date_text();

	BFont font(be_plain_font);
	font.SetSize(F(92));
	font.SetFace(B_LIGHT_FACE);
	// Subpixel antialiasing fringes in colour over the wallpaper.
	font.SetFlags(B_FORCE_ANTIALIASING);
	BFont small(font);
	small.SetSize(F(22));
	small.SetFace(B_REGULAR_FACE);

	float timeWidth = font.StringWidth(time.String());
	float dateWidth = small.StringWidth(date.String());
	font_height height;
	font.GetHeight(&height);

	BPoint timeWhere, dateWhere;
	if (fCompactClock) {
		// side by side, the pair centred
		float total = timeWidth + S(28) + dateWidth;
		timeWhere = BPoint(floorf((Bounds().Width() - total) / 2),
			fClockTop + height.ascent);
		dateWhere = BPoint(timeWhere.x + timeWidth + S(28), timeWhere.y);
	} else {
		timeWhere = BPoint(floorf((Bounds().Width() - timeWidth) / 2),
			fClockTop + height.ascent);
		dateWhere = BPoint(floorf((Bounds().Width() - dateWidth) / 2),
			timeWhere.y + height.descent + S(38));
	}

	view->SetFont(&font);
	view->SetHighColor(0, 0, 0, 90);
	view->DrawString(time.String(), timeWhere + BPoint(2, 2));
	view->SetHighColor(Theme::kText);
	view->DrawString(time.String(), timeWhere);

	view->SetFont(&small);
	view->SetHighColor(0, 0, 0, 90);
	view->DrawString(date.String(), dateWhere + BPoint(1, 1));
	view->SetHighColor(with_alpha(Theme::kText, 230));
	view->DrawString(date.String(), dateWhere);
}


void
HomeView::_SetDrawer(bool open, bool settings)
{
	_StopFling();
	fDrawerOpen = open;
	if (settings != fSettingsOnly) {
		fSettingsOnly = settings;
		_Layout();
	}
	fScroll = 0;
	fPressedCell = NULL;
	_Render();
}


float
HomeView::_MaxScroll() const
{
	if (fDrawerCells.empty())
		return 0;
	float contentBottom = fDrawerCells.back().frame.bottom + S(24);
	float visibleBottom = Bounds().bottom - Theme::NavBarHeight();
	return std::max(0.0f, contentBottom - visibleBottom);
}


const HomeView::Cell*
HomeView::_CellAt(BPoint where) const
{
	if (fDrawerOpen) {
		if (where.y < _DrawerTop() - S(12)
			|| where.y > Bounds().bottom - Theme::NavBarHeight()) {
			return NULL;
		}
		for (const Cell& cell : fDrawerCells) {
			if (cell.frame.OffsetByCopy(0, -fScroll).Contains(where))
				return &cell;
		}
		return NULL;
	}

	for (const Cell& cell : fHomeCells) {
		if (cell.frame.Contains(where))
			return &cell;
	}
	return NULL;
}


void
HomeView::_StopFling()
{
	delete fFling;
	fFling = NULL;
}


void
HomeView::_Fling()
{
	bigtime_t now = system_time();
	float seconds = (now - fFlingTime) / 1000000.0f;
	fFlingTime = now;

	fVelocity *= powf(kFlingFriction, seconds);
	float scroll = std::clamp(fScroll - fVelocity * seconds, 0.0f,
		_MaxScroll());
	bool stopped = scroll == fScroll || fabsf(fVelocity) < S(20);
	fScroll = scroll;
	_Render();
	if (stopped)
		_StopFling();
}


// #pragma mark - HomeWindow


HomeWindow::HomeWindow(BRect frame, AppList* apps)
	:
	BWindow(frame, "Home", B_NO_BORDER_WINDOW_LOOK, kDesktopWindowFeel,
		B_NOT_MOVABLE | B_NOT_RESIZABLE | B_NOT_ZOOMABLE | B_NOT_MINIMIZABLE
			| B_NOT_CLOSABLE | B_ASYNCHRONOUS_CONTROLS,
		B_ALL_WORKSPACES)
{
	fView = new HomeView(Bounds(), apps);
	AddChild(fView);
}


void
HomeWindow::ScreenChanged(BRect frame, color_space mode)
{
	be_app->PostMessage(kMsgScreenChanged);
}
