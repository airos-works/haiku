# air/OS Mobile (the touch desktop of tablet mode)

A 2-in-1 folded into a tablet has no keyboard and no touchpad within reach,
and the desktop is made for both. air/OS Mobile is a second desktop for that
position: a home screen with large icons, applications that fill the screen,
buttons along the bottom to go home, switch and close, and a keyboard on the
screen. It is one program, `MobileShell`, that takes the desktop over while
it runs and gives it back, as it was, when it quits.

It is experimental: off by default, and only offered on machines that know
when they are a tablet ([TABLET-MODE.md](TABLET-MODE.md)).

## Started by

The tablet daemon starts `MobileShell` when the machine becomes a tablet and
the Screen preferences say that tablet mode switches to the mobile view, and
asks it to quit when the machine is a laptop again.

By hand, for testing on any machine:

```
$ /system/apps/MobileShell &                            # take the desktop over
$ hey application/x-vnd.airOS-MobileShell quit          # give it back
$ /system/apps/MobileShell --restore                    # after a shell that died
```

`MobileShell` is in `/system/apps` without an entry in the Deskbar menu, and
does not list itself among the running applications.

## What is on the screen

| Part | Source | What it does |
|---|---|---|
| Status bar | `Panels.cpp` | Clock, network and battery. Tap it or pull it down for the action drawer. |
| Navigation panel | `Panels.cpp` | Task switcher, home and close buttons; the keyboard's button on the right. Swiping up from it opens the task switcher. |
| Home screen | `HomeScreen.cpp` | Clock, favourite applications and a dock over the desktop's own picture. Swipe up (or tap the chevron) for the drawer with every application and preferences panel; flick to scroll it, pull down to close it. |
| Action drawer | `ActionDrawer.cpp` | Date and time, and quick settings: Network, Keyboard, Screenshot, Settings (the preferences panels). |
| Task switcher | `TaskSwitcher.cpp` | A card for every open window, with a picture of it: tap to switch, ✕ to close. |
| Windows | `WindowTracker.cpp` | Every application window loses its border and fills the screen between the panels (above the keyboard while it is up). Windows that cannot be that size keep theirs, centred, with a line around them. |
| Keyboard | `Keyboard.cpp`, `EmojiPanel.cpp`, `SwipeDecoder.cpp` | Comes up when a text field gets the focus and goes when it loses it. The globe key opens a menu of layouts: English; Terminal, with a row of Esc, Tab, Ctrl, Alt, Home, End and arrows; Corners, with symbols in the corners of the letter keys, typed by flicking a key towards one. Emoji in place of the letters. Swiping across the letters types a word. |

Sizes follow the system's font size, so the shell scales with the rest of
the system (`Theme.h`), and the layout follows the screen: more columns and
the date beside the time on a wide screen, the phone-like arrangement on a
tall one. It lays itself out again when the screen changes.

## Taking the desktop over, and giving it back

app_server has no window manager to plug in, so the shell does what the
Deskbar's window list does: it reads the list of windows a few times a second
and changes windows through their scripting properties (`Look`, `Frame`,
`Hidden`).

When it starts it

- hides the Deskbar's windows and Tracker's desktop window, and shows its own
  home screen in the desktop's place;
- takes every normal window of every application that is not a background
  program: borderless, and sized to the work area.

Before it changes a window it notes what the window looked like (its look, its
frame, whether it was minimized). The notes are kept in memory and in
`/tmp/mobile_shell_state` (`SavedState.cpp`), written whenever they change.

When it is asked to quit (`B_QUIT_REQUESTED`, or `SIGTERM`) it puts every
window back: look and frame as noted, the windows it minimized for the home
screen up again in their order, windows that were minimized before still
minimized; then the Deskbar and the desktop reappear, and the notes are
deleted.

A shell that dies leaves the windows borderless and the Deskbar hidden.
`MobileShell --restore` puts things back from the notes without starting the
shell; a shell started again instead carries on from them, so the original
frames survive a restart of the shell. Notes from before the last boot are
ignored.

## The two input_server add-ons

Both are in every image and do nothing until a running shell asks
(`Protocol.h` has the details).

