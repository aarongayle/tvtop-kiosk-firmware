// Band rasteriser (see raster.h). Integer only: this runs on core 0 of an M0+ for every band of
// every frame, so the per-row and per-pixel loops use only adds, compares and shifts. Divisions
// and 64-bit products appear once per edge, per row or per call (setup), never per pixel.
//
// Sampling rule (kiosk_config.h): pixel (i, y) has its centre at (8i+4, 8y+4) px8 and is covered
// by a shape iff the centre is inside. For polygons the span rule is half-open, [xa, xb), so
// abutting shapes never double-cover or leave a seam.
#include "raster.h"
#include "geom.h"
#include <string.h>

// Crossings dropped because a scanline had more than the buffer holds. Not in raster_t (the
// header is shared); readable by tests and the stats console through this symbol.
uint32_t raster_stats_cross_dropped;

// ---- small integer helpers ----

// floor(v / 8) for any sign without relying on the (implementation-defined) sign of a shifted
// negative value.
static inline int32_t floor_div8(int32_t v) { return v >= 0 ? (v >> 3) : -((-v + 7) >> 3); }
// First pixel whose centre (8i+4) is >= a (px8): i = ceil((a-4)/8) = floor((a+3)/8).
// A half-open px8 interval [a, b) therefore covers pixels [px_first(a), px_first(b)).
static inline int32_t px_first(int32_t a) { return floor_div8(a + 3); }
// ceil(v / 65536) for a 16.16 value of either sign.
static inline int32_t ceil16(int64_t v) { return v >= 0 ? (int32_t)((v + 0xFFFF) >> 16) : -(int32_t)((-v) >> 16); }

// Integer square root (floor) of a 64-bit value, binary digit-by-digit. Used for corner insets,
// circle half-widths and segment lengths; a few dozen iterations per row or per segment.
static uint32_t isqrt64(uint64_t v) {
    uint64_t res = 0, bit = (uint64_t)1 << 62;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= res + bit) { v -= res + bit; res = (res >> 1) + bit; }
        else res >>= 1;
        bit >>= 2;
    }
    return (uint32_t)res;
}

