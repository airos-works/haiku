# Terminal

air/OS's Terminal looks and works like Haiku's: the same windows, tabs,
menus, preferences, keyboard shortcuts and icon. Underneath, it is a new
terminal. The emulator was rewritten to understand what modern command line
programs send (kitty's protocol extensions, true color, styled underlines,
images, the kitty keyboard protocol). Drawing was rewritten so the GPU can do
it.

What it does not do yet, compared with kitty, is in
[TERMINAL-KITTY-GAPS.md](TERMINAL-KITTY-GAPS.md).

## For programs

| Variable | Value |
| --- | --- |
| `TERM` | `xterm-256color` (unchanged; every system has its terminfo entry) |
| `COLORTERM` | `truecolor` |
| `TERM_PROGRAM` | `airos-terminal` |
| `TERM_PROGRAM_VERSION` | `2.0` |
| `BROWSER` | `/bin/open`, unless set (see *Links* below) |
| `FORCE_HYPERLINK` | `1`, unless set |

Programs that ask get these answers: XTVERSION gives
`airos-terminal(2.0)`, DA1 `CSI ? 62 ; 22 ; 52 c` (VT220 class, color,
OSC 52 clipboard), DA2 `CSI > 1 ; 4000 ; 1 c`. XTGETTCAP answers the usual
terminfo capabilities, including the ones for styled and colored
underlines (`Smulx`, `Setulc`) and true color (`RGB`, `Tc`).

### kitty's protocol extensions