- **`mobile_keyboard`** (`src/add-ons/input_server/devices/mobile_keyboard`)
  types the keys the on-screen keyboard sends to its port, as a real keyboard
  would, so every application gets them. It takes keys only from the program
  that said hello to it, and only the mobile shell may say hello. It also
  tells that shell when a text field gets or loses the focus, which
  input_server learns from the windows but tells no one else.
- **`mobile_touch`** (`src/add-ons/input_server/filters/mobile_touch`) turns a
  finger held still for half a second into a click of the secondary button
  (context menus), inside the rectangle the shell gives it: the applications'
  windows, not the shell's own panels and keyboard. It only looks at events of
  touch screens and pens, only while the shell that asked is still running, and
  passes everything else through untouched.

One fix this needed is in the system rather than in the shell: the tablet
device add-on now sends the pointer's move before a press that lands
somewhere new. A finger does not hover before it lands, and app_server takes
a press's position from the last move, so presses on a touch screen landed
where the previous touch had ended.

## Limits

- Experimental. The first machine is the Lenovo Yoga 11e; so far it has been
  tried in a virtual machine with a mouse standing in for the finger.
- Windows are opaque on this desktop, which costs the shell what a compositor
  would give it: the drawer and the task switcher dim a still picture of the
  screen rather than the live applications, the task switcher's cards are
  pictures taken when it opens, windows that cannot fill the screen are not
  scaled to fit, and there is no ring around a held finger.
- Windows that cannot fill the screen (fixed-size ones, and ones whose
  application limits their size, such as some preferences panels) stay
  their size in the middle of it, over whatever was open before; one larger
  than the space between the panels is cut off at the bottom.
- A double tap does not open things that want a double click (icons in a
  Tracker window): taps on a touch screen are not counted as clicks by the
  HID driver, and app_server wants the second click within 4 pixels of the
  first.
- The keyboard follows text fields of native applications. Terminal does not
  announce its text view and is known by name; web pages in a browser do not
  announce theirs at all: use the keyboard button.
- English only: the keyboard layouts, the 10,000 words swipe typing knows, and
  the shell's own labels.
- The keyboard's emoji are single characters: a key carries 8 bytes, so
  sequences (skin tones, flags, families) are left out, though the system
  draws them ([COLOR-EMOJI.md](COLOR-EMOJI.md)).
- No dictation: the microphone key is waiting for the Neural Kit's speech
  recognition ([NEURAL-KIT.md](NEURAL-KIT.md)).
- The favourites and the dock are fixed lists of what is installed, and the
  task switcher shows the first six windows (four on a tall screen).
- No list of notifications in the action drawer.
- Only applications from the applications and preferences directories are
  listed (`AppList.cpp` is where another kind of application would be added).

## Testing

- `src/apps/mobileshell/test/run.sh` checks swipe typing on the build host:
  3,000 words swiped with made-up fingers over the keys of a phone and over
  the wide keys of a laptop panel.
- In a virtual machine the pointer is QEMU's USB tablet, which the system
  takes for a pen: a press sent together with a jump of the pointer is what a
  finger looks like, a press held still is a long press.

## Credits

air/OS Mobile is a port of [Renku Mobile](https://github.com/idumych/RenkuMobile)
(MIT License, © 2026 Renku Mobile contributors), a phone shell written for
Renku OS: the home screen, panels, drawers, task switcher, keyboard and swipe
decoder are its code, changed here to share a desktop with Tracker and the
Deskbar, to give it back, and to fit a tablet's screen.

- Swipe typing's words: the Google Books Ngram Corpus's most frequent English
  words, as cleaned in
  [orgtre/google-books-ngram-frequency](https://github.com/orgtre/google-books-ngram-frequency),
  [CC BY 3.0](https://creativecommons.org/licenses/by/3.0/)
  (`tools/make-word-data.py` makes `WordData.h` from it).
- The swipe trail's widths and timings are the defaults of AOSP's LatinIME
  (Apache 2.0).
- The Corners layout follows the idea of
  [Unexpected Keyboard](https://github.com/Julow/Unexpected-Keyboard), flicking
  a key towards its corners; none of its code or layouts is used.
- Emoji: Unicode's emoji-test.txt (Unicode License;
  `tools/make-emoji-data.py` makes `EmojiData.h` from it).
