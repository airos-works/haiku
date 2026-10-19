# Terminal compared with kitty: what is missing

State on 2026-10-19, after the Terminal rewrite ([TERMINAL.md](TERMINAL.md)).
This list is for choosing what to add next. Each item says what it is, who
would notice, how big the work is, and what on air/OS stands in the way.
Effort: **S** is about a day, **M** a few days, **L** a week or more.

Already done, for reference: the graphics protocol (complete, with
animation and Unicode placeholders), the keyboard protocol (all flags),
styled and colored underlines, pointer shapes, OSC 21 colors and the color
stack, OSC 5522 clipboard and paste events, unscroll, DECCARA, the mode
save/restore, SGR 221/222, ED 22, the mouse-leave event, synchronized
output, in-band resize, color scheme reports, OSC 8 links, OSC 52, OSC 133
marks with prompt jumps, OSC 99 notifications (partly), OSC 66 (width
only), grapheme clusters, reflow, GPU drawing.

## Recommended next

| # | What | Effort | Why first |
| --- | --- | --- | --- |
| 1 | Shell integration for bash, on by default | S | Prompt jumps (Ctrl+Shift+Z/X) already work, but only for a shell that marks its prompts, and air/OS's bash does not. A profile snippet that emits OSC 133 and OSC 7 when `TERM_PROGRAM=airos-terminal` makes them work, and lets kitty-style features below build on it. |
| 2 | Color emoji | M | The most visible difference from kitty: emoji are drawn as monochrome outlines today. Prompts, `git` tools and Claude Code use them. |
| 3 | Faster full-screen drawing | S-M, then L | At 4K the GPU path loses to software on the X399 because of reading back. A multi-threaded software renderer (S-M) gives ~3x on full frames. Drawing straight into the screen without the read back (L) is what makes the GPU worth it. |
| 4 | Notification clicks, icons, expiry | S-M | Lets `kitten notify`-style tools and long-running jobs bring you back to the right tab. |
| 5 | Ligatures | M-L | Wanted by users of coding fonts (Fira Code, JetBrains Mono). |

## Protocols

### Shell integration beyond marks — S to M
kitty ships scripts for bash, zsh and fish that mark prompts and commands
(OSC 133) and report the working directory (OSC 7). It builds on them: click
at the prompt to move the cursor there (`click_events`), redraw the prompt
on resize (`redraw=0`), warn before closing a tab that runs a command
(`cmdline`), open the last command's output in a pager, select it.
Terminal reads the marks and jumps between prompts. Missing: the scripts
(S: a `profile.d` snippet for bash) and the features on top (M).

### Notifications (OSC 99) — S to M, partly blocked
Missing: telling the program that a notification was clicked (`a=report`)
or closed (`c=1`), buttons (`p=buttons`), icons (`p=icon`, `n=`, `g=`),
sounds other than the system's, expiry (`w=`).
- Icons and expiry: S. `BNotification::SetIcon()` and `Send(timeout)`.
- Clicks: M. `BNotification::SetOnClickApp()` with arguments relaunches
  Terminal on a click; the new instance has to hand the click to the window
  that sent the notification, then quit.
- Close reports and buttons: blocked. notification_server has no buttons and
  does not report closing; that would need changes there.

### Text sizing (OSC 66) — L
kitty draws text 2 to 7 times larger (headings in terminal programs), and
fractional sizes with alignment. Terminal reserves the cells but draws the
text at normal size. It touches the cell model, every editing operation,
history, reflow and both renderers. Few programs use it yet.

### Multiple cursors (`CSI > ... SP q`) — S to M
Extra cursors drawn by the terminal, for editors with multiple selections.
Very new; few editors send it.

### Remote control (`DCS @kitty-cmd`) — M for a useful subset
Scripting kitty from programs running in it (`kitten @ launch`, `send-text`,
`get-text`, `set-colors`, `set-tab-title` ...). Terminal can already be
scripted from outside with `hey` (see TERMINAL.md); this would add the kitty
wire format and the commands that fit Terminal's windows and tabs. kitty's
encrypted mode needs X25519 and AES-GCM.

