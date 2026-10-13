# Fractional drawing regression checks

The October 2026 review found these causes of poor edges:

- Moving or scrolling by one logical pixel at 175% copied the old pixels by
  a rounded two device pixels. Repeating that operation accumulated drift.
  `DrawingEngine::CopyRegion()` now reports whether an exact copy is possible;
  its callers redraw when the translation is fractional or crosses the buffer.
- Solid borders and clipping used integer device edges, while a rectangular
  gradient used fractional edges. At 125% the gradient partially overwrote
  the right and bottom borders. Aligned rectangular paths now use the same
  device boundaries as solid fills. Transformed/subpixel paths retain their
  antialiasing.
- A logical screen rounded to 356 rows at 225% can cover 801 physical rows on
  an 800-row framebuffer. Device clipping and the background fill now respect
  the actual buffer size. Single-display reporting retains the real hardware
  timing instead of reconstructing it from rounded logical dimensions.
- Sequentially scaling two displays down in logical size left their old
  origins in place. The growing gap could exceed the NVIDIA framebuffer
  allocation limit. Scale changes now close gaps unless the request also
  changes display positions. Explicit positions are retained.

## Vector icon rendering

`BIconUtils` keeps the vector source alongside the usual bitmap pixels.
`BBitmap` copies, full imports and archives preserve that optional information
without changing its public object layout. Button trimming, active/disabled
states and Tracker's selected state preserve the corresponding crop/effects.
RGB alpha behaviour and palette conversion are retained.

The app_server rasterizes this source at the requested drawing density and
caches one raster per bitmap. The cache is replaced when the density changes.
Saved pixel snapshots invalidate the vector representation when a client edits
`Bits()` directly. Bitmaps accepting views and overlays use their ordinary
bitmap path. Size and effect-count limits bound individual representations;
allocation or rendering failure falls back to the bitmap.

A bitmap-only icon has no vector detail to recover. Mixed-density monitors
still share one desktop rendering density; this change does not introduce a
separate renderer for each monitor. Since 2026-10-10 that density is the
largest display scale the display engine can show (`DisplayLayout::
RenderScales()`, tried in turn by `Desktop::_SetDisplayLayout()`), so the
most scaled monitors are drawn one to one and the others are shrunk by the
hardware; see the STATUS.md entry of that day.

## Running the visual test

Cross-build with `tools/build-fractionalscale-test.sh`. Its default build is
`/mnt/HaikuWork/build/master-x86_64`; `BUILD`, `TOOLS` and `OUT` can override the
build directory, compiler prefix and output directory. Copy the executable to
a fresh name on the target, or upload to a temporary name and rename it, so a
running image is never overwritten in place.

Run in an unobscured desktop, with the screen saver dismissed. The window must
fit completely on screen. For example, after setting the display to 175%:

```
fractionalscale /boot/home/tests/scale175.ppm 8 1.75
```

The test saves device-pixel captures and checks 64 border samples and eight
icon comparisons: original, copy constructor, archive/unarchive, full bitmap
import, normal button, disabled button, active button and disabled-active
button. References are independently rasterized at physical size. It repeats
these checks after eight one-pixel moves, eight one-pixel scrolls and a raw
pixel mutation. Success means every border sample passes, every comparison has
zero differing pixels, and the program exits with status zero.

Additional cases:

```
FRACTIONAL_ICON_FORMAT=rgb fractionalscale rgb175.ppm 8 1.75
FRACTIONAL_ICON_FORMAT=cmap8 fractionalscale cmap175.ppm 8 1.75
```

On a single-display test desktop initially at 100%, keep the same source icons
alive across changes of density:

```
FRACTIONAL_SCALE_SEQUENCE=125,150,175,200,225,250,175,100 \
    fractionalscale live.ppm 0 1
```

This last command changes display 1. Restore the original display arrangement
after testing. Do not run two visual tests concurrently: they change the same
desktop and can obscure one another.

QEMU verification used its standard VGA framebuffer at 1280x800, all seven
scales from 100% through 250%, the live sequence, RGB and palette icons at
175%, and `displaylayouttest` (including rounded single-display timing).
The changed libraries and app_server were also cross-built for ARM64; that is
build coverage, not an ARM hardware rendering test. Native X399 results are
recorded in `STATUS.md`.
