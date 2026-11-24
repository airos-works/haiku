# Mobile Kit

Mobile Kit lets one running Be application present a touch layout in air/OS Mobile and its usual layout on the desktop. Include `<MobileKit.h>` and link with `libbe`. The kit uses native controls, fonts, layout coordinates and interface colors. The active presentation follows MobileShell, including when it is started manually. A folded machine configured to keep its desktop continues to use the desktop layout.

## Declare support in the executable

Put this resource alongside the application's signature and launch flags in its resource definition.

```rdef
resource(1, "Mobile Kit") #'MOBV' $"01";
```

A packaging tool can make the same declaration with `BMobileKit::DeclareSupport(resources)` on a writable `BResources` and call `resources.Sync()`. Declare support when building the executable. Package files are read-only once installed. `BMobileKit::SupportsMobileView(ref)` reads the versioned resource even when the program has never run. The launcher checks it while scanning the application and preferences directories and again before launch.

## Adapt a window

Build the ordinary window at its usual desktop frame. At the end of the primary window constructor, after its controls and model exist and before the first Show(), call

```cpp
BMobileKit::PrepareWindow(this, [this](bool mobile) {
    SetMobileView(mobile);
});
```

In a running shell this registers the desktop frame and look for restoration, calls the supplied function on the window's thread, then fits the borderless window between the system panels. On the desktop the constructor keeps the layout it built. Dialogs can use ordinary modal or subset windows.

Handle mode changes on the same window looper. A window can receive the same mode more than once, including after the shell recovers.

```cpp
void
ReaderWindow::MessageReceived(BMessage* message)
{
    if (message->what == BMobileKit::B_MOBILE_VIEW_CHANGED) {
        SetMobileView(message->GetBool("mobile", false));
        return;
    }
    BWindow::MessageReceived(message);
}

void
ReaderWindow::SetMobileView(bool mobile)
{
    if (mobile == fMobile)
        return;
    fMobile = mobile;
    fChrome->CardLayout()->SetVisibleItem(mobile ? 1 : 0);
}
```

In this example `fMobile` starts false and `fChrome` contains two toolbars above a shared document view. Keep the browser tabs, playback engine, selection and document in the same objects across transitions. Keep the last desktop geometry for preferences saved while mobile. Let the shell update the frame when the keyboard opens or the work area changes. Configure size limits and layout maxima so the main view can fill that area.

A primary window with `B_FLOATING_ALL_WINDOW_FEEL`, such as a full-screen movie player, can identify itself by replying to `B_MOBILE_WINDOW_QUERY` with a `B_REPLY` containing boolean `"primary"` set to true. Return it to normal feel while mobile so the home screen can minimize it.

## Build the touch layout

| Element | API and behavior |
| --- | --- |
| App bar, bottom navigation, media transport | Use a `BMobileKit::Bar` and its `AddAction(name, label, message)`. Put an address field, search field or progress view in the bar with `GroupLayout()->AddView()`. |
| Drill-down and back | `NavigationView::Push(page)` displays a new page and keeps earlier views alive. `Back()` removes the top page and returns false at the root. Wire a Back action to it. Use a `BCardView` to keep peer destinations alive when switching tabs. |
| Buttons, search, forms and settings | `Action`, `TextField`, `Toggle` and `Slider` create native Be controls and take ordinary BMessages. A text field participates in the shell keyboard and edit menu. `TouchTarget(control)` sizes another native control for touch. |
| Lists, grids and scrolling | Use `ListItem` in a BListView for font-scaled rows. `Scroll(name, content)` provides native vertical scrolling. Lay out image cards with BGridLayout and use the same touch target dimensions for hit testing. |
| Pickers and overflow menus | Construct `ActionSheet(owner, title)`, call `AddAction(label, message)` for each choice, then `Open()`. The scrollable sheet accepts taps and keyboard invocation and sends the original message to its owner. Cancel and Escape dismiss it. Keep a BMessenger to dismiss transient UI when switching presentations. |
| Dialogs, feedback and progress | Use BAlert or a modal subset BWindow for confirmation, BStatusBar for determinate work and BStringView for status. Set custom surfaces to named interface colors so Appearance changes apply. |

A newly allocated control or page belongs to its parent once added. An action control owns the BMessage pointer supplied to it. A sheet copies each action message. `TargetSize()` gives custom controls the same minimum tap size as the kit's controls. Text, focus, keyboard shortcuts and control messages follow Interface Kit conventions.

