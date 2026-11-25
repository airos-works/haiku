# Sheets: dialogs attached to their window

On Haiku an alert or a file panel is a window of its own. It opens in the
middle of the screen (or near the window that asked for it), and once the user
clicks into another window it can disappear behind the rest. In air/OS a dialog
that a window opens is a *sheet* of that window, as on macOS:

- it is attached below the window's title bar, centered, and slides down from
  under it (about a quarter of a second, easing out) and back up when it
  closes;
- the window's content is dimmed while the sheet is up, fading in as the sheet
  comes out and back as it goes;
- it is modal to that window only: the rest of the application, and every other
  application, go on as usual;
- it goes wherever the window goes: moved, resized, minimized, onto another
  workspace. A click into the dimmed window brings both to the front and gives
  the sheet the focus.

Since branch `window-sheets` (2026-11-25).

## Which dialogs become sheets

Nothing has to change in an application:

- `BAlert::Go()` and `BAlert::Go(BInvoker*)`, called from a window's thread
  (the usual "Save changes?" from `QuitRequested()` or `MessageReceived()`),
  show the alert as a sheet of that window. Alerts that are not
  `B_MODAL_APP_WINDOW_FEEL` (the default) or `B_MODAL_SUBSET_WINDOW_FEEL`
  stay windows of their own: a `B_MODAL_ALL_WINDOW_FEEL` alert is for every
  application, a floating one is not modal at all.
- `BFilePanel::Show()`, called from a window's thread, shows the open or save
  panel as a sheet of that window. Shown again from another window it moves
  to that one; shown from the application's thread (no window), it is a
  window of its own again, with the look, feel and position it had.
- The window must be an ordinary one the user works in: `B_NORMAL_WINDOW_FEEL`,
  a titled or document look, shown, not minimized, and not a sheet itself.
  A dialog opened by a sheet (the save panel's "Replace the file?") is a
  window of its own, above the sheet. A window has one sheet at a time; a
  second dialog while one is up is a window of its own.

An application that wants a dialog of its own calls
`BAlert::SetSheetEnabled(false)` or `BFilePanel::SetSheetEnabled(false)`
first (a panel that should stay open beside the window, say). Any other window
can be made a sheet with `BWindow::SetSheetParent()`.

## API

```cpp
class BWindow {
	status_t	SetSheetParent(BWindow* parent);
	BWindow*	SheetParent() const;
	bool		IsSheet() const;
};

class BAlert : public BWindow {
	void		SetSheetEnabled(bool enabled);	// default true
	bool		IsSheetEnabled() const;
};

class BFilePanel {
	void		SetSheetEnabled(bool enabled);	// default true
	bool		IsSheetEnabled() const;
};
```

`SetSheetParent(parent)` makes the window a sheet of `parent`. Call it before
`Show()` or while the window is hidden; a window that is shown already moves
into place at once, without sliding. The window becomes
`B_MODAL_SUBSET_WINDOW_FEEL` with `parent` as its subset, `B_MODAL_WINDOW_LOOK`
(no tab), and `B_NOT_MOVABLE`, `B_NOT_CLOSABLE`, `B_NOT_ZOOMABLE`,
`B_NOT_MINIMIZABLE`. `SetSheetParent(NULL)` makes it an ordinary window again:
the look, feel and flags it had, and its earlier position if it is hidden.

It fails with `B_BAD_VALUE` when `parent` is the window itself, not a
`B_NORMAL_WINDOW_FEEL` window, a sheet itself, an offscreen window or one of
another application, and with `B_BUSY` when `parent` shows another sheet (a
hidden sheet gives way).

A sheet's position belongs to its parent: `MoveTo()` and `MoveBy()` on it
fail (`B_NOT_ALLOWED` from app_server, so `Frame()` does not change), and the
user cannot drag it. It can be resized; it stays in the middle of its parent.
`Show()` and `Hide()` slide it; `Quit()` hides it first, as for any window.

`Hide()` returns at once, while the sheet slides back in. When the parent's
own thread hides it (a synchronous `BAlert::Go()` returning, or a window
quitting its sheet), `Hide()` waits for the slide and has the parent draw what
comes out from under the sheet meanwhile, as the parent's thread is busy.

## How app_server does it

`Desktop::SetSheetParent()` links the two `Window`s (`Window::Sheet()`,
`SheetParent()`); `AS_SET_SHEET_PARENT` carries the request.

- **Clipping.** `Window::SetClipping()` cuts a sheet's visible region off at
  the top of its parent's content, so the part above is never shown. Placed
  all out (`Desktop::_PlaceSheet()`, position 1), the sheet's content starts
  at its parent's content top and its own top border is under that line; at
  position 0 all of it is above, under the parent's tab.
- **The slide.** `ShowWindow()` places a sheet at 0 and starts the slide; the
  sheet's `ServerWindow` takes the steps (`AS_SHEET_ANIMATION_STEP`, a delayed
  message to itself, once a frame). Each step moves the sheet by as much as
  the time since the last one asks for, on an ease-out cubic (250 ms out,
  200 ms back). Coming out, every step uncovers a strip of the sheet that its
  client has to draw; the next step waits until it has (up to 60 ms), so the
  content comes down with the sheet rather than as a blank band. Going in, the
  sheet's pixels are only moved, and the parent draws what comes out from
  under it. `ServerWindow::_Hide()` starts the slide back, and the last step
  hides the window. A window deleted mid-slide finishes it first, in its
  thread (`ServerWindow::_FinishSheetAnimation()`).
- **Moving together.** `Desktop::MoveWindowBy()` moves a parent and its sheet
  in one step, copied as one, so the sheet does not lag or leave a band of the
  parent to redraw. A sheet's own moves are refused. `ResizeWindowBy()`
  centers the sheet again after either changes size. Hiding or minimizing the
  parent hides the sheet (`IsSheetHiddenWithParent()`); it comes back, out
  and dimming, with the parent.
- **Dimming.** app_server has no compositor: windows draw into the back buffer
  directly. The parent's `Window::ContentDim()` (of 256; 166 when the sheet is
  out, 35 % darker) is applied where its pixels come from:
  - `Window::EndUpdate()` multiplies what the update drew by it before the
    update reaches the screen (`DrawingEngine::ScaleRegionColors()`, a lookup
    table per channel). An update draws all of its region anew, so every
    pixel is dimmed once; the window goes on drawing while it is dimmed
    (a clock, a progress bar, a video in an update).
  - The background app_server fills in at once where the window is exposed
    is dimmed the same way (`_TriggerContentRedraw()`).
  - Drawing outside an update (into a dimmed window) is dropped: it would show
    undimmed until the next update. Haiku views mostly invalidate and draw in
    `Draw()`; one that draws directly appears again with the next update.
  - Changing the dim factor rescales what is on the screen by the ratio
    (`Window::SetContentDim()`), which is how it fades in and out with the
    slide. The steps round, so once the sheet is out (and once it is gone)
    the parent draws once more, exactly dimmed (or not).
  - Moving and scrolling copy pixels, dimmed already.
  - A `BDirectWindow` or a `BGLView` that presents directly into the frame
    buffer behind a sheet is not dimmed.
- The title bar and border of the parent are not dimmed: the window can still
  be dragged by its tab, and its close button still sends `B_QUIT_REQUESTED`.

## Testing

`tmp/sheets-vm` (not in the repository): a copy of an x86_64 test VM image
with this branch's `haiku.hpkg` swapped in, and `SheetTest`, a document window
with an animated view and buttons for a synchronous alert, an asynchronous
one, a save panel, an open panel and an alert with sheets turned off.
