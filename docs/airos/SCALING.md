# Fractional scaling

air/OS draws the whole desktop at one density, the render scale
(`HWInterface::RenderScale()`, 100 to 250 percent in 25 percent steps): every
logical coordinate is multiplied by it on its way into the frame buffer, and
monitors that want less are shrunk by the display engine. Whole scales are
easy: every logical pixel is two buffer pixels at 200 percent. At 125, 150
and 175 percent a logical pixel is one buffer pixel in some places and two in
others (`floor(x * scale)` is where pixel `x` starts), which is where the
problems of October 2026 came from. This note says what other systems do,
what air/OS does now and why.

## What others do

| | Text | Thin lines and frames | Programs that render their own pixels |
|---|---|---|---|
| macOS | renders everything at 2x (`backingScaleFactor` is only ever 1 or 2) and the window server shrinks the frame to the panel (a 1.5x look renders 1.78 times the panel's pixels). No hinting; subpixel anti-aliasing removed in 10.14 | uniform, as the 2x picture is shrunk as a whole, but soft | at 2x, then shrunk |
| Wayland (fractional-scale-v1 + viewporter) | clients render at the exact scale into buffers of physical pixels. GTK 4.16: grey anti-aliasing, slight hinting, unhinted metrics, glyph origins on quarter pixels horizontally and whole pixels vertically | GTK leaves borders unsnapped (anti-aliased) by default; 4.24 can snap per edge on request. Qt snaps nothing (QTBUG-86344) | X11 programs under Xwayland are enlarged (blurry) unless the toolkit scales itself (KWin's "apply scaling themselves") |
| Chromium | slight hinting is turned off and glyphs are positioned on subpixels at any device scale above 1 | box edges round to the nearest device pixel; border widths are floored to whole device pixels (1 px at 125-175 percent) | n/a |
| Windows | DirectWrite: vertical-only grid fitting, subpixel positions | WPF/WinUI layout rounding: border thickness rounded to whole pixels (2 px at 150 and 175 percent) | programs that are not DPI aware are stretched (blurry) |

The common ground: grey anti-aliasing at high density, vertical-only hinting
with unhinted advances, glyphs on whole device pixels vertically, lines of
whole device pixels, and real pixels for programs that render their own.

## What air/OS does

**Text** (`ServerFont::IsDeviceScaled()`, `FontEngine`, `Painter::_DeviceBaseLine()`).
Programs measure at the logical size without hinting. The Painter renders
the same string with a font that many times larger, hinted vertically only
(`FT_LOAD_TARGET_LIGHT`) at the exact fractional pixel size, with unhinted
advances, so the string is as wide as measured. Its glyphs come from the
glyph cache as bitmaps (before, every glyph was rasterized from its outline:
a text view redraw took 5 to 7 ms in QEMU at 150 to 200 percent, now about
1 ms). The base line goes on the top edge of the pixel row where filled
rectangles put it; before it fell between rows at 125, 150 and 175 percent
and blurred every horizontal stroke.

**Anti-aliasing.** On a scaled desktop subpixel anti-aliasing is off for text
and shapes (`subpixel_antialiasing_active()`): most edges fall between pixels
there, where the colour stripes turned into fringes on round rectangles,
circles and text, and the buffer's pixels are not always the panel's.

**Frames** (`AS_STROKE_FRAME_LINEARRAY`, `HaikuControlLook::FrameLines`). Each
one pixel ring drawn alone is one or two buffer pixels thick depending on
where it falls, so a button's outline was 2 px on one side and 1 px on the
other. Nothing in app_server can fix that alone: rings and fills are separate
calls in logical pixels, and the next ring or the child view starts where
the grid says. The control look therefore sends the rings of a frame
together, with the band they cover. app_server keeps the band's outer and
inner edges on the grid, so everything around and inside still meets it,
and shares out its pixels the same way on every side: the dark outline gets
the scale rounded (2 px at 150 and 175 percent), the faint edge outside it
what is left. Two logical pixels are always three buffer pixels at 150
percent, so there outline and edge come out the same on all four sides of
every button, text control, check box, scroll view, menu field and tab. Bevels
are drawn over their surface, which is filled first, and keep their width.
A tab view draws its selected tab last, as its neighbours' separators share
a column with it. Window borders (`DefaultDecorator`) send their five rings
per side the same way: the dark outlines keep their width (2 px at 150 and
175 percent; the top ones were 1 px at 175), the middle ring takes the rest.
Lines a program draws itself keep the grid (one or two pixels).

**Separators and edges** (`FrameLines.h`, `stroke_line_over_fill()`). A
line that separates two areas or ends one is drawn after what it lies on,
over that fill, with its rounded width from its outer edge inwards; it
also covers its own logical pixel (`kFrameLinesCoverPixels`), so nothing
is left undrawn in a transparent view. That way the bottom border of menu
bars (drawn again over the background), the bottom line and column
separators of Tracker's and `BColumnListView`'s headers, the dark ends of
a scroll bar's track and a disabled thumb's bevel are 2 pixels at 150 and
175 percent wherever the window is. A menu separator's dark and light
lines are one band (2 + 1 pixels at 150 percent), as are a slider bar's
frame and edge; the slider thumb's outline is drawn again over the thumb.
A scroll bar lies on the dark ring of a fancy `BScrollView` frame; on those
sides it draws that ring with the frame's two ring band (the faint edge
outside it included), so the frame and the scroll bar agree on its pixels
(the dark line there was 2 or 3 pixels). A plain frame (`BScrollView`,
`BColumnListView`) is one ring around views that cover all of its inside,
so it cannot grow inwards: it gets the whole part of the scale (1 pixel at
125 to 175 percent, as Chromium floors border widths) against those views
on every side (`kFrameLinesHugInside`), and the background fills what its
pixel leaves outside; so do plain `BBox` frames, `BColumnListView` without a
border, plain separators, the inactive tabs' bevel, the slider thumb's
shadow and the text caret (inverted as such a band, it was one or two
pixels wide depending on where it was in the text). `BSeparatorView`'s
fancy separators are a band of edge and dark line, as are boxes too thin
for their rings (dividers). A square default button's indicator and the
background between it and the edge are rings of the button's band; the
background takes up what the scale leaves (`kFrameLinesOuterSlack`), so
the indicator and the outline keep their widths. Calendars (`BCalendarView`)
and status bars draw their frames over what is inside them. Scroll bars over
a focused frame draw its ring in the focus colour, and also recognise
`BColumnListView`'s fancy frame.

Checked with `parshot.sh` and `linepar.py`: a window is captured, moved by
one logical pixel and captured again, and every line that changed width is
listed. A survey of the X399's programs at 175 percent (58 with windows;
October 2026, master after the plain-frame work): 50 have no dark line
whose width depends on the window's position; the others draw those lines
themselves (Amp, Icon-O-Matic's colour swatches, People's picture frame,
SoundRecorder, Workspaces, Backgrounds' preview, Burrow's separator; E-mail's
one hit is an artifact of the comparison). Of the 36 programs surveyed
before the last of these changes, 21 had such lines (separators, tab
bevels, slider shadows, default buttons, list frames, scroll bar
junctions); 18 of them have none now.