| Extension | Sequences | State |
| --- | --- | --- |
| Graphics | `APC G ... ST` | Complete: direct, file, temporary file and shared memory transfers; PNG, RGB, RGBA, zlib; chunking; placements with z-index layers (below the background, between background and text, above text); relative placements; Unicode placeholders (U+10EEEE with diacritics); every delete mode; animation (frames, composition, gaps, loops). Images move with the text when it scrolls and when a resize rewraps it. 320 MB of images per terminal. |
| Keyboard | `CSI = > < ? u` | Complete: all five flags (disambiguate, event types, alternate keys, all keys as escapes, associated text), separate stacks for the main and alternate screens. |
| Styled and colored underlines | `SGR 4:0`..`4:5`, `58`, `59` | Complete: straight, double, curly, dotted, dashed, in any color. |
| Notifications | `OSC 99` | Through the system's notifications: title, body, urgency, `o=unfocused`, ids, chunked and base64 payloads, icons by name (`n=`: kitty's error, warn, info, question, help, file-manager, system-monitor, text-editor, or an application signature) or as PNG/JPEG/GIF data (`p=icon`, kept by its `g=` key), expiry (`w=`), clicks (`a=focus` shows the tab, `a=report` tells the program), taking one back (`p=close`), the `p=?` and `p=alive` queries. Closing cannot be reported (`c=1` gets kitty's `untracked`) and there are no buttons: the system's notifications have neither. |
| Pointer shapes | `OSC 22` | Complete, including the stack (`>` `<`) and queries. CSS names map to Haiku's cursors; the few Haiku has no cursor for fall back to the nearest one. |
| Color control | `OSC 21`, `OSC 30001`, `OSC 30101` | Set, query and reset any palette or special color; the color stack. Not: background transparency (`transparent_background_color`). |
| Clipboard | `OSC 5522`, mode `5522` | Reading and writing by MIME type, paste events. Reading asks the user every time. |
| Unscroll | `CSI Pn + T` | Complete. |
| Text sizing | `OSC 66` | Text 2 to 7 times larger (`s=`), in a given number of cells (`w=`), smaller within its cells (`n=`/`d=`, superscripts and subscripts) and placed (`v=`, `h=`); colored emoji too. Blocks follow the spec's rules for wrapping, scrolling to fit, overwriting, erasing (ICH, DCH, ECH, EL, ED, IL, DL) and the CPR test. A resize to another width turns blocks into normal text, since they cannot be wrapped again. |
| Miscellaneous | `CSI ? s`/`r` without parameters, `SGR 221`/`222`, `CSI 22 J`, DECCARA/DECSACE with every SGR attribute, the mouse-leave event | Complete. |
| Shell integration | `OSC 133` | Prompt and output marks; jumping between prompts. |
| Multiple cursors | `CSI > ... SP q` | Complete: block, beam, underline and follow-the-cursor shapes at points, rectangles or the cursor; their two colors (`30`, `40`, including "special"); the support, cursor and color queries. They blink with the cursor, show when DECTCEM hides it, and go with ED 2/3/22, a reset, the other screen and a resize. |
| Remote control | `DCS @kitty-cmd` | Off unless the settings say `"RemoteControl" , "yes"` (as kitty's `allow_remote_control` is off). The commands that fit Terminal's windows and tabs, kitty's "windows": `ls`, `launch` and `new-window` (a tab, or another Terminal), `send-text`, `get-text` (screen, all, selection, command outputs), `set-tab-title`, `set-window-title`, `set-colors`, `get-colors`, `scroll-window`, `focus-window`/`focus-tab`, `close-window`/`close-tab`, `set-font-size`, `resize-os-window`, `signal-child`; kitty's `match` expressions `id:`, `title:`, `pid:`, `num:`, `recent:0`, `state:`, and `self`/`all`. Not: the encrypted (password) form, async and streaming commands, kittens. |
| File transfer, drag and drop | `OSC 5113`, `OSC 72` | Not done. |

### Other extensions

- **xterm and DEC:** the VT220 set and most of xterm's: margins (top/bottom
  and left/right), rectangular area operations (DECCRA, DECERA, DECFRA,
  DECSERA, DECCARA, DECRARA), DECIC/DECDC, tab stops, character sets,
  DECRQM for every mode, DECRQSS, XTGETTCAP, XTSAVE/XTRESTORE, XTPUSHSGR,
  XTPUSHCOLORS, title stacks, window size reports (`CSI 14/16/18 t`),
  modifyOtherKeys (`CSI > 4 ; 2 m`).
- **Colors:** 256 colors and 24-bit color in both the `;` and `:` forms;
  OSC 4, 10-19 and 104-119 to set, query and reset them; the 753 color names
  of X11 and kitty.
- **Mouse:** X10, normal, button and any-motion tracking; default, UTF-8, SGR,
  urxvt and SGR pixel encodings; focus events (mode 1004); alternate scroll
  (mode 1007). Holding Shift selects text even when a program reports the
  mouse.
- **Modes:** bracketed paste (2004), synchronized output (2026: drawing waits
  until the program is done, at most a second), in-band resize reports
  (2048), dark/light color scheme reports (2031, `CSI ? 996 n`), grapheme
  clusters (2027, always on).
- **OSC:** 0/1/2 titles, 7 working directory, 8 hyperlinks, 9 and 777
  notifications (iTerm2, urxvt), `9;4` progress (shown as a progress
  notification), 52 clipboard, 1337 `CurrentDir`.

### Text

Characters are grouped into grapheme clusters by Unicode 16's rules (UAX
#29), so a family emoji, a flag or a letter with several accents takes the
cells it should and moves as one. Widths follow Unicode 16's East Asian Width
and emoji tables. When the window is resized, lines that wrapped are wrapped
again to the new width, in the history too, and the cursor and images stay
with their text.

**Emoji in color.** app_server draws glyphs in grey levels only, so
Terminal draws emoji itself with FreeType from a color font: Noto Color
Emoji (HaikuPorts' `noto_emoji_color`, in air/OS images), or any font file
named `*ColorEmoji*` or `*Twemoji*` in the font directories;
`TERMINAL_EMOJI_FONT` names a file. A cluster asking for emoji presentation
(an emoji character, VS16, a keycap) is drawn in color, fitted into its two
cells. Flags, skin tones, keycaps and ZWJ sequences such as families come
from the font's ligatures (its GSUB table). Without a color font, emoji are
drawn in one color as before. Color glyphs are faint, underlined and struck
through like text.

**Accents.** app_server places glyphs one after the other without
positioning combining marks, which put the accent of a decomposed é past the
e. Combining diacritics (U+0300 to U+036F and the other diacritical mark
blocks) are centered over their character's cells instead. Marks of other
scripts still need shaping (see the gaps).

## Drawing

Everything in the view is drawn into one bitmap at the screen's pixel
density, which `Draw()` puts on screen 1:1. Text stays sharp at 150 % or 200 %.

- **Glyphs** are drawn once by app_server into an atlas (pages of 2048 x 2048
  pixels) and reused. Box drawing, block elements, sextants, braille, and
  Powerline symbols are drawn by Terminal itself, so they join without gaps
  whatever the font. Bold uses the font's bold face; italic uses its italic
  face, or a slant where the font has none (Noto Sans Mono has no italic).
- **The OpenGL renderer** (the `Terminal-OpenGL` add-on in
  `add-ons/Terminal`) draws the rows that changed on the GPU, in an
  offscreen frame buffer. Rows without images take one pass. It then reads
  those rows back into the bitmap. It needs OpenGL 3.0 (GLSL 1.30) and is
  skipped when OpenGL is Mesa's software renderer (llvmpipe, softpipe).
- **The software renderer** draws the same rows with the processor.

Terminal uses whichever is faster, because reading pixels back from a GPU
can cost more than drawing them with the processor. It times the large
frames (more than half the view changed, as when scrolling). Each renderer
draws a series of 8 of them after 4 that are not counted, since a GPU that
was idle needs a few frames to speed up. After that, Terminal keeps the
faster one, changes when the other is 20 % faster, and gives the other a new
series after a minute. Timing steady series matters. Timing single frames
drawn by both renderers made the GPU look worse (it had slowed down) and the
processor too (the read back had emptied its caches).

Full-screen scrolling, both machines on master 2026-10-19 (software on one
thread; the software renderer now shares large frames out among up to four
threads, row by row: 3.8 ms for the X399's 4K frame):

| Machine | Frame | GPU | Software (one thread) | Terminal uses |
| --- | --- | --- | --- | --- |
| X399, GTX 1070 (NVK through Zink, Mesa 22) | 3802 x 2150 | 23.5 ms | 12.4-13.4 ms | software |
| Framework 13, Radeon 760M (radeonsi) | 2223 x 1495 | 2.8 ms | 1.8 ms | software |

On the GTX 1070, 7.4 ms of a 4K frame is drawing (the GPU seems to run at
low clocks) and 12.5 ms reading back. If the GPU is lost (a reset, a driver
giving up, more than 2 seconds for a frame), Terminal draws in software from
then on.

To choose a renderer, add one line to `~/config/settings/Terminal/Default`:

    "Renderer" , "software"

or `"opengl"`. The `TERMINAL_RENDERER` environment variable does the same for
one run. `hey Terminal get renderer of View termview of Window 0` tells
which renderer is drawing. `TERMINAL_STATS=1` prints frame times and the
renderer comparisons to standard error.

Frames are drawn at most 120 times a second, only for the rows that
changed. Reading the shell's output happens in its own thread, and so does
writing to it (a program that floods the terminal with queries cannot block
the window).

Measured on the X399 against the old Terminal, at the same window size:
`cat` of a 5.7 MB file takes 0.14 s instead of 0.42 s, and `seq 1000000`
0.58 s instead of 0.71 s.

## Keys and mouse

Keys send what they always did (xterm's encoding), unless a program turns on
modifyOtherKeys or the kitty keyboard protocol. The protocol's key numbers
come from Haiku's key codes. The "base layout key" is the US layout's key at
the same position. Modifiers map to kitty's like this: Shift and Control as
themselves; the Command key (left Alt on a PC keyboard) as Super; the left
Option key as Alt when the *Use left Option as Meta key* setting is on; Caps
Lock and Num Lock as locks.

New shortcuts, as in kitty:

| Keys | Does |
| --- | --- |
| Ctrl+Shift+Z / Ctrl+Shift+X | Scroll to the previous / next shell prompt |
| Ctrl+Shift+Up / Down | Scroll the history one line |

Shift+Page Up/Down/Home/End scroll the history, as before.

### Shell integration

Jumping between prompts needs the shell to mark them. The haiku package
installs `data/profile.d/airos-terminal.sh`, which the profile reads: in an
interactive bash running in Terminal, it marks the start of each prompt, the
command typed and its output (OSC 133 `A`, `B`, `C`, and `D` with the exit
status), and reports the working directory (OSC 7), the way kitty's bash
integration does. It wraps `PS1` and `PS2` before each prompt, so a profile
that sets its own prompt keeps the marks. `AIROS_TERMINAL_INTEGRATION=0` in
the environment turns it off.

## Links

Hold **Alt** and click a link to open it in the default browser, or Alt and
right-click it for *Open link* and *Copy link location*. Holding Alt over a
link underlines it and shows the link pointer. (The left Alt key is the
Command key of a PC keyboard in Haiku's default key layout; with the Keymap
preference's "Windows/Linux" shortcut layout it is Ctrl instead.)

- **OSC 8 hyperlinks** of any length up to 1 MiB (whole sequences of
  operating system commands are). The link's range is every cell around
  the pointer with the same link, across lines filled to the margin, which is
  how a program repeats a link it has wrapped.
- **Plain addresses** that a program broke into lines itself continue on the
  next line when a line is full to the right margin and the next one starts
  with address characters (`HyperLinkState::_ExtendURLOverLineBreaks()`).
  Claude Code prints its sign-in address this way.

`BROWSER` and `FORCE_HYPERLINK` (see the table at the top) are not changed
when already set, so a profile can override them. Programs that open a web
page run `$BROWSER <address>` and otherwise fall back to `xdg-open`, which
Haiku does not have; `open` hands the address to the preferred application.
Node.js programs using the common `supports-hyperlinks` check only print
OSC 8 links for terminals they know, hence `FORCE_HYPERLINK`. It also
applies when such a program's output goes to a pipe or file; set it to `0`
in the profile if that is unwanted.

## Clipboard

A program can put text on the clipboard with OSC 52 (Claude Code's "c to
copy" does) or with kitty's OSC 5522, which can also carry other MIME
types. A program that wants to *read* the clipboard gets an alert asking
whether to allow it, every time. Listing the clipboard's types is allowed
without asking.

## Scripting

Besides `encoding`, `tty` and `command`, the view (`View termview of
Window 0`) has:

| Property | Does |
| --- | --- |
| `renderer` | get: the renderer drawing now |
| `contents` | get: the text on the screen |
| `snapshot` | execute with a path: save what the view shows, in frame buffer pixels, as PNG |

## Source

| Directory | What |
| --- | --- |
| `src/apps/terminal/emulator/` | The emulator: parser, screen and history, sequences, OSC commands, key and mouse encoding, kitty graphics. Plain C++ with no Interface Kit, so it builds and is tested on the build host. `UnicodeTables.cpp` and `ColorNames.cpp` are generated (see their headers). |
| `src/apps/terminal/render/` | Frames for the renderers (`FrameBuilder`), the glyph atlas (`GlyphCache`), box drawing, the software renderer. |
| `src/apps/terminal/render/opengl/` | The OpenGL renderer add-on. |
| `src/apps/terminal/` | The application: windows, tabs, preferences (unchanged), `TermView` (input, selection, scrolling, drawing, events from the emulator), `Shell`. |

The old engine (`TermParse`, `TerminalBuffer`, `BasicTerminalBuffer`,
`HistoryBuffer`, the VT tables) is gone.

## Tests

- `make -C src/tests/apps/terminal test` builds the emulator with
  AddressSanitizer on the build host and runs 300 checks: parsing, cursor
  movement, margins, wrapping and reflow, history, every reply, keys, mouse,
  paste, the kitty protocols, grapheme clusters.
- `jam GLStressTest` builds a program that drives the OpenGL add-on with
  made-up frames at any size (`STRESS_FULL`, `STRESS_RESIZE`,
  `STRESS_ATLAS`, `STRESS_IMAGE`) and reports frame times and GPU loss.
- On the X399 (2026-10-19): a test page of every text style, color mode,
  box drawing, CJK, emoji, links and a kitty image. Also nano, selection,
  paste, resize reflow, wheel scrolling, notifications, kitty keyboard flags
  1 and 11, SGR mouse reports, and switching between the GPU and software
  renderers mid-run.
