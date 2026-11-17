# Terminal

air/OS's Terminal looks and works like Haiku's: the same windows, tabs,
menus, preferences, keyboard shortcuts and icon. Underneath, it is a new
terminal. The emulator was rewritten to understand what modern command line
programs send (kitty's protocol extensions, true color, styled underlines,
images, the kitty keyboard protocol). Drawing was rewritten so the GPU can do
it. From iTerm2 it took split panes, the search bar, inline images
(`OSC 1337`) and alerts as notifications (see *Panes, search and alerts*).

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
| `KITTY_PUBLIC_KEY` | `1:` and an X25519 public key, new for each Terminal; programs encrypt passwords for it (remote control, file transfer). Only in builds with OpenSSL (x86_64 today; the arm64 build has no OpenSSL package) |

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
| Notifications | `OSC 99` | Complete as far as the protocol goes, through the system's notifications: title, body, urgency, `o=unfocused`, ids, chunked and base64 payloads, icons by name (`n=`: kitty's error, warn, info, question, help, file-manager, system-monitor, text-editor, or an application signature) or as PNG/JPEG/GIF data (`p=icon`, kept by its `g=` key), expiry (`w=`), clicks (`a=focus` shows the tab, `a=report` tells the program), buttons (`p=buttons`, the number of the one clicked is reported), close events (`c=1`), taking one back (`p=close`), the `p=?` and `p=alive` queries. Sounds other than the system's are not played. |
| Pointer shapes | `OSC 22` | Complete, including the stack (`>` `<`) and queries. CSS names map to Haiku's cursors; the few Haiku has no cursor for fall back to the nearest one. |
| Color control | `OSC 21`, `OSC 30001`, `OSC 30101` | Set, query and reset any palette or special color; the color stack. Not: background transparency (`transparent_background_color`). |
| Clipboard | `OSC 5522`, mode `5522` | Reading and writing by MIME type, paste events. Reading asks the user every time. |
| Unscroll | `CSI Pn + T` | Complete. |
| Text sizing | `OSC 66` | Text 2 to 7 times larger (`s=`), in a given number of cells (`w=`), smaller within its cells (`n=`/`d=`, superscripts and subscripts) and placed (`v=`, `h=`); colored emoji too. Blocks follow the spec's rules for wrapping, scrolling to fit, overwriting, erasing (ICH, DCH, ECH, EL, ED, IL, DL) and the CPR test. A resize to another width turns blocks into normal text, since they cannot be wrapped again. |
| Miscellaneous | `CSI ? s`/`r` without parameters, `SGR 221`/`222`, `CSI 22 J`, DECCARA/DECSACE with every SGR attribute, the mouse-leave event | Complete. |
| Shell integration | `OSC 133` | Prompt and output marks; jumping between prompts. |
| Multiple cursors | `CSI > ... SP q` | Complete: block, beam, underline and follow-the-cursor shapes at points, rectangles or the cursor; their two colors (`30`, `40`, including "special"); the support, cursor and color queries. They blink with the cursor, show when DECTCEM hides it, and go with ED 2/3/22, a reset, the other screen and a resize. |
| Remote control | `DCS @kitty-cmd` | kitty's windows are Terminal's panes: `launch` (and `new-window`) with the default type `window` splits the sender's tab (`--location=vsplit` side by side, `hsplit` on top of each other, otherwise along the longer side) and types the command into the new pane's shell; `--type=tab` opens a tab, `os-window` another Terminal. `ls` lists each tab's panes as its windows (layout `splits`), `close-window` closes a pane, `close-tab` all of a tab's. Off unless the settings say `"RemoteControl" , "yes"` (as kitty's `allow_remote_control` is off). The commands that fit Terminal's windows and tabs, kitty's "windows": `ls`, `launch` and `new-window` (a tab, or another Terminal), `send-text`, `get-text` (screen, all, selection, command outputs), `set-tab-title`, `set-window-title`, `set-colors`, `get-colors`, `scroll-window`, `focus-window`/`focus-tab`, `close-window`/`close-tab`, `set-font-size`, `resize-os-window`, `signal-child`; kitty's `match` expressions `id:`, `title:`, `pid:`, `num:`, `recent:0`, `state:`, and `self`/`all`. Commands encrypted for `KITTY_PUBLIC_KEY` with a password (`kitten @ --password`) run when it is the settings' `"RemoteControlPassword"`, even with remote control off. Not: kitty's per-password lists of allowed commands, async and streaming commands. |
| Drag and drop | `OSC 72` | Drops: a program that registers (`t=a`) hears about drags over it (`t=m`, with the types once and when they change, and when the drag leaves), takes or refuses them, gets the drop (`t=M`) and asks for its data by type: files as `text/uri-list` (and their paths as `text/plain`), anything else by MIME type. A program on another machine (its machine id differs, `t=a:x=1`) reads dropped files, symbolic links and directories through the terminal. Drags: a program that offers them (`t=o:x=1`) gets the drag gesture instead of a selection starting, offers types, sends their data and an image (RGB, RGBA, PNG or text) and starts the drag; Tracker, for one, takes text and files from it. Not: data asked for after the drop rather than sent before (`t=e:x=5`), pulling files from a program on another machine (`t=k`), changing the image during the drag. Haiku does not tell the source where its drag went, so "dropped" and "finished" come when the button goes up. Terminal's machine id is a random one kept in `~/config/settings/Terminal/machine_id`. |
| File transfer | `OSC 5113` | Both ways: a program sends files, directories, symbolic and hard links with their times and permissions (a send session), or reads them from this computer (a receive session), zlib-compressed or not, after the user allows it in an alert naming what is asked for. A password, the settings' `"FileTransferPassword"`, skips the alert: encrypted for `KITTY_PUBLIC_KEY` (`kitten transfer -p`) or as kitty's older SHA-256 form. Cancel, quiet modes, ten sessions of each kind, ten minutes without activity end a session. Files are written next to their place and moved there at the end; hard links become copies on BFS, which has none. Tested with kitty 0.49.2's `kitten transfer` on Linux both ways (a 200 MB file at 3.1 MB/s to air/OS, 4.5 MB/s from it). Not: rsync deltas (`kitten transfer -x`), refused per file so the client sends whole files. |

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
  (mode 1007: the wheel sends cursor keys on the alternate screen; on unless
  a program turns it off or `~/config/settings/Terminal/Default` says
  `"Alternate screen scroll" , "false"`). Holding Shift selects text even
  when a program reports the mouse.
- **Modes:** bracketed paste (2004), synchronized output (2026: drawing waits
  until the program is done, at most a second), in-band resize reports
  (2048), dark/light color scheme reports (2031, `CSI ? 996 n`), grapheme
  clusters (2027, always on).
- **OSC:** 0/1/2 titles, 7 working directory, 8 hyperlinks, 9 and 777
  notifications (iTerm2, urxvt), `9;4` progress (shown as a progress
  notification), 52 clipboard, 133 shell integration marks, 1337 (iTerm2,
  below).

### iTerm2's extensions (OSC 1337)

| Sequence | Does |
| --- | --- |
| `File=inline=1;...:`*base64* | An inline image, as iTerm2's `imgcat` sends it: any format the Translation Kit reads (PNG, JPEG, GIF's first frame, WebP, ...). `width` and `height` in cells (`N`), points (`Npx`, one point is one logical pixel, so images keep their size at 200 %), percent of the terminal (`N%`) or `auto` (the image's own size, in points); `preserveAspectRatio=0` stretches it; `doNotMoveCursor=1`. It is placed at the cursor, no wider than the space to the right, over cells it clears; the cursor goes after it on its last row, as iTerm2 leaves it (so `imgcat`'s newline puts the prompt below). It scrolls with the text and goes with `clear` like a kitty image. Up to 64 MB in one sequence (other OSC strings stay limited to 1 MB). |
| `MultipartFile=`*args*, `FilePart=`*base64*, `FileEnd` | The same in pieces (iTerm2 3.5's `imgcat`, and through tmux). |
| `File=` without `inline=1` | A download: Terminal asks whether to save it, then saves it in `~/Downloads` (`name 2.ext` if the name is taken) and says so in a notification, which opens it when clicked. |
| `ReportCellSize` | Answers `OSC 1337 ; ReportCellSize=`*height*`;`*width*`;`*scale* `ST`, in points. |
| `RequestAttention=yes`, `once`, `no` | A notification "needs your attention" when the terminal is not seen (`yes` stays until clicked); `no` takes it back. |
| `StealFocus` | Brings the window, the tab and the pane to the front. |
| `SetMark` | A mark at the cursor's line: the prompt jumps (Ctrl+Shift+Z/X) stop there, and *Alert on next mark* fires. |
| `ClearScrollback`, `CursorShape=0/1/2`, `Copy=:`*base64*, `SetUserVar=`*name*`=`*base64*, `ReportVariable=`*base64*, `CurrentDir=` | As in iTerm2. |

`TERM_PROGRAM` stays `airos-terminal`; programs that only try iTerm2's
images when it says `iTerm.app` need to be told (`timg -pi`), or given
`TERM_PROGRAM=iTerm.app` for one command.
`imgcat` itself sends the sequence whatever the terminal.

### Text

Characters are grouped into grapheme clusters by Unicode 16's rules (UAX
#29), so a family emoji, a flag or a letter with several accents takes the
cells it should and moves as one. Widths follow Unicode 16's East Asian Width
and emoji tables. When the window is resized, lines that wrapped are wrapped
again to the new width, in the history too, and the cursor and images stay
with their text.

**Emoji in color.** Terminal draws emoji itself, to fit them into its
cells (app_server draws them in color for other programs, see
[COLOR-EMOJI.md](COLOR-EMOJI.md)), with FreeType from a color font: Noto Color
Emoji (HaikuPorts' `noto_emoji_color`, in air/OS images), or any font file
named `*ColorEmoji*` or `*Twemoji*` in the font directories;
`TERMINAL_EMOJI_FONT` names a file. A cluster asking for emoji presentation
(an emoji character, VS16, a keycap) is drawn in color, fitted into its two
cells. Flags, skin tones, keycaps and ZWJ sequences such as families come
from the font's ligatures (its GSUB table). Without a color font, emoji are
drawn in one color as before. Color glyphs are faint, underlined and struck
through like text.

**Ligatures.** Coding fonts that join characters (Fira Code, JetBrains
Mono, Cascadia Code: `->` as an arrow, `!=` as ≠, `===`, `&&`, `//`) show
their ligatures. A word (the characters between spaces, in one style) with
two symbols next to each other is shaped with HarfBuzz, loaded at run time
from `libharfbuzz.so.0` (air/OS has it with Summit; without it there are no
ligatures). Where the font puts other glyphs than the characters' own, those
cells are drawn together with FreeType from the glyphs and cut into cells,
so each keeps its color and the cursor still covers one cell. The font's file
is found by its family and style names in the font directories; a style the
family does not have (app_server's slanted italics) gets no ligatures. As
kitty's `disable_ligatures`, the settings' `"DisableLigatures"` is `never`
(the default), `cursor` (not in the word the cursor is on) or `always`.
Printing 1.8 MB of C to the screen takes as long with ligatures as without
(0.14 to 0.2 s on the X399).

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

## Panes, search and alerts

### Split panes

Any tab can be split, again and again, as in iTerm2. The new pane starts a
shell in the active pane's directory.

| Keys | Does |
| --- | --- |
| Alt+D | Split the active pane vertically (side by side) |
| Alt+Shift+D | Split it horizontally (one above the other) |
| Alt+] / Alt+[ | The next / previous pane |
| Alt+Option+arrow keys | The pane to the left, right, above or below |
| Alt+Shift+Enter | Maximize the active pane (the others stay running); again to go back |
| Alt+W | Close the active pane (the tab, when it has only one) |

These are also in **Terminal ▸ Split vertically / Split horizontally** and
**Terminal ▸ Panes** (*Make panes the same size*, *Close active pane*).
Click a pane to make it the active one. The dividers can be dragged; a
double click on one shares its room out evenly again. Panes that are not
active are dimmed, unless the settings say `"Dim inactive panes" ,
"false"`. A pane whose shell quits closes; the last one closes its tab. The
tab's title is that of its active pane. Changing the font size keeps the
window's size when the tab is split: the panes get fewer or more rows.
Closing a pane whose shell runs something asks first, as closing a tab does.

### Search

**Edit ▸ Find** (Alt+F) opens a search bar at the top right of the active
pane, iTerm2's find bar instead of the old dialog. It finds as you type, in
the history and on the screen, highlights every match (yellow; the current
one orange, and selected so Alt+C copies it) and counts them ("3 of 17").
Return or the up arrow goes to the match above (older), Shift+Return or the
down arrow to the one below; Alt+G and Alt+Shift+G do the same from the
terminal. Escape or the × closes the bar. The menu at its left chooses the
case (*Smart case*, the default: the case matters only when the text has a
capital; *Match case*; *Ignore case*), *Regular expression* (ECMAScript) and
*Whole words*. A match can cross a line the terminal wrapped, not a line
end. While the bar is open, new output is searched again (at most every
quarter second). **Use selection for find** (Alt+E) searches for the
selected text.

### Notifications and alerts

Programs' own notifications (OSC 9, 99 and 777, iTerm2's
`RequestAttention`) go to the system's notifications, as before; a click
shows the tab and the pane they came from. Terminal adds its own, as
iTerm2 does:

- **The bell** in a terminal you cannot see (another tab, another window
  in front, minimized) shows a notification, at most every five seconds
  per pane. `"Notify on bell" , "false"` in the settings turns it off.
- **A long command finished** where you cannot see it: with the shell
  integration's marks (OSC 133, on by default for bash), a command that ran
  10 seconds or more says it finished or failed (with its exit status) and
  how long it took. `"Notify when commands finish" , "30"` changes the
  time, `"0"` turns it off.
- **Edit ▸ Alert on next mark** (Alt+Shift+A) asks for a notification when
  the running command finishes, or a program sets a mark, seen or not.

Terminals in tabs that are not shown handle their notifications, bells and
marks at once; before, they waited until the tab was shown.

## Links

**Click a link to open it** in the default browser (Summit). Hovering over
a link underlines it and shows the link pointer. A drag that starts on a
link still selects text, a double click still selects the word, and the
click that brings the window to the front opens nothing. The click opens
the link even when the program asks for the mouse (Claude Code, codex,
tmux with `mouse on`): those do nothing with a click on an address, and
over ssh they could only open it on the other computer. A drag from a link
goes to the program as usual. *Open links with a click* in the settings
turns this off.

Hold **Alt** and click to open links and also file names (`path`,
`path:line`, `path:line:column`, relative to the shell's directory), or Alt
and right-click for *Open link* and *Copy link location*. (The left Alt key
is the Command key of a PC keyboard in Haiku's default key layout; with the
Keymap preference's "Windows/Linux" shortcut layout it is Ctrl instead.)

What counts as a link (`emulator/TerminalLinks.cpp`, with tests):

- **OSC 8 hyperlinks** of any length up to 1 MiB (whole sequences of
  operating system commands are), from the cells around the pointer with
  the same address, across lines: a program that wraps a link repeats it
  on each line, after the indentation of its text.
- **Addresses in the text** that start with `http://`, `https://`,
  `ftp://`, `ftps://`, `sftp://`, `ssh://`, `file://`, `git://` or
  `mailto:`, without punctuation that ends the sentence and without
  closing brackets they did not open (`(see https://e.com/Foo_(bar)).`
  gives `https://e.com/Foo_(bar)`; markdown's `[text](https://...)` gives
  the address). A long address is followed over the lines it was broken
  into:
  - by the terminal (a soft wrap);
  - by the program, to the right margin, with the rest indented like the
    paragraph (Claude Code's answers and its sign-in address; its text
    fields stop one column short), or short of the margin after `://`,
    `-`, `=`, `&` and the like when the rest would not have fitted
    (codex);
  - by tmux at the border of a pane (`│`), on either side of it; tables
    drawn with `│` are the same.

  A line that ends with an address is not continued with the next line's
  words when they would have fitted, or when the next line is indented
  further than the address started.

### Programs on another computer, and tmux

Over ssh nothing tells the remote program about Terminal (ssh passes on
`TERM` and, if configured, `LANG`/`LC_*` only), so Claude Code prints
plain addresses (a markdown link as `text (address)`), which are found
in the text as above. To get OSC 8 links from it anyway, send
`FORCE_HYPERLINK=1` (`SetEnv FORCE_HYPERLINK=1` in `~/.ssh/config`, if the
server's `AcceptEnv` takes it).

**tmux** 3.4 and newer passes OSC 8 links on only to terminals it knows to
support them, by the name the terminal answers to XTVERSION (iTerm2,
XTerm, foot, mintty, tmux) or by `terminal-features`; Terminal answers
`airos-terminal(2.0)`, which tmux does not know. Inside tmux, Claude Code
(which sees `TERM_PROGRAM=tmux` 3.4) prints OSC 8 links, tmux drops them,
and a markdown link then shows only its text, without the address. Tell
tmux, in `~/.tmux.conf` on the machine running it:

    set -as terminal-features ',xterm-256color:hyperlinks'

(new tmux clients pick it up; `tmux kill-server` or a new session for
running ones). Plain addresses work without it.

`BROWSER` and `FORCE_HYPERLINK` (see the table at the top) are not changed
when already set, so a profile can override them. Programs that open a web
page run `$BROWSER <address>` and otherwise fall back to `xdg-open`, which
Haiku does not have; `open` hands the address to the preferred application.
Node.js programs using the common `supports-hyperlinks` check only print
OSC 8 links for terminals they know, hence `FORCE_HYPERLINK`. It also
applies when such a program's output goes to a pipe or file; set it to `0`
in the profile if that is unwanted.

`src/tests/apps/terminal/TermRun.cpp` (`make termrun` there) runs a program
on the build host with Terminal's emulator answering its queries, and
lists the links it would find: `termrun -s script -- claude` records,
`termrun -p recording` plays a recording back.

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
| `src/apps/terminal/` | The application: windows, tabs, preferences (unchanged), `TermView` (input, selection, scrolling, drawing, events from the emulator, searching, alerts), `PaneView` (a tab's split panes), `FindBar`, `Shell`. |

The old engine (`TermParse`, `TerminalBuffer`, `BasicTerminalBuffer`,
`HistoryBuffer`, the VT tables) is gone.

## Tests

- `make -C src/tests/apps/terminal test` builds the emulator with
  AddressSanitizer on the build host and runs 550 checks: parsing, cursor
  movement, margins, wrapping and reflow, history, every reply, keys, mouse,
  paste, the kitty protocols, grapheme clusters, iTerm2's OSC 1337 (image
  sizes, cursor, pieces, downloads), searching for all matches (case,
  words, regular expressions, wraps, wide characters), shell marks.
- `jam GLStressTest` builds a program that drives the OpenGL add-on with
  made-up frames at any size (`STRESS_FULL`, `STRESS_RESIZE`,
  `STRESS_ATLAS`, `STRESS_IMAGE`) and reports frame times and GPU loss.
- On the X399 (2026-10-19): a test page of every text style, color mode,
  box drawing, CJK, emoji, links and a kitty image. Also nano, selection,
  paste, resize reflow, wheel scrolling, notifications, kitty keyboard flags
  1 and 11, SGR mouse reports, and switching between the GPU and software
  renderers mid-run.
- In a QEMU VM of the branch's image (2026-11-17): `imgcat`-style images
  (natural size, `width=20`, `height=2` next to text, multipart, `clear`
  removing them), 24-bit colors; splitting vertically and horizontally with
  `stty size` right in every pane (a new pane once kept 80 × 25: detached
  views do not resize their children), pane keys, maximize, closing panes
  (`exit`, Alt+W, the alert for a busy pane), clicking a pane, dragging a
  divider, font size in split and single tabs; the search bar (127 matches
  counted and highlighted, Return, smart case, regular expressions, the
  options menu, new output found while it is open); notifications from a
  hidden tab (bell, a failed 11 s command, RequestAttention) and a click on
  one bringing its tab back; Alert on next mark; a download saved after the
  alert; kitty's `launch --location=vsplit` and `ls` listing the panes.
