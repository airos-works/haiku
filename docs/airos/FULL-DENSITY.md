# Full density for programs that draw at the logical size

Status: design, not implemented (October 2026). See `SCALING.md` for what
fractional scaling does today.

## The problem

On a scaled desktop app_server draws views at the frame buffer's density.
Two kinds of programs do not get that, because the pixels they show are not
drawn by app_server in the window:

- **Offscreen views.** A program draws into a `BBitmap` with
  `B_BITMAP_ACCEPTS_VIEWS` (flicker-free drawing, cached graphs, drag images)
  and draws that bitmap into its window. The bitmap has the logical size, so
  app_server enlarges it: Cortex's labels, ActivityMonitor's graphs, the Deskbar
  switcher, Tracker's drag images, RemoteDesktop's remote screen.
- **OpenGL.** `BGLView` renders a frame of the view's logical size and draws it
  as a bitmap (GLTeapot, GLInfo, games).

Since October 2026 such bitmaps are area sampled (one to one) or filtered (GL),
so they are even, but they keep the detail of the logical size. macOS, Windows
and Wayland behave the same for programs that do not ask for more:
`wantsBestResolutionOpenGLSurface`, per-monitor DPI awareness,
`wl_surface.set_buffer_scale` and `wp_fractional_scale_v1` are all opt-in.

## What already exists

- `BScreen::DeviceScale()`: buffer pixels per coordinate unit.
- `B_DEVICE_PIXEL_BITMAP`: a bitmap that holds buffer pixels, drawn one to one.
- The Qt plugin, Summit and the Wayland server use them; a program can create
  its offscreen bitmap `DeviceScale()` times larger, `SetScale()` its view and
  draw the bitmap with `B_DEVICE_PIXEL_BITMAP`.

## Option A: opt-in, and porting our own programs

1. A `BBitmap` flag (`B_BITMAP_DEVICE_DENSITY`) that makes a view-accepting
   bitmap of a logical size allocate the frame buffer's density, scale its views
   and be drawn one to one; `Bits()` then has the larger size, which only a
   program that sets the flag sees.
2. A `BGLView` option (`BGL_DEVICE_PIXELS`): the surface gets
   `Bounds() * DeviceScale()` pixels and the frame is drawn with
   `B_DEVICE_PIXEL_BITMAP`; the program sets its viewport from
   `BGLView::DrawableSize()` (new). SDL's Haiku backend maps
   `SDL_WINDOW_ALLOW_HIGHDPI` to it.
3. Port the system programs: ActivityMonitor (graph and replicant), Cortex,
   Pulse, the Deskbar switcher, Tracker's drag images, RemoteDesktop.

Lessons from a first ActivityMonitor attempt (not merged): the graph's line
array, drawn segment by segment with butt caps under `SetScale(1.75)`, showed
seams at every joint (use one `BShape` path or round caps when scaled), and a
bitmap of `ceil(size * scale)` pixels must be filled completely (its last row
lies outside the logical rectangle the code fills).

Risk: low, per program. Cost: days for the API, an hour or two per program.

## Option B: full density without the program's help (offscreen views)

app_server keeps a second, dense buffer for every view-accepting bitmap and
draws its views there; the bitmap the program sees stays at the logical size.

- **Drawing.** `OffscreenWindow` gets a `BitmapHWInterface` over the dense buffer
  with the desktop's render scale; `DrawingEngine` and `Painter` then draw it as
  they draw a window on the scaled desktop (no new drawing code).
- **The program's copy.** After each drawing batch (and on `Sync()`), the dirty
  device region is reduced into the logical buffer (area average, premultiplied
  for alpha), so `Bits()` after `Sync()` holds what it held before, nearly
  (anti-aliasing reduced from the dense drawing instead of drawn at 1x).
- **Writes by the program.** Many programs draw and then change `Bits()`
  themselves (alpha fix-ups of drag images, `SetBits()`, `ImportBits()`). app_server
  cannot see those writes. It keeps a hash of the logical buffer after its own
  last reduction and compares it before using the dense copy and before the
  next drawing batch; on a difference the logical buffer is enlarged into the
  dense one (area sampled) and drawing continues from there. The hash costs a
  read of the logical buffer per batch (0.5 ms for 8 MB).
- **Drawing the bitmap.** `Painter::DrawBitmap` of such a bitmap uses the dense
  copy (the `B_DEVICE_PIXEL_BITMAP` path for one to one, its scale divided by the
  bitmap's density otherwise) while the hash still matches.
- **Limits.** Only `B_RGB32`/`B_RGBA32`, only on a desktop scaled above 100
  percent, and below a size cap (memory grows with the scale squared: 3 times
  at 175 percent). A bitmap keeps the density it was made at; one drawn on a
  desktop of another scale is filtered.

Risks: every program with offscreen views changes (Tracker, Deskbar, media
players, games). Wrong hashes or missed writes show stale pictures; the
reduction costs CPU at every drawing batch. Test plan: the gallery tools,
Cortex, ActivityMonitor and its replicant, Tracker icon and drag images, the
switcher, RemoteDesktop, BeOS-era games that draw into bitmaps every frame;
scalebench and teamtime before and after; a kill switch (`no_dense_offscreen`
in the app_server settings) for the first releases.

Estimate: a week, with the test plan.

## Option C: OpenGL without the program's help

Give `BGLView` a surface at the frame buffer's density and rewrite the
program's pixel coordinates in libGL (glvnd's dispatch): `glViewport`,
`glScissor`, `glReadPixels`, `glCopyTex(Sub)Image`, `glBlitFramebuffer`,
`glWindowPos`, `glLineWidth`, `glPointSize` and the queries of their state,
only while framebuffer 0 is bound. Shaders that use `gl_FragCoord` or a
viewport size the program passes itself would still be wrong, as would
programs that read the surface size from elsewhere.

Not recommended: Option A's `BGL_DEVICE_PIXELS` (and SDL's high-DPI flag) gives
the same result for programs that ask, without breaking the others.

## Recommendation

Option A first: the flags, then our own programs, then SDL. Option B as a
separate project with its kill switch, once the opt-in path shows how much is
left.
