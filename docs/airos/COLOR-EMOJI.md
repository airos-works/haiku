# Color emoji

app_server draws emoji in color in every application: Tracker, Deskbar,
window titles, menus, `BTextView` (StyledEdit, Mail), Scintilla (Kiri) and
whatever else draws text with `BView::DrawString()`. Flags, skin tones,
keycaps, subdivision flags and joined sequences (families, professions) are
one glyph. Since branch `color-emoji` (2026-10-26).

Before, app_server drew every glyph in grey levels, so emoji came from the
monochrome Noto Emoji (or Noto Sans Symbols) as line drawings, and a
sequence showed its parts side by side.

## Fonts

Any color font installed in a font directory works:

- **Noto Color Emoji** (HaikuPorts' `noto_emoji_color`, in air/OS images)
  ships twice, as color bitmaps (`NotoColorEmoji.ttf`, CBDT) and as color
  vector glyphs (`Noto-COLRv1.ttf`, COLR version 1). Both have the family
  name "Noto Color Emoji"; app_server takes the first one it scans, which
  is the vector font on air/OS. Both are drawn alike.
- CBDT and sbix bitmap fonts: FreeType decodes the PNG strike, app_server
  scales it to the size (area averaging down, bilinear up).
- COLR version 0 layers: FreeType blends them.
- COLR version 1 paint graphs: `font/ColorGlyphRenderer` paints them with
  AGG, as FreeType only reads them: layers, solid colors, linear, radial and
  sweep gradients with pad, repeat and reflect, glyph clips, transforms
  (translate, scale, rotate, skew, matrix) and all composite modes. The
  text color (palette index 0xffff) is black, since glyphs are cached
  without it.

The fallback list (`GlyphLayoutEngine::PopulateFallbacks()`) has "Noto Color
Emoji", "Twemoji Mozilla" and "Twemoji" after the Noto Sans Symbols fonts
and before the monochrome "Noto Emoji".

## Presentation

As in UTS #51:

- a character with the Emoji_Presentation property (😀, 🔥), one followed by
  U+FE0F (❤️, ☺️), a skin tone, a keycap or a flag is drawn from the color
  font even when the text's own font has a glyph for it;
- a character that is text by default (❤, ☺, ✔) is drawn from the text
  fonts, and only falls back to the color font when no text font has it;
- U+FE0E asks for text.

The tables come from the Unicode emoji data:
`src/kits/shared/gen_emoji_tables.py emoji-data.txt > EmojiTables.cpp`
(`headers/private/shared/EmojiTables.h`, Unicode 16.0).

## Sequences

`GlyphLayoutEngine::LayoutGlyphs()` collects an emoji cluster (a regional
indicator pair, a keycap, or an emoji with variation selectors, skin tones,
tags and further emoji after zero width joiners, `emoji_cluster_length()`),
drops the variation selectors (the font does not map them) and asks the
color font for the longest prefix it joins into one glyph:
`font/OpenTypeLigatures` applies the ligature lookups of the font's 'ccmp',
'liga', 'clig' and 'rlig' features, which is how Noto Color Emoji and
Twemoji join their sequences. The first character of the cluster gets the
glyph and its advance; the others get a glyph of zero width. So
`BFont::GetEscapements()` of "👍🏽" is `1.245, 0`, and the string's width is
one emoji's. A sequence the font does not know falls back to its parts.

Each font cache entry remembers the glyph of every sequence it saw
(`FontCacheEntry::CachedSequence()`), and that there is none, so the
ligature lookup runs once per sequence and size. Color glyphs are cached
under `kColorGlyphKey | glyph index`.

## Drawing

Color glyphs are stored as RGBA (`glyph_data_color`) at the size they are
drawn and blended by `AGGTextRenderer::_DrawColorGlyph()` through the
drawing mode, their alpha as the coverage. `B_OP_COPY`, `B_OP_OVER` and
`B_OP_ALPHA` keep the colors; with `B_OP_ALPHA` the high color's alpha makes
them translucent as it does text. Rotated, sheared and scaled text samples
the glyph through the transform. Clipping regions and `ClipToPicture()`
masks apply.

Halos, outlines and shadows of text are drawn with a false bold width or in
an offset translucent color before the text. A color copy of an emoji there
would smear it, so with a false bold width, or with the new font flag
`B_COLOR_GLYPHS_AS_SHAPES`, color glyphs are drawn as their shape in the high
color (grown by the false bold width). Tracker's desktop labels and the
control looks' desktop labels set the flag for their shadow.

## Editing

`BTextView` moves the caret, selects and deletes an emoji cluster as one
character (`_NextInitialByte()`, `_PreviousInitialByte()`), and its width
cache (`WidthBuffer`) measures a cluster as a whole and the parts of
sequences one at a time, so that its line layout matches what app_server
draws.

## Applications

- **Terminal** draws emoji itself, fitted into cells (see
  [TERMINAL.md](TERMINAL.md)).
- **Summit** pages use Skia and the browser's bundled color fonts; its
  tabs, location bar and menus are drawn by app_server.
- **Mail** used an embedded monochrome Noto Emoji and shaped sequences with
  HarfBuzz into private fonts. It now draws an emoji once at start; when the
  system draws it in color, Mail leaves emoji to the system fonts.
- **Kiri**: Scintilla and its terminal draw with `DrawString()`.

## Testing

Off the machine, the glyph code builds on Linux against the host FreeType:
a harness renders sequences from both Noto Color Emoji files to PNG, and a
fuzzer mutates their GSUB and COLR tables under AddressSanitizer. A test
image (`build/emoji-x86_64`, QEMU) checked sizes from 9 to 120 pixels,
the drawing modes, bold, sheared and rotated text, escapements, BTextView
caret steps and Backspace, window titles and desktop labels.