### File transfer (OSC 5113) — L
Copying files over the terminal connection, used by `kitten transfer` and
`kitten ssh`. It needs an approval dialog, a file system state machine,
rsync-style deltas, xxHash and zlib. Of little use until the kitten programs
run on air/OS (see below).

### Drag and drop (OSC 72) — L
Programs in the terminal accepting drops and starting drags, MIME negotiated.
Terminal already pastes the paths of files dropped on it. Very new protocol.

### Background transparency (`transparent_background_color` in OSC 21) — blocked
Needs translucent windows, which app_server does not have.

### Clipboard passwords (OSC 5522 `pw=`) — S
kitty lets a program skip the read confirmation with a password the user
approved once. Terminal asks every time.

## Drawing

### Color emoji — M
app_server draws glyphs in grey levels only (no color bitmaps or color
layers). Terminal would rasterize emoji itself with FreeType (in the system)
from a color font. The emoji font in the image, Noto Emoji, is the
monochrome one; Noto Color Emoji (about 10 MB) would have to be added. The
atlas already has room for color pages (`GlyphAtlasInfo::colorPages`); both
renderers need to draw them.

### Ligatures — M to L
Shaping runs of text with HarfBuzz (a package other applications already
pull in) and drawing glyphs by id, which app_server's `BFont` cannot do; it
means FreeType in Terminal, as for emoji.
kitty also offers `disable_ligatures` under the cursor.

### Drawing without reading back — L
The GPU renderer draws into an offscreen buffer and reads the changed rows
back into a bitmap that app_server copies to the screen. On the X399
(GTX 1070, NVK) reading back a 4K frame takes 12.5 ms, so Terminal chooses
software there. Presenting the GPU's image directly needs a scanout or shared
buffer path per driver (amdgpu has one, used by Summit; NVK has none).

### Multi-threaded software drawing — S to M
Not a kitty feature, but the cheapest way to faster full-screen redraws: rows
can be drawn in bands on several cores (9 ms for a 4K frame now; an
estimated 3 ms with four).

### Font options — S each
kitty's `symbol_map` (a font per Unicode range), `narrow_symbols`,
`modify_font` (cell size, underline position), `text_composition_strategy`
(gamma and contrast). Terminal uses the font and size chosen in its
settings; app_server picks fallback fonts.

### Cursor trail, background image — S to M
Cosmetic.

## Terminal features (outside the protocols)

The owner's direction is to keep Terminal's interface as it is; these are
listed for completeness.

| Feature in kitty | Effort | Note |
| --- | --- | --- |
| Rectangular selection (Ctrl+Alt+drag) | S | Terminal selects lines only. |
| Scrollback in a pager, last command's output in a pager | S-M | The second needs shell integration. |
| Hints: pick URLs, paths, hashes with the keyboard | M | |
| Marks: highlight text matching a pattern | S-M | Terminal has Find. |
| Configurable key and mouse bindings | M | Terminal's shortcuts are fixed. |
| Splits and layouts (several shells side by side in a tab) | L | Terminal has windows and tabs. |
| Sessions (startup layouts) | M | |
| Broadcast typing to several shells | S | |

## kittens

kitty's helper programs (`icat`, `diff`, `ssh`, `transfer`, `themes`,
`hints`, `unicode_input`, `clipboard`, `notify`, `choose-files`, `query_terminal`)
are one Go program, `kitten`. Go does not support Haiku officially, so none of
them run. The protocols they use do work: programs that draw images
through the graphics protocol (`chafa`, `timg`, `viu`, `yazi`, `mpv
--vo=kitty`, notcurses) can be ported or built instead.

Programs that decide by `TERM=xterm-kitty` or `KITTY_WINDOW_ID` rather than
by asking the terminal will not turn on kitty features. Terminal keeps
`TERM=xterm-256color` on purpose: claiming to be kitty would make programs
expect the rest of kitty (remote control, kittens).