**Translucent lines** (`B_OP_ALPHA`, blended frames) cover whole buffer pixels
like opaque ones instead of being rasterized between pixels.

**Moving windows** (`Desktop::_CopyWindowApproximately()`). Most one pixel
moves are a fraction of a buffer pixel at fractional scales, and a copy can
only move whole pixels, so a dragged window used to be redrawn by its
program at every step. While the user drags, the content now moves by the
whole number of pixels its frame moves, a band of two logical pixels along
the edges of the copy is redrawn, and the window is redrawn once when the
drag ends. A 300 step drag in QEMU at 150 percent: app_server 1525 ms and the
program 829 ms of CPU time before, 263 and 20 ms now.

**Programs that render their own pixels.** `BScreen::DeviceScale()` says how
many buffer pixels a coordinate unit is (from the memory app_server shares
with applications; `B_SCREEN_CHANGED` carries it as `"device scale"`). A
bitmap drawn with `B_DEVICE_PIXEL_BITMAP` holds buffer pixels and is drawn one
to one from the buffer pixel the destination's top left corner falls on.
The Qt platform plugin (airos-works/qt6-haikuplugins, branch `airos`, packaged
as qt6_base 6.10.3-103) reports the scale as the device pixel ratio, keeps
its backing store at that density and draws it this way, and uses grey,
vertically hinted text there. Qt programs such as Telegram were rendered at
the logical size and enlarged before, which made them fuzzy at any scale.