The system Open, Save and folder-selection panels adapt automatically. Use the ordinary `BFilePanel` API with the same file filters, node flavors and result messages as on the desktop. In the tablet grid, tap a folder to enter it or use Up to go to its parent. For a directory-only panel, Open chooses the current folder when nothing is selected.

## Skeuomorphic materials

`B_MOBILE_KIT_VERSION` 2 adds an optional look modelled on objects, after the iPad's iOS 5 media apps: brushed metal bars, an inset LCD, glossy segmented buttons, a pill search field, wooden rails and a paper page. Everything is drawn by the kit (vector glyphs, gradients, a generated wood grain), so it needs no images, fonts or resources and scales with the system font. The materials follow the panel colour: aluminium and cherry on a light panel, graphite and walnut on a dark one. Amp's tablet view is the reference application.

| Element | API and behavior |
| --- | --- |
| Bars | `MetalBar(name, B_MOBILE_TOP_BAR or B_MOBILE_BOTTOM_BAR)` is a `Bar` in brushed metal with the dark edge towards the content. The controls below paint the bar's metal behind themselves (`FillBarBehind`), so a bar and its controls read as one plate. Use nested `BLayoutBuilder` groups, not plain group views, to arrange controls inside it. |
| Tabs and transport | `SegmentedControl(name, message, momentary)` joins segments with labels or glyphs. As a selector, `Value()` is the selected index and a tap invokes the message with `"index"`; `Select(index)` changes the selection without invoking. A momentary control keeps no selection and sends each segment's own message (`AddSegment(NULL, B_GLYPH_NEXT, new BMessage(...))`). `SetSegmentGlyph` swaps Play for Pause. |
| Buttons | `MetalButton(name, label, message, shape, glyph)` with `B_ROUNDED_BUTTON`, `B_PILL_BUTTON` or `B_BACK_BUTTON` (pointed on the left, labelled with the page it returns to). |
| Media display | `NowPlayingDisplay` shows the cover, title between artist and album, elapsed and remaining time, a scrubber with a playhead, and repeat and shuffle. Feed it `SetArtwork` (copied), `SetText`, `SetProgress(position, duration)` in microseconds, `SetRepeat(0, 1 or 2)` and `SetShuffle`. `SetMessage(part, message)` reports a finished scrub (`"fraction"`, `"position"`), and taps on the cover, repeat and shuffle. A finger dragged along the scrubber in tablet mode arrives as touch scrolling (`"be:wheel_pixel_delta_x"`, never a press); the playhead follows it and the scrub is reported when the finger lifts. `SetIdleText` names the app while nothing plays. A subclass can give the scrubber its own look by overriding `DrawScrubber(track, fraction)`. |
| Level | `MetalSlider` is a volume slider from 0 to 1000 with speaker glyphs. Its modification message follows the knob, its message is sent on release, and `IsTracking()` tells a player not to move it under the finger. A finger dragged along it in tablet mode arrives as touch scrolling, not as a press: the knob follows the finger's pixels from where it was and the message goes when the scroll phase ends. A mouse wheel moves it a step a notch. |
| Search | `SearchField(name, placeholder, message)` sends the message with `"text"` as the text changes and again on Enter. `SetText` does not send it. The inner BTextView brings up the shell keyboard. |
| Frame and page | `WoodFrame(name, content)` holds one view between wooden rails; `SetRails(false)` makes it a plain container, so the desktop layout can keep it. Call `ColorsChanged()` on `B_COLORS_UPDATED`. `DrawPage(view, view->Bounds(), update)` paints the paper behind a grid, lit from above, and `CurrentSkin().pageText` and `pageSecondary` colour the captions on it. |
| Covers | `DrawReflection(view, bitmap, rect, depth, floor)` mirrors a picture onto the floor below it, fading out, for a cover on a dark stage (`CurrentSkin().stage`). |
| Glyphs and colours | `DrawGlyph(view, glyph, rect, color)` draws transport, repeat, shuffle, back, search, clear, speaker and note symbols. `CurrentSkin()` returns the material colours for custom views, and `ControlHeight()` the visible height of a bar control inside its `TargetSize()` touch area. |

## Browser and list controls (version 3)