static inline int32_t clamp32(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline int16_t to_i16(int32_t v) { return (int16_t)clamp32(v, -PX8_MAX, PX8_MAX); }

// ---- setup ----

void raster_init(raster_t *r, uint8_t *band, uint8_t *scratch, uint16_t width, uint16_t height, palette_t *pal) {
    memset(r, 0, sizeof *r);
    r->band = band;
    r->width = width;
    r->height = height;
    r->pal = pal;
    r->cross = (int16_t *)scratch;
    r->ncross = scratch + KIOSK_BAND_LINES * KIOSK_MAX_CROSSINGS * 2;
    r->cov = r->ncross + KIOSK_BAND_LINES;
    memset(r->cov, 0, OUT_MAX_W);   // the AA filler relies on cov being zero between calls
    raster_clear_clip(r);
}

void raster_begin_band(raster_t *r, int16_t y0, int16_t lines, uint8_t bg_idx) {
    if (lines > KIOSK_BAND_LINES) lines = KIOSK_BAND_LINES;
    if (lines < 0) lines = 0;
    r->y0 = y0;
    r->lines = lines;
    memset(r->band, bg_idx, (size_t)lines * r->width);
}

void raster_set_clip(raster_t *r, int16_t x0_px, int16_t y0_px, int16_t x1_px, int16_t y1_px) {
    int32_t x0 = clamp32(x0_px, 0, r->width), x1 = clamp32(x1_px, 0, r->width);
    int32_t y0 = clamp32(y0_px, 0, r->height), y1 = clamp32(y1_px, 0, r->height);
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    r->clip_x0 = (int16_t)x0; r->clip_x1 = (int16_t)x1;
    r->clip_y0 = (int16_t)y0; r->clip_y1 = (int16_t)y1;
}

void raster_clear_clip(raster_t *r) {
    r->clip_x0 = 0; r->clip_y0 = 0;
    r->clip_x1 = (int16_t)r->width; r->clip_y1 = (int16_t)r->height;
}

// Rows [ry0, ry1) of the screen that this band may touch: band ∩ clip.
static inline void band_rows(const raster_t *r, int32_t *ry0, int32_t *ry1) {
    int32_t a = r->y0, b = r->y0 + r->lines;
    if (a < r->clip_y0) a = r->clip_y0;
    if (b > r->clip_y1) b = r->clip_y1;
    *ry0 = a; *ry1 = b;
}

bool raster_band_intersects(const raster_t *r, int32_t x0_px, int32_t y0_px, int32_t x1_px, int32_t y1_px) {
    int32_t ry0, ry1;
    band_rows(r, &ry0, &ry1);
    return y1_px > ry0 && y0_px < ry1 && x1_px > r->clip_x0 && x0_px < r->clip_x1;
}

// ---- spans ----

// Blends `n` pixels at p with fg/alpha, remembering the last background→result pair: a span over
// a flat background then costs one palette_blend (a cache walk) instead of one per pixel.
static void blend_run(raster_t *r, uint8_t *p, int32_t n, uint8_t idx, uint8_t alpha) {
    int memo_bg = -1;
    uint8_t memo_res = 0;
    while (n-- > 0) {
        uint8_t bg = *p;
        if (bg != memo_bg) {
            memo_bg = bg;
            memo_res = palette_blend(r->pal, bg, idx, alpha);
        }
        *p++ = memo_res;
    }
}

void raster_span(raster_t *r, int16_t y_px, int32_t x0_px, int32_t x1_px, uint8_t idx, uint8_t alpha) {
    if (alpha == 0) return;
    if (y_px < r->y0 || y_px >= r->y0 + r->lines || y_px < r->clip_y0 || y_px >= r->clip_y1) return;
    if (x0_px < r->clip_x0) x0_px = r->clip_x0;
    if (x1_px > r->clip_x1) x1_px = r->clip_x1;
    if (x1_px <= x0_px) return;
    uint8_t *p = r->band + (size_t)(y_px - r->y0) * r->width + x0_px;
    r->stats_spans++;
    if (alpha == 255) memset(p, idx, (size_t)(x1_px - x0_px));
    else blend_run(r, p, x1_px - x0_px, idx, alpha);
}

// Span given as a half-open px8 interval: pixels whose centre lies in [a8, b8).
static inline void span8(raster_t *r, int32_t y, int32_t a8, int32_t b8, uint8_t idx, uint8_t alpha) {
    raster_span(r, (int16_t)y, px_first(a8), px_first(b8), idx, alpha);
}

// ---- rects and circles ----

void raster_fill_rect(raster_t *r, int32_t x8, int32_t y8, int32_t w8, int32_t h8, int32_t radius8, uint8_t idx, uint8_t alpha) {
    if (w8 <= 0 || h8 <= 0 || alpha == 0) return;
    int32_t ry0, ry1;
    band_rows(r, &ry0, &ry1);
    int32_t ya = px_first(y8), yb = px_first(y8 + h8);
    if (ya < ry0) ya = ry0;
    if (yb > ry1) yb = ry1;
    if (yb <= ya) return;
    int32_t rad = radius8;
    int32_t half = (w8 < h8 ? w8 : h8) / 2;
    if (rad > half) rad = half;
    if (rad <= 0) {
        for (int32_t y = ya; y < yb; y++) span8(r, y, x8, x8 + w8, idx, alpha);
        return;
    }
    // Rounded: rows inside the corner zones are inset by rad - sqrt(rad² - dy²), where dy is the
    // row centre's distance from the corner circle's centre line. An integer sqrt is exact for the
    // centre test because |xc - cx| is an integer in px8.
    int32_t top_c = y8 + rad, bot_c = y8 + h8 - rad;
    uint32_t rad2 = (uint32_t)rad * (uint32_t)rad;
    for (int32_t y = ya; y < yb; y++) {
        int32_t yc = y * PX8_ONE + PX8_HALF;
        int32_t dy = yc < top_c ? top_c - yc : (yc > bot_c ? yc - bot_c : 0);
        int32_t inset = 0;
        if (dy) {
            uint32_t d2 = (uint32_t)dy * (uint32_t)dy;
            inset = d2 >= rad2 ? rad : rad - (int32_t)isqrt64(rad2 - d2);
        }
        // Left edge: centre xc covered iff xc - cx >= -s, i.e. xc >= x8 + inset. Right edge:
        // xc - cx' <= s, i.e. xc < x8 + w8 - inset + 1 (the +1 turns the closed bound half-open).
        int32_t a = x8 + inset, b = x8 + w8 - inset;
        if (dy) b += 1;
        if (b > a) span8(r, y, a, b, idx, alpha);
    }
}

void raster_fill_circle(raster_t *r, int32_t cx8, int32_t cy8, int32_t r8, uint8_t idx, uint8_t alpha) {
    if (r8 <= 0 || alpha == 0) return;
    int32_t ry0, ry1;
    band_rows(r, &ry0, &ry1);
    int32_t ya = px_first(cy8 - r8), yb = px_first(cy8 + r8 + 1);
    if (ya < ry0) ya = ry0;
    if (yb > ry1) yb = ry1;
    uint64_t r2 = (uint64_t)r8 * (uint64_t)r8;
    for (int32_t y = ya; y < yb; y++) {
        int64_t dy = (int64_t)(y * PX8_ONE + PX8_HALF) - cy8;
        uint64_t d2 = (uint64_t)(dy * dy);
        if (d2 > r2) continue;
        int32_t hw = (int32_t)isqrt64(r2 - d2);   // |xc - cx| <= hw is inside
        span8(r, y, cx8 - hw, cx8 + hw + 1, idx, alpha);
    }
}

void raster_stroke_circle(raster_t *r, int32_t cx8, int32_t cy8, int32_t r8, int32_t w8, uint8_t idx, uint8_t alpha) {
    if (w8 <= 0 || r8 < 0 || alpha == 0) return;
    int32_t ro = r8 + w8 / 2, ri = r8 - w8 / 2;
    if (ro <= 0) return;
    if (ri <= 0) { raster_fill_circle(r, cx8, cy8, ro, idx, alpha); return; }
    int32_t ry0, ry1;
    band_rows(r, &ry0, &ry1);
    int32_t ya = px_first(cy8 - ro), yb = px_first(cy8 + ro + 1);
    if (ya < ry0) ya = ry0;
    if (yb > ry1) yb = ry1;
    uint64_t ro2 = (uint64_t)ro * (uint64_t)ro, ri2 = (uint64_t)ri * (uint64_t)ri;
    for (int32_t y = ya; y < yb; y++) {
        int64_t dy = (int64_t)(y * PX8_ONE + PX8_HALF) - cy8;
        uint64_t d2 = (uint64_t)(dy * dy);
        if (d2 > ro2) continue;
        int32_t ho = (int32_t)isqrt64(ro2 - d2);
        if (d2 > ri2) {
            span8(r, y, cx8 - ho, cx8 + ho + 1, idx, alpha);
        } else {
            // Inside the inner circle means |xc - cx| <= hi; the ring is the two remainders.
            int32_t hi = (int32_t)isqrt64(ri2 - d2);
            span8(r, y, cx8 - ho, cx8 - hi, idx, alpha);
            span8(r, y, cx8 + hi + 1, cx8 + ho + 1, idx, alpha);
        }
    }
}

// ---- polygons ----
//
// Crossings are stored per sub-row as int16 (x_px8 * 2 + dir) with x clamped to just outside the
// clip so the value fits, dir = 1 for a downward edge and 0 for upward. Sorting the packed value
// sorts by x. With ss sub-rows per pixel row the buffer is split into lines*ss lists of
// KIOSK_MAX_CROSSINGS/ss entries each.

typedef struct {
    int16_t *lists;
    uint8_t *n;            // crossings per list
    int32_t cap;           // entries per list
    int32_t ss;            // sub-rows per pixel row
    int32_t sub0, sub1;    // global sub-row index range covered by the lists: [sub0, sub1)
    int32_t xlo, xhi;      // crossing x clamp, px8
} walk_t;

// Adds the crossings of one edge (px8 endpoints) with every sub-row centre it spans, using the
// top-inclusive / bottom-exclusive rule so a vertex shared by two edges is counted once.
static void walk_edge(raster_t *r, const walk_t *w, int32_t xa, int32_t ya, int32_t xb, int32_t yb) {
    r->stats_edge_visits++;
    if (ya == yb) return;   // horizontal: contributes no crossing
    int dir = 1;
    if (ya > yb) { int32_t t = xa; xa = xb; xb = t; t = ya; ya = yb; yb = t; dir = 0; }
    // Sub-row k (global, k = y*ss + s) has its centre at 8k+4 in units of px8/ss.
    int32_t ss = w->ss;
    int32_t ytop = ya * ss, ybot = yb * ss;
    int32_t kg = floor_div8(ytop + 3);          // first sub-row with centre >= ytop
    int32_t ke = floor_div8(ybot + 3);          // first sub-row with centre >= ybot (exclusive)
    int32_t k0 = kg > w->sub0 ? kg : w->sub0;
    int32_t k1 = ke < w->sub1 ? ke : w->sub1;
    if (k0 >= k1) return;
    // x along the edge in 16.16 px8. Both the start value (at the edge's own first sub-row) and
    // the per-sub-row step are functions of the edge alone, and fixed-point addition is exact, so
    // the crossing of a given sub-row is identical whichever band computes it — a glyph split
    // across bands renders the same as one drawn in a single band.
    int64_t dx16 = (int64_t)(xb - xa) << 16;
    int32_t dy = ybot - ytop;
    int64_t step = (dx16 * 8) / dy;
    int64_t x = ((int64_t)xa << 16) + ((int64_t)(kg * 8 + 4 - ytop) * dx16) / dy + (int64_t)(k0 - kg) * step;
    int16_t *lists = w->lists;
    uint8_t *n = w->n;
    int32_t cap = w->cap;
    for (int32_t k = k0; k < k1; k++, x += step) {
        int32_t xc = clamp32(ceil16(x), w->xlo, w->xhi);
        int32_t slot = k - w->sub0;
        if (n[slot] < cap) lists[slot * cap + n[slot]++] = (int16_t)(xc * 2 + dir);
        else raster_stats_cross_dropped++;
    }
}

// Walks every contour of a vertex stream. Contours are implicitly closed; a stream may or may
// not begin with a GEOM_BREAK marker.
static void walk_stream(raster_t *r, const walk_t *w, const int16_t *verts, uint32_t count) {
    int32_t sx = 0, sy = 0, px = 0, py = 0;
    uint32_t nv = 0;   // vertices in the current contour
    for (uint32_t i = 0; i < count; i++) {
        int32_t x = verts[i * 2], y = verts[i * 2 + 1];
        if (x == GEOM_BREAK) {
            if (nv >= 2) walk_edge(r, w, px, py, sx, sy);
            nv = 0;
            continue;
        }
        if (nv == 0) { sx = x; sy = y; }
        else walk_edge(r, w, px, py, x, y);
        px = x; py = y; nv++;
    }
    if (nv >= 2) walk_edge(r, w, px, py, sx, sy);
}

// walk_stream with each vertex mapped through a placement as it is read. A separate loop so the
// stream walk of unplaced geometry stays exactly as it was.
static void walk_stream_xf(raster_t *r, const walk_t *w, const int16_t *verts, uint32_t count, const raster_xf_t *xf) {
    int32_t sx = 0, sy = 0, px = 0, py = 0;
    uint32_t nv = 0;
    for (uint32_t i = 0; i < count; i++) {
        int32_t x = verts[i * 2];
        if (x == GEOM_BREAK) {
            if (nv >= 2) walk_edge(r, w, px, py, sx, sy);
            nv = 0;
            continue;
        }
        x = clamp32(raster_xf_x(xf, x), -PX8_MAX, PX8_MAX);
        int32_t y = clamp32(raster_xf_y(xf, verts[i * 2 + 1]), -PX8_MAX, PX8_MAX);
        if (nv == 0) { sx = x; sy = y; }
        else walk_edge(r, w, px, py, x, y);
        px = x; py = y; nv++;
    }
    if (nv >= 2) walk_edge(r, w, px, py, sx, sy);
}

static void sort_crossings(int16_t *a, int32_t n) {
    for (int32_t i = 1; i < n; i++) {
        int16_t v = a[i];
        int32_t j = i;
        while (j > 0 && a[j - 1] > v) { a[j] = a[j - 1]; j--; }
        a[j] = v;
    }
}

// Sets up the crossing lists for the band rows ∩ clip. Returns false if there is nothing to draw.
static bool walk_begin(raster_t *r, walk_t *w, uint8_t ss, uint8_t *counts, int32_t *ry0, int32_t *ry1) {
    band_rows(r, ry0, ry1);
    if (*ry1 <= *ry0 || r->clip_x1 <= r->clip_x0) return false;
    w->lists = r->cross;
    w->n = counts;
    w->ss = ss;
    w->cap = KIOSK_MAX_CROSSINGS / ss;
    w->sub0 = *ry0 * ss;
    w->sub1 = *ry1 * ss;
    // Anything left of the clip behaves like a crossing at the clip's edge (it only changes the
    // winding before the first visible pixel) and the clamp keeps x*2 inside int16.
    w->xlo = r->clip_x0 * PX8_ONE - PX8_ONE;
    w->xhi = r->clip_x1 * PX8_ONE + PX8_ONE;
    memset(counts, 0, (size_t)(w->sub1 - w->sub0));
    return true;
}

static inline int32_t cross_x(int16_t c) { int32_t d = c & 1; return (c - d) / 2; }

static void fill_poly(raster_t *r, const int16_t *verts, uint32_t count, uint8_t rule, const raster_xf_t *xf, uint8_t idx, uint8_t alpha) {
    if (alpha == 0 || count < 3) return;
    walk_t w;
    int32_t ry0, ry1;
    if (!walk_begin(r, &w, 1, r->ncross, &ry0, &ry1)) return;
    if (xf) walk_stream_xf(r, &w, verts, count, xf);
    else walk_stream(r, &w, verts, count);
    for (int32_t y = ry0; y < ry1; y++) {
        int32_t slot = y - ry0, n = w.n[slot];
        if (n < 2) continue;
        int16_t *list = w.lists + slot * w.cap;
        sort_crossings(list, n);
        int32_t wind = 0, xs = 0;
        bool inside = false;
        for (int32_t i = 0; i < n; i++) {
            int32_t x = cross_x(list[i]);
            wind += (list[i] & 1) ? 1 : -1;
            bool now = rule == FILL_EVENODD ? (wind & 1) != 0 : wind != 0;
            if (now && !inside) xs = x;
            else if (!now && inside) span8(r, y, xs, x, idx, alpha);
            inside = now;
        }
    }
}

void raster_fill_poly(raster_t *r, const int16_t *verts, uint32_t count, uint8_t rule, uint8_t idx, uint8_t alpha) {
    fill_poly(r, verts, count, rule, NULL, idx, alpha);
}

void raster_fill_poly_xf(raster_t *r, const int16_t *verts, uint32_t count, uint8_t rule, const raster_xf_t *xf, uint8_t idx, uint8_t alpha) {
    fill_poly(r, verts, count, rule, xf, idx, alpha);
}

// Coverage 0..255 → the four levels the blend cache is sized for.
static inline uint8_t quant4(uint8_t c) { return c < 43 ? 0 : (c < 128 ? 85 : (c < 213 ? 170 : 255)); }

void raster_fill_poly_aa(raster_t *r, const int16_t *verts, uint32_t count, uint8_t rule, uint8_t ss, uint8_t idx, uint8_t alpha) {
    if (alpha == 0 || count < 3) return;
    if (ss < 1) ss = 1;
    if (ss > 4) ss = 4;
    uint8_t counts[KIOSK_BAND_LINES * 4];
    walk_t w;
    int32_t ry0, ry1;
    if (!walk_begin(r, &w, ss, counts, &ry0, &ry1)) return;
    walk_stream(r, &w, verts, count);

    // Coverage contributed by an overlap of ov eighths of a pixel on one sub-row; cw[8]*ss ≈ 255.
    uint8_t cw[9];
    for (int ov = 0; ov <= 8; ov++) cw[ov] = (uint8_t)((ov * 255 + 4 * ss) / (8 * ss));
    // The three non-zero levels scaled by alpha, and a one-entry memo per level.
    uint8_t lv[4];
    for (int i = 1; i < 4; i++) lv[i] = alpha == 255 ? (uint8_t)(i * 85) : (uint8_t)((i * 85 * alpha + 127) / 255);
    lv[0] = 0;
    int memo_bg[4] = { -1, -1, -1, -1 };
    uint8_t memo_res[4] = { 0, 0, 0, 0 };

    int32_t cx0 = r->clip_x0, cx1 = r->clip_x1;
    uint8_t *cov = r->cov;
    for (int32_t y = ry0; y < ry1; y++) {
        int32_t ext_lo = cx1, ext_hi = cx0;   // touched pixel range [ext_lo, ext_hi)
        for (int32_t s = 0; s < ss; s++) {
            int32_t slot = (y - ry0) * ss + s, n = w.n[slot];
            if (n < 2) continue;
            int16_t *list = w.lists + slot * w.cap;
            sort_crossings(list, n);
            int32_t wind = 0, xs = 0;
            bool inside = false;
            for (int32_t i = 0; i < n; i++) {
                int32_t x = cross_x(list[i]);
                wind += (list[i] & 1) ? 1 : -1;
                bool now = rule == FILL_EVENODD ? (wind & 1) != 0 : wind != 0;
                if (now && !inside) { xs = x; inside = true; continue; }
                if (now || !inside) { inside = now; continue; }
                inside = false;
                // Accumulate [xs, x) px8 into cov over pixels [cx0, cx1).
                int32_t a = xs, b = x;
                if (a < cx0 * PX8_ONE) a = cx0 * PX8_ONE;
                if (b > cx1 * PX8_ONE) b = cx1 * PX8_ONE;
                if (b <= a) continue;
                int32_t ia = floor_div8(a), ib = floor_div8(b - 1);   // first and last pixel touched
                if (ia < ext_lo) ext_lo = ia;
                if (ib + 1 > ext_hi) ext_hi = ib + 1;
                if (ia == ib) {
                    unsigned c = cov[ia] + cw[b - a];
                    cov[ia] = c > 255 ? 255 : (uint8_t)c;
                    continue;
                }
                unsigned c = cov[ia] + cw[(ia + 1) * PX8_ONE - a];
                cov[ia] = c > 255 ? 255 : (uint8_t)c;
                uint8_t full = cw[8];
                for (int32_t i2 = ia + 1; i2 < ib; i2++) {
                    c = cov[i2] + full;
                    cov[i2] = c > 255 ? 255 : (uint8_t)c;
                }
                c = cov[ib] + cw[b - ib * PX8_ONE];
                cov[ib] = c > 255 ? 255 : (uint8_t)c;
            }
        }
        if (ext_hi <= ext_lo) continue;
        uint8_t *row = r->band + (size_t)(y - r->y0) * r->width;
        r->stats_spans++;
        for (int32_t i = ext_lo; i < ext_hi; i++) {
            uint8_t c = cov[i];
            if (!c) continue;
            int q = quant4(c) / 85;
            if (!q) continue;
            uint8_t level = lv[q];
            if (level == 255) { row[i] = idx; continue; }
            uint8_t bg = row[i];
            if (bg != memo_bg[q]) { memo_bg[q] = bg; memo_res[q] = palette_blend(r->pal, bg, idx, level); }
            row[i] = memo_res[q];
        }
        memset(cov + ext_lo, 0, (size_t)(ext_hi - ext_lo));
    }
}

// ---- strokes ----

// Fills the rectangle of half-width hw8 around segment a→b (butt ends). Zero length draws nothing.
static void fill_segment(raster_t *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t hw8, uint8_t idx, uint8_t alpha) {
    int64_t dx = (int64_t)x1 - x0, dy = (int64_t)y1 - y0;
    uint64_t len2 = (uint64_t)(dx * dx + dy * dy);
    if (len2 == 0 || hw8 <= 0) return;
    int64_t len = isqrt64(len2);
    if (len == 0) len = 1;
    // Normal of length hw8, rounded to nearest px8 (round half away from zero).
    int64_t nxn = -dy * hw8, nyn = dx * hw8;
    int32_t nx = (int32_t)((nxn >= 0 ? nxn + len / 2 : nxn - len / 2) / len);
    int32_t ny = (int32_t)((nyn >= 0 ? nyn + len / 2 : nyn - len / 2) / len);
    int16_t q[8] = {
        to_i16(x0 + nx), to_i16(y0 + ny), to_i16(x1 + nx), to_i16(y1 + ny),
        to_i16(x1 - nx), to_i16(y1 - ny), to_i16(x0 - nx), to_i16(y0 - ny),
    };
    raster_fill_poly(r, q, 4, FILL_NONZERO, idx, alpha);
}

void raster_line(raster_t *r, int32_t x0_8, int32_t y0_8, int32_t x1_8, int32_t y1_8, int32_t w8, uint8_t idx, uint8_t alpha) {
    if (alpha == 0) return;
    if (w8 < PX8_ONE) w8 = PX8_ONE;   // hairlines must not vanish between pixel centres
    fill_segment(r, x0_8, y0_8, x1_8, y1_8, w8 / 2, idx, alpha);
}

static void stroke_poly(raster_t *r, const int16_t *verts, uint32_t count, int32_t w8, bool closed, bool round_joins, const raster_xf_t *xf, uint8_t idx, uint8_t alpha) {
    if (alpha == 0 || w8 <= 0) return;
    if (w8 < PX8_ONE) w8 = PX8_ONE;
    int32_t hw = w8 / 2;
    int32_t sx = 0, sy = 0, px = 0, py = 0;
    uint32_t nv = 0;
    for (uint32_t i = 0; i <= count; i++) {
        bool end = i == count;
        int32_t x = 0, y = 0;
        if (!end) { x = verts[i * 2]; y = verts[i * 2 + 1]; }
        // Per-vertex mapping costs a predictable branch next to a segment fill (a square root and
        // a quad) per vertex, so this loop is shared rather than duplicated like walk_stream.
        if (xf && !end && x != GEOM_BREAK) { x = clamp32(raster_xf_x(xf, x), -PX8_MAX, PX8_MAX); y = clamp32(raster_xf_y(xf, y), -PX8_MAX, PX8_MAX); }
        if (end || x == GEOM_BREAK) {
            if (nv >= 2 && closed) {
                fill_segment(r, px, py, sx, sy, hw, idx, alpha);
                if (!round_joins && hw >= 2 * PX8_ONE) {   // the two joints the loop could not see: last vertex and start
                    raster_fill_rect(r, px - hw, py - hw, 2 * hw, 2 * hw, 0, idx, alpha);
                    raster_fill_rect(r, sx - hw, sy - hw, 2 * hw, 2 * hw, 0, idx, alpha);
                }
            }
            nv = 0;
            continue;
        }
        if (nv == 0) { sx = x; sy = y; }
        else {
            fill_segment(r, px, py, x, y, hw, idx, alpha);
            // Plain joins fill an axis-aligned square of the stroke width at each interior vertex:
            // exactly the mitre for right angles (board outlines) and within half a width of it
            // otherwise, so butt-ended segments never leave a notch at a corner. Only for strokes
            // of 4 px and up: thinner ones show no notch, and the extra spans would double the
            // line-pool cost of a map border. Open ends stay butt (SVG default).
            if (!round_joins && nv >= 2 && hw >= 2 * PX8_ONE) raster_fill_rect(r, px - hw, py - hw, 2 * hw, 2 * hw, 0, idx, alpha);
        }
        // Round joins: a disc at every vertex covers the joins and, for open contours, the end
        // caps; a lone vertex (icon dots such as "M12 20h.01") becomes a dot.
        if (round_joins) raster_fill_circle(r, x, y, hw, idx, alpha);
        px = x; py = y; nv++;
    }
}

void raster_stroke_poly(raster_t *r, const int16_t *verts, uint32_t count, int32_t w8, bool closed, bool round_joins, uint8_t idx, uint8_t alpha) {
    stroke_poly(r, verts, count, w8, closed, round_joins, NULL, idx, alpha);
}

void raster_stroke_poly_xf(raster_t *r, const int16_t *verts, uint32_t count, int32_t w8, bool closed, const raster_xf_t *xf, uint8_t idx, uint8_t alpha) {
    stroke_poly(r, verts, count, w8, closed, false, xf, idx, alpha);
}

// ---- bitmaps and glyphs ----

#define RASTER_MAX_MODULES 160

void raster_bitmap(raster_t *r, int32_t x8, int32_t y8, int32_t size8, uint16_t modules, const uint8_t *bits, uint8_t idx) {
    if (!bits || modules == 0 || modules > RASTER_MAX_MODULES || size8 <= 0) return;
    int32_t ry0, ry1;
    band_rows(r, &ry0, &ry1);
    int32_t ya = px_first(y8), yb = px_first(y8 + size8);
    if (ya < ry0) ya = ry0;
    if (yb > ry1) yb = ry1;
    if (yb <= ya) return;
    // Cell edges e_j = x8 + floor(j*size8/modules), rounded to pixels by the centre rule; cell j
    // is [e_j, e_{j+1}) so cells tile with no gaps and no overlap.
    int16_t col[RASTER_MAX_MODULES + 1];
    for (int32_t j = 0; j <= modules; j++) col[j] = (int16_t)px_first(x8 + (j * size8) / modules);
    uint32_t stride = ((uint32_t)modules + 7) / 8;
    int32_t m = 0;
    for (int32_t y = ya; y < yb; y++) {
        // Module row: advance m while the next row edge is at or above this pixel row. Rows are
        // visited in order so this is amortised O(1) per pixel row.
        while (m + 1 < modules && px_first(y8 + ((m + 1) * size8) / modules) <= y) m++;
        const uint8_t *row = bits + (size_t)m * stride;
        int32_t j = 0;
        while (j < modules) {
            if (!(row[j >> 3] & (0x80u >> (j & 7)))) { j++; continue; }
            int32_t j0 = j;
            while (j < modules && (row[j >> 3] & (0x80u >> (j & 7)))) j++;
            raster_span(r, (int16_t)y, col[j0], col[j], idx, 255);
        }
    }
}

void raster_blit_glyph2(raster_t *r, int32_t x_px, int32_t y_px, const uint8_t *bits, uint16_t w, uint16_t h, uint16_t stride, uint8_t idx, uint8_t alpha) {
    if (!bits || alpha == 0 || w == 0 || h == 0) return;
    int32_t ry0, ry1;
    band_rows(r, &ry0, &ry1);
    int32_t ya = y_px > ry0 ? y_px : ry0, yb = y_px + h < ry1 ? y_px + h : ry1;
    int32_t xa = x_px > r->clip_x0 ? x_px : r->clip_x0, xb = x_px + w < r->clip_x1 ? x_px + w : r->clip_x1;
    if (yb <= ya || xb <= xa) return;
    uint8_t lv[4];
    lv[0] = 0;
    for (int i = 1; i < 4; i++) lv[i] = alpha == 255 ? (uint8_t)(i * 85) : (uint8_t)((i * 85 * alpha + 127) / 255);
    int memo_bg[4] = { -1, -1, -1, -1 };
    uint8_t memo_res[4] = { 0, 0, 0, 0 };
    for (int32_t y = ya; y < yb; y++) {
        const uint8_t *src = bits + (size_t)(y - y_px) * stride;
        uint8_t *row = r->band + (size_t)(y - r->y0) * r->width;
        r->stats_spans++;
        for (int32_t x = xa; x < xb; x++) {
            int32_t j = x - x_px;
            int q = (src[j >> 2] >> (6 - 2 * (j & 3))) & 3;
            if (!q) continue;
            uint8_t level = lv[q];
            if (level == 255) { row[x] = idx; continue; }
            uint8_t bg = row[x];
            if (bg != memo_bg[q]) { memo_bg[q] = bg; memo_res[q] = palette_blend(r->pal, bg, idx, level); }
            row[x] = memo_res[q];
        }
    }
}
