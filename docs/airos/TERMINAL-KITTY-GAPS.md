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
marks with prompt jumps and bash integration, OSC 99 notifications (clicks,
icons, expiry), OSC 66 text sizing, multiple cursors, color emoji, grapheme
clusters, reflow, GPU drawing.

## Recommended next

| # | What | Effort | Why first |
| --- | --- | --- | --- |
| 1 | ~~Shell integration for bash~~ | done | `data/profile.d/airos-terminal.sh` marks prompts (OSC 133) and reports the directory (OSC 7); prompt jumps work out of the box. |
| 2 | ~~Color emoji~~ | done | Drawn with FreeType from Noto Color Emoji (`noto_emoji_color`, added to the CI images), sequences through the font's GSUB ligatures. |
| 3 | Faster full-screen drawing | ~~S-M~~ done, then L | Because of reading back, the GPU path loses to software on both test machines (X399 with NVIDIA, Framework 13 with AMD), so Terminal draws in software there today. The software renderer now uses up to four threads (4K frame 13 ms to 3.8 ms on the X399). Drawing straight into the screen without the read back (L) is what would make the GPU worth it. |
| 4 | ~~Notification clicks, icons, expiry~~ | done | A click shows the tab and reports to the program; icons by name or data; `w=`; `p=close`. |
| 5 | Ligatures | M-L | Wanted by users of coding fonts (Fira Code, JetBrains Mono). |

## Protocols

### Shell integration beyond marks — S to M
kitty ships scripts for bash, zsh and fish that mark prompts and commands
(OSC 133) and report the working directory (OSC 7). It builds on them: click
at the prompt to move the cursor there (`click_events`), redraw the prompt
on resize (`redraw=0`), warn before closing a tab that runs a command
(`cmdline`), open the last command's output in a pager, select it.
Terminal reads the marks and jumps between prompts, and air/OS's bash marks
them (`data/profile.d/airos-terminal.sh`). Missing: the features on top (M),
and scripts for zsh and fish (not in the image).

### Notifications (OSC 99) — done
Clicks, buttons and close events come from air/OS's notification_server,
which now answers a sender that asks (fields in the request the
BNotification API has no calls for: a reply messenger, the messages for a
click or a button and for the notification going away, button labels).
Missing: sounds other than the system's (kitty's `s=` names).

### Text sizing (OSC 66) — done
Blocks of larger, fitted or fractional text with the spec's editing rules.
Differences from kitty: a resize to another width turns blocks into normal
text (kitty keeps those that still fit); the cursor is drawn on its cell,
not over the whole block (a "should" in the spec); insert mode does not open
room for a block first.

### Multiple cursors (`CSI > ... SP q`) — done
All of it, drawn by both renderers.

### Remote control (`DCS @kitty-cmd`) — the useful part done
Off by default, like kitty's. The commands that fit Terminal's tabs work
(see TERMINAL.md). Missing: the encrypted form with passwords (X25519,
AES-GCM; S-M), commands for kitty's layouts, markers, background images and
the like (no such things in Terminal), `send-key`, async and streaming
commands, and the `kitten @` client itself (Go).

### File transfer (OSC 5113) — L
Copying files over the terminal connection, used by `kitten transfer` and
`kitten ssh`. It needs an approval dialog, a file system state machine,
rsync-style deltas, xxHash and zlib. Of little use until the kitten programs
run on air/OS (see below).

### Drag and drop (OSC 72) — mostly done
Drops (local and remote reads) and drags with pre-sent data work (see
TERMINAL.md). Missing (M): data the target asks for after the drop
(`t=e:x=5`, which would need Haiku's negotiated drag protocol), pulling
files from a program on another machine (`t=k`), changing the image while
dragging, and real "accepted", "dropped" and "finished" events: Haiku does
not tell a drag's source what became of it.

### Background transparency (`transparent_background_color` in OSC 21) — blocked
Needs translucent windows, which app_server does not have.

### Clipboard passwords (OSC 5522 `pw=`) — S
kitty lets a program skip the read confirmation with a password the user
approved once. Terminal asks every time.

## Drawing

### Color emoji — done
Terminal draws them with FreeType from Noto Color Emoji's bitmaps (CBDT) or
a font's COLRv0 layers. Not done: COLRv1 fonts (FreeType's paint API), and
emoji newer than the font (Unicode 15.1 for Noto Color Emoji 2.042), which
fall back to their first component or to the monochrome font.

### Ligatures — M to L
Shaping runs of text with HarfBuzz (a package other applications already
pull in) and drawing glyphs by id, which app_server's `BFont` cannot do; it
means FreeType in Terminal, as for emoji.
kitty also offers `disable_ligatures` under the cursor.

### Drawing without reading back — L
The GPU renderer draws into an offscreen buffer and reads the changed rows
back into a bitmap that app_server copies to the screen. On the X399
(GTX 1070, NVK) reading back a 4K frame takes 12.5 ms, and a whole frame
23.5 ms against 13 ms in software. On the Framework 13 (Radeon 760M,
radeonsi) a 2223 x 1495 frame takes 2.8 ms against 1.8 ms. Terminal draws in
software on both. (Reading back through a pixel buffer object was slower
still: 3.2 ms on the 760M, 175 ms with Zink.) Presenting the GPU's image
directly needs a scanout or shared buffer path per driver (amdgpu has one,
used by Summit; NVK has none).

### Multi-threaded software drawing — done
Large frames are shared out row by row among up to four threads: a 4K frame
on the X399 went from 13 ms to 3.8 ms.

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