**Enlarged bitmaps** (`BilinearSeparable` in `DrawBitmapBilinear.h`). A
program that does not know the scale draws bitmaps in logical pixels and
app_server enlarges them. Without a filter the nearest pixel doubles every
other row and column at fractional scales, so a one pixel line inside the
bitmap is one or two pixels wide. A `BGLView` (libglvnd's Haiku GL kit,
airos-works/libglvnd `airos-1.7.0`) therefore asks for
`B_FILTER_BITMAP_BILINEAR` when the scale is fractional, as macOS and Wayland
compositors filter surfaces that are not drawn at the panel's density; whole
scales stay exact. On x86_64 that filter was the per pixel default version
(the SIMD one is i386 assembly), 3.5 ns per buffer pixel; it now filters
each row in two passes, four pixels at a time with SSE2, about four times
faster, and its weights add up to 256 so white stays 255 (it was 253). On
the Framework 13 at 175 percent, a 400 x 300 bitmap filtered to the frame
buffer takes 0.43 ms instead of 1.65 (0.21 unfiltered), and GLTeapot costs
app_server 1.30 s of CPU time per 10 s filtered, 1.19 unfiltered (1.92 with
the old filter).

**Bitmaps drawn one to one** (`BitmapPainter::_DrawAreaSampled()`). Most
programs that draw bitmaps draw them at their own size: offscreen views
for flicker-free drawing (Cortex, Clock, ActivityMonitor's graphs, the
switcher), raster pictures and icons, and surfaces of programs drawn
through the in-process Wayland server. Enlarged with the nearest pixel, the
strokes of their text and their one pixel lines were one or two buffer
pixels wide; with bilinear filtering they would be even but soft, the
fuzziness programs that are not aware of the scale have on Windows and
under Xwayland. These copies are area sampled instead: a buffer pixel takes
the average of the bitmap pixels it covers at their exact fractional
positions, so a pixel inside one bitmap pixel keeps its colour and only
those on its borders are mixed (what emulators call sharp bilinear). Every
bitmap pixel is 1.75 buffer pixels wide at 175 percent and the picture
stays as sharp as the scale allows. Positions come from the whole bitmap on
the desktop's grid, so redrawing part of an offscreen view gives exactly
the pixels drawing all of it does (host tests: within 3 levels of exact
area sampling, no difference between partial and full draws). Alpha
blended bitmaps are mixed with their alpha applied, so transparent pixels
add no colour. A 400 x 300 bitmap drawn one to one costs 0.40 ms at 175
percent on the Framework 13. Programs that ask for a filter or a scale of
their own, vector icons and `B_DEVICE_PIXEL_BITMAP` keep what they had.

