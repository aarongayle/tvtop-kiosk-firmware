// Band rasteriser: draws ops into an 8 bpp (palette index) band of KIOSK_BAND_LINES scanlines.
// All coordinates are device-space px8 unless the name says _px. Everything is clipped to the
// current clip rect ∩ band. Fills with alpha < 255 blend against the pixels already in the band
// through palette_blend.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "kiosk_config.h"
#include "palette.h"

enum { FILL_NONZERO = 0, FILL_EVENODD = 1 };

typedef struct {
    uint8_t *band;                 // KIOSK_BAND_LINES * width bytes
    uint16_t width, height;        // output size in px
    int16_t y0, lines;             // current band: rows [y0, y0+lines)
    int16_t clip_x0, clip_y0, clip_x1, clip_y1;   // px, exclusive max
    palette_t *pal;
    // scratch for polygon crossings: lines * KIOSK_MAX_CROSSINGS int16 + lines counts
    int16_t *cross; uint8_t *ncross;
    // scratch for AA coverage: one row of width uint8
    uint8_t *cov;
    uint32_t stats_edge_visits, stats_spans;
} raster_t;

#define RASTER_SCRATCH_BYTES (KIOSK_BAND_LINES * KIOSK_MAX_CROSSINGS * 2 + KIOSK_BAND_LINES + OUT_MAX_W)

void raster_init(raster_t *r, uint8_t *band, uint8_t *scratch, uint16_t width, uint16_t height, palette_t *pal);
void raster_begin_band(raster_t *r, int16_t y0, int16_t lines, uint8_t bg_idx);
void raster_set_clip(raster_t *r, int16_t x0_px, int16_t y0_px, int16_t x1_px, int16_t y1_px);
void raster_clear_clip(raster_t *r);
// True if a bbox [y0,y1) px intersects the current band and clip (cheap culling for callers).
bool raster_band_intersects(const raster_t *r, int32_t x0_px, int32_t y0_px, int32_t x1_px, int32_t y1_px);

void raster_span(raster_t *r, int16_t y_px, int32_t x0_px, int32_t x1_px, uint8_t idx, uint8_t alpha);   // [x0,x1)
void raster_fill_rect(raster_t *r, int32_t x8, int32_t y8, int32_t w8, int32_t h8, int32_t radius8, uint8_t idx, uint8_t alpha);
void raster_fill_circle(raster_t *r, int32_t cx8, int32_t cy8, int32_t r8, uint8_t idx, uint8_t alpha);
void raster_stroke_circle(raster_t *r, int32_t cx8, int32_t cy8, int32_t r8, int32_t w8, uint8_t idx, uint8_t alpha);
// Line segment with butt caps and width w8 (a rotated rectangle); zero length draws nothing.
void raster_line(raster_t *r, int32_t x0_8, int32_t y0_8, int32_t x1_8, int32_t y1_8, int32_t w8, uint8_t idx, uint8_t alpha);
// Polygon from a vertex stream (int16 px8 pairs; x == GEOM_BREAK starts a new contour). Every
// contour is closed. `rule` is FILL_NONZERO or FILL_EVENODD.
void raster_fill_poly(raster_t *r, const int16_t *verts, uint32_t count, uint8_t rule, uint8_t idx, uint8_t alpha);
// Anti-aliased variant for glyphs/icons: exact horizontal coverage, `ss` sub-rows per pixel row
// (1..4). Coverage is quantised through palette_blend.
void raster_fill_poly_aa(raster_t *r, const int16_t *verts, uint32_t count, uint8_t rule, uint8_t ss, uint8_t idx, uint8_t alpha);
// Placed geometry: a uniform scale about the device-space canvas origin (ox8, oy8), then a move to
// (tx8, ty8). p' = t + (p - o) * s_q10 / 1024, px8. s_q10 is the wire scale in 1/1024 so the
// product stays in 32 bits (see RASTER_XF_S_MAX); 1000 thousandths is exactly 1024.
typedef struct { int32_t s_q10, ox8, oy8, tx8, ty8; } raster_xf_t;
#define RASTER_XF_S_MAX 32767   // largest wire scale (thousandths) a placed op may carry
_Static_assert((int64_t)(PX8_MAX + OUT_MAX_W * PX8_ONE) * ((RASTER_XF_S_MAX * 1024 + 500) / 1000) + 512 <= INT32_MAX,
               "placed transform must not overflow int32");
static inline int32_t raster_xf_x(const raster_xf_t *t, int32_t x8) { return t->tx8 + (((x8 - t->ox8) * t->s_q10 + 512) >> 10); }
static inline int32_t raster_xf_y(const raster_xf_t *t, int32_t y8) { return t->ty8 + (((y8 - t->oy8) * t->s_q10 + 512) >> 10); }
static inline int32_t raster_xf_len(const raster_xf_t *t, int32_t l8) { return (l8 * t->s_q10 + 512) >> 10; }
// raster_fill_poly / raster_stroke_poly with every vertex mapped through `xf` as it is read (the
// stream itself is untouched, so it can stay in flash). Mapped vertices are clamped to ±PX8_MAX.
void raster_fill_poly_xf(raster_t *r, const int16_t *verts, uint32_t count, uint8_t rule, const raster_xf_t *xf, uint8_t idx, uint8_t alpha);
void raster_stroke_poly_xf(raster_t *r, const int16_t *verts, uint32_t count, int32_t w8, bool closed, const raster_xf_t *xf, uint8_t idx, uint8_t alpha);
// Strokes each contour of the stream with width w8. Segments are drawn as quads; joins/caps are
// round discs when `round_joins` (used for icons), otherwise plain quads (map borders). Contours
// are closed when `closed`.
void raster_stroke_poly(raster_t *r, const int16_t *verts, uint32_t count, int32_t w8, bool closed, bool round_joins, uint8_t idx, uint8_t alpha);
// 1-bit bitmap scaled into a size8 × size8 square: modules × modules cells, rows byte-padded, MSB
// first. Set bits fill their cell; clear bits leave the band untouched. Cells tile without seams.
void raster_bitmap(raster_t *r, int32_t x8, int32_t y8, int32_t size8, uint16_t modules, const uint8_t *bits, uint8_t idx);
// 2 bpp coverage glyph (rows packed MSB-first, stride bytes) drawn with its top-left at (x_px,y_px).
void raster_blit_glyph2(raster_t *r, int32_t x_px, int32_t y_px, const uint8_t *bits, uint16_t w, uint16_t h, uint16_t stride, uint8_t idx, uint8_t alpha);
