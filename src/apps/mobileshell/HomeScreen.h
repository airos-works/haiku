/*
 * Copyright 2026, air/OS.
 * Copyright 2026 Renku Mobile contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef HOME_SCREEN_H
#define HOME_SCREEN_H

/*	HomeScreen: the window behind every application. A clock, a grid of
	favourite applications and a dock over the wallpaper (the desktop's own
	picture where it has one); swipe up (or tap the handle) for the drawer
	with every application, swipe down to close it again. The drawer scrolls
	with the finger and keeps going after a flick.
*/

#include <View.h>
#include <Window.h>

#include <vector>


class AppList;
struct AppEntry;
class BBitmap;
class BMessageRunner;


class HomeView : public BView {
public:
								HomeView(BRect frame, AppList* apps);
	virtual						~HomeView();

	virtual	void				AttachedToWindow();
	virtual	void				FrameResized(float width, float height);
	virtual	void				Draw(BRect updateRect);
	virtual	void				MouseDown(BPoint where);
	virtual	void				MouseMoved(BPoint where, uint32 transit,
									const BMessage* dragMessage);
	virtual	void				MouseUp(BPoint where);
	virtual	void				MessageReceived(BMessage* message);

			void				ShowHome();
			void				ShowSettings();

private:
	struct Cell {
		BRect		frame;
		AppEntry*	app;
		bool		label;
	};

			void				_MakeWallpaper();
			BBitmap*			_DesktopPicture() const;
			float				_DrawerTop() const;
			void				_Layout();
			void				_Render();
			void				_RenderCell(BView* view, const Cell& cell,
									float offset);
			void				_RenderClock(BView* view);
			void				_SetDrawer(bool open, bool settings = false);
			float				_MaxScroll() const;
			const Cell*			_CellAt(BPoint where) const;
			void				_StopFling();
			void				_Fling();

			AppList*			fApps;
			BBitmap*			fWallpaper;
			BBitmap*			fBuffer;
			BView*				fBufferView;

			std::vector<Cell>	fHomeCells;
			std::vector<Cell>	fDrawerCells;
			BRect				fDock;
			BRect				fHandle;
			float				fClockTop;
			bool				fCompactClock;	// the date beside the time

			bool				fDrawerOpen;
			bool				fSettingsOnly;
			float				fScroll;
			float				fScrollAtDown;
			bool				fTracking;
			bool				fMoved;
			BPoint				fDownPoint;
			const Cell*			fPressedCell;

			// The finger's speed in pixels per second, and the scrolling it
			// keeps up after a flick
			float				fVelocity;
			BPoint				fLastPoint;
			bigtime_t			fLastTime;
			BMessageRunner*		fFling;
			bigtime_t			fFlingTime;
};


class HomeWindow : public BWindow {
public:
								HomeWindow(BRect frame, AppList* apps);

	virtual	void				ScreenChanged(BRect frame, color_space mode);

			HomeView*			View() const { return fView; }

private:
			HomeView*			fView;
};


#endif	// HOME_SCREEN_H