**GTK, Firefox and other Wayland programs** (airos-works/wayland-server,
branch `airos`, on X547's in-process Wayland server). The server reported
`wl_output` scale 1 and drew every buffer one to one, so GTK and Firefox
drew at the logical size and app_server enlarged them (pixel doubling even
at 200 percent). It now reports the device scale rounded up (2 at 125 to
200 percent) with the mode in pixels of that scale; GTK 3 then draws its
buffers at twice the density (`set_buffer_scale`), and a surface covers
its buffer size divided by the scale: one to one at 200 percent, filtered
down at 150 and 175, as macOS draws at 2x and shrinks. Buffer damage,
viewport sources and cursors follow the scale, and a viewport destination
without a source (Firefox's content) is honoured. When the desktop's scale
changes, the outputs say so and surfaces have entered them, so a running
GTK program follows (200 -> 100 -> 200 percent: buffer scale 2 -> 1 -> 2).
airos-ci builds it with the AMD GPU user land and puts it into
`system/non-packaged/lib` of every x86_64 image, ahead of HaikuPorts'
`wayland_server`.

**What the programs on the laptop and the X399 look like at 150 and 175
percent** (October 2026, device pixel captures): native Interface Kit
programs, preferences and Deskbar replicants are drawn at the device
density (Terminal, Tracker, HaikuDepot, Mail, Pe, Vision, Debugger, ShowImage,
Icon-O-Matic, Screen, Media, Network, Keymap, Bluetooth, Wi-Fi and the other
preferences); Qt programs (Telegram, Qt6Configurator) through the Qt
plugin; WebPositive and Summit render their pages at device density; video
(airTime, CodyCam) is filtered; GL views (GLTeapot, GLInfo) are filtered;
programs with offscreen views or raster pictures (Cortex, Clock, Pulse) are
area sampled; GTK programs and Firefox draw at 2x through the server above.

## What is left

- One pixel lines a program draws itself (custom views, the primitives in
  `gallery`) are still one or two buffer pixels thick depending on where
  they fall; only the kit knows which lines belong together.
- Lines that are a gap between child views, painted in the parent's view
  colour (the edge of Tracker's windows, Deskbar's separators), are 1 or 2
  pixels. A plain `BColumnListView` frame is 1 pixel and its scroll bars'
  outer sides 2 (both every time). Faint rings that take up what the scale
  leaves next to a ring that keeps its width (a box's outer edge, the bevel
  above a menu bar's border, a thumb's highlight next to the scroll bar's
  border) vary between 1 and 2 pixels; the survey counts them separately.
- Rounded default buttons (a control look other than the default) keep
  their stroked indicator.
- Scrolling by an odd number of logical pixels at 150 percent (most steps at
  125 and 175) still redraws the view instead of copying it; with glyphs from
  the cache that is cheap for text, less so for large images.
- OpenGL views and other programs that draw logical-size bitmaps are still
  enlarged (GL filtered, other bitmaps area sampled: even, but with the
  detail of the logical size). Programs can use `BScreen::DeviceScale()` and
  `B_DEVICE_PIXEL_BITMAP` as the Qt plugin does. `FULL-DENSITY.md` designs
  the rest (not implemented): opt-in `BBitmap` and `BGLView` flags (what
  macOS calls `wantsBestResolutionOpenGLSurface`, SDL
  `SDL_WINDOW_ALLOW_HIGHDPI`), ports of the system programs, and dense
  offscreen buffers in app_server for programs that do not ask.
- GTK 3 only knows whole scales, so at 125 percent it draws at 2x and is
  filtered down to 62.5 percent of that.
- Qt: the x86_64 images carry qt6_base with the plugin that uses the device
  pixel ratio (HaikuPorts' revision plus 100, airos-ci
  `deps/build-qt-airos.sh`), so Telegram from HaikuDepot is sharp. ARM64
  images have no Qt yet, and HaikuPorts' next Qt version replaces ours (and
  draws at the logical size again) until an image brings the next one.

## Tools

`docs/airos/scaling` (cross-build with `build-test.sh <name>`):

- `gallery [x y] [--quit-after s]`: standard controls and primitives,
  including a checkerboard of single buffer pixels drawn with
  `B_DEVICE_PIXEL_BITMAP`;
- `devshot file.png [x y w h]`: the screen in buffer pixels (the screenshot
  APIs average a scaled desktop down to its logical size);
- `scalebench [n]`: text view, bitmap and window redraws, moves and scrolls;
- `teamtime [name]`: a team's CPU time so far, for drag tests;
- `parshot.sh` (on the device) and `linepar.py` (on the host, Python with
  PIL): capture a window, move it by one logical pixel, capture it again,
  and list the lines whose width changed (example: `parshot.sh FileTypes 1
  300 200 /boot/home/par-ft 1.75`, then `linepar.py 1.75 par-ft-a.png 300
  200 par-ft-b.png 301 201 <what parshot.sh printed>`; with the scale it
  captures only the window, about 15 seconds per program). At 175 percent
  the grid repeats every four logical pixels, so check from two places.

A QEMU loop: boot the CI x86_64 image with the new `haiku.hpkg` swapped in
with `bfs_shell --start-offset 6291456`, set `screenmode -q 1920 1080 32`,
then `screenmode --display 1 --scale 150`, and use QMP `screendump` or
`devshot`. The live image keeps nothing across a reboot.
