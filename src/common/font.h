// Text: Roboto Regular/Bold from a generated blob (tools/fontgen → src/common/font_blob.c).
//
// Blob layout (little-endian, all offsets from blob start):
//   header  { char magic[4]="TVFN"; u16 version=1; u16 nfaces; u32 outlines_off; u32 faces_off; }
//   faces[nfaces] { u8 size_px; u8 bold; u16 nglyphs; i16 ascent8; i16 descent8; u32 glyphs_off; u32 bits_off; }
//   glyph  { u16 cp; u8 w; u8 h; i8 bx; i8 by; u16 adv8; u32 off; }   // 12 bytes, sorted by cp
//          bitmap at bits_off+off: h rows, each ceil(w*2/8) bytes, 2 bpp coverage, MSB = leftmost
//          bx/by: offset of the bitmap's top-left from the pen position in device space (y grows
//          down): left = pen_x + bx, top = baseline_y + by. adv8/ascent8/descent8 are px8;
//          descent8 is stored as the font gives it (negative, below the baseline).
//   outlines: 2 faces (regular, bold) { u16 units_per_em; i16 ascent; i16 descent; u16 nglyphs;
//          u32 glyphs_off; u32 points_off; u32 flags_off; u32 ends_off; }
//     oglyph { u16 cp; u16 adv_fu; i16 x0,y0,x1,y1; u16 npts; u16 ncont; u32 pts_off; u32 flags_off; u32 ends_off; }
//          sorted by cp. The three oglyph offsets are byte offsets *relative to the face's*
//          points_off / flags_off / ends_off (the face offsets are absolute).
//          points: int16 x,y pairs (font units, y up); flags: 1 byte per point (bit0 = on-curve);
//          ends: u16 index of last point of each contour, strictly increasing. TrueType quadratic
//          semantics (implied on-curve midpoints between consecutive off-curve points); a contour
//          is closed implicitly (no repeated start point).
//   Faces appear regular sizes ascending, then bold; every offset is little-endian and read
//   byte-wise, so the blob needs no alignment.
//   Contents (docs/FONTS.md): bitmap sizes 10 12 13 14 16 19 22 24 26 px per weight, charset
//   U+0020-007E, U+00A0-00FF, U+2010-2027, U+2212 minus what Roboto lacks; faces >= 22 px carry
//   only ASCII, Latin-1 letters and a few punctuation marks (flash budget). Outlines hold the
//   full charset for both weights. Nothing here depends on that list: faces are discovered from
//   the header and glyphs are looked up per face.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "kiosk_config.h"
#include "raster.h"

extern const uint8_t font_blob[];
extern const uint32_t font_blob_size;

bool font_init(const uint8_t *blob, uint32_t size);   // validates; call once with font_blob

// Nearest bitmap size to size_px (ties up), or -1 if size_px > KIOSK_FONT_BITMAP_MAX.
int font_bitmap_size_for(int size_px);
// Advance width of a UTF-8 string in px8 at size_px (bitmap metrics when a bitmap face is used,
// outline metrics otherwise). Missing glyphs advance by the space width.
int32_t font_measure(const char *utf8, size_t len, int size_px, bool bold);
// Draws a single line of text. x8 is the anchor; align 0 left, 1 centre, 2 right; baseline8 is
// the alphabetic baseline. Renders bitmap glyphs (raster_blit_glyph2) for small sizes and
// outline glyphs via raster_fill_poly_aa otherwise. Scratch: uses r->cov and an internal vertex
// buffer of KIOSK_GLYPH_MAX_VERTS.
void font_draw(raster_t *r, int32_t x8, int32_t baseline8, const char *utf8, size_t len, int size_px, bool bold, uint8_t align, uint8_t idx, uint8_t alpha);
// Vertical extent of a line at size_px, px8: ascent is positive above the baseline and descent is
// positive below it (top = baseline - ascent, bottom = baseline + descent). For culling.
void font_extent(int size_px, bool bold, int32_t *ascent8, int32_t *descent8);

// UTF-8 decoding helper (shared with the frame decoder).
uint32_t utf8_next(const char **s, const char *end);
