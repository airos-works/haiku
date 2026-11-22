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

## Try both presentations

Open a document on the desktop, note its frame, then start MobileShell or fold a convertible configured for air/OS Mobile. Select the running window from the switcher. Go home, launch a second supporting application and open the on-screen keyboard in a text field. Quit MobileShell or unfold the convertible. Both apps should show their desktop controls with their document or playback state intact. A desktop-only application should be absent from the launcher and have a labeled, closeable card in the task switcher.
