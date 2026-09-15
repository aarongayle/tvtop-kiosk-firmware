# Fonts

Text is Roboto Regular and Roboto Bold (`tools/fonts/*.ttf`), pre-rendered into one blob that is
linked into the firmware as `src/common/font_blob.c` (also written to `assets/fonts.bin` for
tools). `src/common/font.h` documents the byte layout; `src/common/font.c` reads it.

## What the blob contains

| Part | Content | Bytes |
|---|---|---|
| bitmap faces, regular | 10 12 13 14 16 19 22 24 26 px, 2 bpp coverage | 76 532 |
| bitmap faces, bold | same sizes | 80 609 |
| outlines, regular | every charset glyph as TrueType quadratic contours, font units | 37 308 |
| outlines, bold | same | 36 936 |
| headers | 16 + 18 × 16 + 2 × 24 and padding | 379 |
| **total** | | **231 764 (226.3 KB)** |

* Charset: U+0020–U+007E, U+00A0–U+00FF, U+2010–U+2027, U+2212, minus the codepoints Roboto lacks
  (a few of the U+2010 block); 211 glyphs per weight. The space is always present.
* Faces of **22 px and up** hold only ASCII, the Latin-1 letters (U+00C0–U+00FF without × ÷) and
  the punctuation the server emits at heading sizes (° · – — • … −): 164 glyphs. Anything else at
  those sizes measures and draws as a space, exactly like emoji at every size.
* Bitmap metrics: `scale = stbtt_ScaleForMappingEmToPixels(size)` so *size* is the CSS font-size
  (em = size px), matching the SVG reference renderer. Coverage is quantised to 2 bpp with
  `round(cov·3/255)`, rows packed MSB-first; `bx, by` are the bitmap's top-left relative to the
  pen and the baseline (y down); advances and the face ascent/descent are px8.
* Outlines: `units_per_em` (2048), advance in font units, bbox, packed contours (int16 points y-up,
  flag bit0 = on-curve, contour end indices). A closing point equal to the contour start is dropped;
  the rasteriser closes every contour.

The bitmap glyph tables (12 bytes × 211 or 164 glyphs × 18 faces ≈ 41 KB) and the outline point
streams are the two biggest fixed costs; shrinking further would mean a leaner record layout
(delta-coded points, 8-byte glyph records), not fewer sizes.

## Regenerating

```
cc -O2 -Ivendor/stb -Isrc/common tools/fontgen/fontgen.c -o /tmp/fontgen -lm
/tmp/fontgen tools/fonts/Roboto-Regular.ttf tools/fonts/Roboto-Bold.ttf assets/fonts.bin src/common/font_blob.c
```

(`tools/fontgen/CMakeLists.txt` builds the same tool.) The tool prints the per-face byte counts and
the total; commit both generated files. `host/tests/test_font.c` cross-checks the blob against the
size list and charset rules above, so update both when changing them.

## Size selection (snapping)

`font_bitmap_size_for(size_px)`:

* `size_px > KIOSK_FONT_BITMAP_MAX` (26): no bitmap; the outline path is used.
* otherwise the nearest available size, **ties round up**: 11 → 12, 15 → 16, 17 → 16, 18 → 19,
  20 → 19, 21 → 22, 23 → 24, 25 → 26, anything ≤ 10 → 10.

`font_measure`, `font_extent` and `font_draw` all use the snapped face, so layout is consistent
with what is drawn. Bold falls back to regular only if the blob had no bold face of any size.

## Drawing

Both paths share the same positioning: `x8` is the anchor (align 0 left, 1 centre, 2 right —
the offset is `font_measure` of the whole string), `baseline8` is the alphabetic baseline, and the
pen advances in px8.

**Bitmap path** (`size ≤ 26`): per glyph the pen is rounded to a whole pixel and the bitmap is
blitted at `(round(pen) + bx, round(baseline) + by)` with `raster_blit_glyph2`, after a band
intersection test on its box.

**Outline path** (`size > 26`): every glyph is flattened on the fly into a static int16 vertex
buffer of `KIOSK_GLYPH_MAX_VERTS` pairs (GEOM_BREAK between contours) and filled with
`raster_fill_poly_aa(FILL_NONZERO, ss)`, `ss = 3` below 40 px and `2` above. Font units are mapped
to px8 with a 16.16 fixed-point scale `size·8/upem` (int64 intermediates), y flipped about the
baseline. Quadratics follow TrueType semantics (implied on-curve midpoints between consecutive
off-curve points; an all-off-curve contour starts at the midpoint of its last and first points)
and are cut into `n` chords with `n` the smallest 1..16 such that the chord error
`|p0 − 2p1 + p2| / (8n²) ≤ 0.2 px`. If a glyph overflows the vertex budget the subdivision is
halved and retried once; if it still overflows the glyph is skipped (the pen still advances).
Glyphs whose scaled bbox misses the current band are culled before flattening, so a glyph costs
flattening once per band it touches. No glyph cache, no malloc, no recursion.