`B_MOBILE_KIT_VERSION` 3 adds what a browser, a mail client or a reader needs on top of the materials, after Safari on the early iPhone and iPad: toolbar glyph buttons with a count, a row of tabs, a grid of page cards, touch tables with an editing mode, a share sheet and a confirmation toast. Summit's mobile view is the reference application (its `docs/mobile.md`).

| Element | API and behavior |
| --- | --- |
| Window skin | `SetWindowSkin(window, B_GRAPHITE_SKIN)` (or `B_ALUMINIUM_SKIN`, `B_PANEL_SKIN`) gives one window's kit views another material set, for example a private browsing window. Call it with the window locked or before it is shown; the change applies on the window's thread before its next update. `IsDarkSkin()` tells a custom view which set it is drawing with. |
| Glyphs | Browser and list symbols: `B_GLYPH_ARROW_BACK` and `B_GLYPH_ARROW_FORWARD`, `RELOAD`, `STOP`, `ADD`, `PAGES` (open tabs), `BOOK` (bookmarks), `ACTION` (share), `CLOCK` (history), `FOLDER`, `STAR`, `GLASSES` (reading list), `PRIVATE`, `CHEVRON`, `CHECK`, `GLOBE` (a page without an icon), `MORE`, `LOCK` and `DOWNLOAD`. `PagesGlyphFace(rect)` is the front page of `B_GLYPH_PAGES`, where a count fits. |
| Toolbar buttons | `MetalButton` with `B_PLAIN_BUTTON` is a glyph cut into the bar with no plate, which glows while pressed. `SetGlyphText("3")` writes a short count inside the glyph (the tab count on `B_GLYPH_PAGES`); `SetHighlighted(true)` keeps the glow and tints the glyph for a mode that is on. |
| Progress | `MetalBar::SetProgress(fraction)` draws a blue loading line along the bar's edge towards the content; below 0 (or 1) hides it. |
| Titles | `TitleView(name, title)` is a page title engraved into a bar, between its buttons. |
| Tables | `TableView` (a `BListView` for `Scroll()`) holds `TableRow`s: a bitmap (copied) or glyph picture, a bold title, an optional detail line and a `B_ROW_CHEVRON` or `B_ROW_CHECK` accessory. A tap highlights the row blue and sends its message with `"index"` to the table's target (or the window). `SetHeader(true)` makes a section heading. `SetEditing(true)` gives each row with a delete message a red minus; tapping it shows a red **Delete** button that sends that message. `SetEmptyText` is shown while the table is empty. |
| Card grid | `CardGrid(name, open, close)` lays out cards of open pages: a strip with an icon, a title and ✕ over a picture of the page (both bitmaps copied), two columns on a phone and more on wider screens. A tap opens a card, ✕ closes it, and a finger swiping a card sideways (touch scrolling) slides it off to close it. Messages carry `"id"` and `"index"`. `SetSelected(id)` rings the page on show, `SetAspect(height / width)` matches the pictures, `SetEmptyText(text, glyph)` explains an empty grid (first line as the heading). Without a scroll bar the grid scrolls itself. |
| Tab row | `TabBar(name, select, close)` is the iPad's row of tabs under a bar: the tab on show is lit with ✕, the others sit back; tabs share the width down to a touch-sized minimum, then the row scrolls sideways. `AddTab(id, title, icon)`, then `SetSelected(id)`. |
| Share sheet | `ActionSheet` now rises from the bottom of its window as a dark slab: `AddActivity(label, glyph, message)` puts large glyph buttons in rows at the top, `AddAction(label, message, B_ACTION_DESTRUCTIVE)` a red button in the column, and Cancel is set apart below. A tap above the sheet or Escape dismisses it; a long sheet scrolls. Existing `AddAction(label, message)` callers are unchanged. |
| Toast | `ShowToast(window, "Bookmark saved", B_GLYPH_CHECK)` shows a dark square with a glyph and a line in the middle of the window for a moment, without taking the focus. |

The kit is header-only. An application that is built with SDKs older than the air/OS release it targets can carry a copy of `MobileKit.h` and include it by path; `B_MOBILE_KIT_VERSION` tells which version a copy provides.

## Try both presentations

Open a document on the desktop, note its frame, then start MobileShell or fold a convertible configured for air/OS Mobile. Select the running window from the switcher. Go home, launch a second supporting application and open the on-screen keyboard in a text field. Quit MobileShell or unfold the convertible. Both apps should show their desktop controls with their document or playback state intact. A desktop-only application should be absent from the launcher and have a labeled, closeable card in the task switcher.
