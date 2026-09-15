// Text over the generated font blob (see font.h for the layout). All blob reads go through
// byte-wise little-endian helpers: the blob is a uint8_t array in flash with no alignment promise
// and the M0+ faults on unaligned halfword/word loads.
#include "font.h"

#include <string.h>
#include "geom.h"

#define FONT_MAX_FACES 32
#define HDR_BYTES 16
#define FACE_BYTES 16
#define GLYPH_BYTES 12
#define OFACE_BYTES 24
#define OGLYPH_BYTES 28
#define SPACE_CP 0x20
#define OUTLINE_SS_SMALL 3     // sub-rows for outline text below 40 px
#define OUTLINE_SS_LARGE 2

typedef struct {
    uint8_t size, bold;
    uint16_t nglyphs;
    int16_t ascent8, descent8;
    uint32_t glyphs_off, bits_off;
} face_t;

typedef struct {
    uint16_t upem;
    int16_t ascent, descent;
    uint16_t nglyphs;
    uint32_t glyphs_off, points_off, flags_off, ends_off;
} oface_t;

static const uint8_t *blob;
static uint32_t blob_size;
static face_t faces[FONT_MAX_FACES];
static int nfaces;
static oface_t ofaces[2];
static bool ready;
// Flattened outline vertices of one glyph (px8 pairs, GEOM_BREAK markers between contours).
static int16_t verts[KIOSK_GLYPH_MAX_VERTS * 2];

static inline uint32_t rd8(uint32_t off) { return blob[off]; }
static inline uint32_t rd16(uint32_t off) { return (uint32_t)blob[off] | ((uint32_t)blob[off + 1] << 8); }
static inline uint32_t rd32(uint32_t off) { return rd16(off) | (rd16(off + 2) << 16); }
static inline int32_t rds16(uint32_t off) { return (int16_t)rd16(off); }
static inline int32_t rds8(uint32_t off) { return (int8_t)blob[off]; }

// Round px8 to whole pixels (floor(v/8 + 1/2)); a shift, not a divide, and correct for negatives.
static inline int32_t px8_round(int32_t v) { return (v + PX8_HALF) >> PX8_SHIFT; }

// ---- validation ----

static bool in_blob(uint32_t off, uint32_t len) { return off <= blob_size && len <= blob_size - off; }

static bool validate_face(face_t *f, uint32_t rec) {
    f->size = (uint8_t)rd8(rec);
    f->bold = (uint8_t)rd8(rec + 1);
    f->nglyphs = (uint16_t)rd16(rec + 2);
    f->ascent8 = (int16_t)rd16(rec + 4);
    f->descent8 = (int16_t)rd16(rec + 6);
    f->glyphs_off = rd32(rec + 8);
    f->bits_off = rd32(rec + 12);
    if (f->size == 0 || f->bold > 1 || f->nglyphs == 0) return false;
    if (!in_blob(f->glyphs_off, (uint32_t)f->nglyphs * GLYPH_BYTES) || f->bits_off > blob_size) return false;
    uint32_t prev_cp = 0;
    for (uint32_t i = 0; i < f->nglyphs; i++) {
        uint32_t g = f->glyphs_off + i * GLYPH_BYTES;
        uint32_t cp = rd16(g), w = rd8(g + 2), h = rd8(g + 3), off = rd32(g + 8);
        if (i && cp <= prev_cp) return false;          // must be sorted for the binary search
        prev_cp = cp;
        uint32_t stride = (w * 2 + 7) / 8;
        if (!in_blob(f->bits_off, off) || !in_blob(f->bits_off + off, stride * h)) return false;
    }
    return true;
}

static bool validate_oface(oface_t *o, uint32_t rec) {
    o->upem = (uint16_t)rd16(rec);
    o->ascent = (int16_t)rd16(rec + 2);
    o->descent = (int16_t)rd16(rec + 4);
    o->nglyphs = (uint16_t)rd16(rec + 6);
    o->glyphs_off = rd32(rec + 8);
    o->points_off = rd32(rec + 12);
    o->flags_off = rd32(rec + 16);
    o->ends_off = rd32(rec + 20);
    if (o->upem == 0 || o->nglyphs == 0) return false;
    if (!in_blob(o->glyphs_off, (uint32_t)o->nglyphs * OGLYPH_BYTES)) return false;
    uint32_t prev_cp = 0;
    for (uint32_t i = 0; i < o->nglyphs; i++) {
        uint32_t g = o->glyphs_off + i * OGLYPH_BYTES;
        uint32_t cp = rd16(g), npts = rd16(g + 12), ncont = rd16(g + 14);
        uint32_t po = rd32(g + 16), fo = rd32(g + 20), eo = rd32(g + 24);
        if (i && cp <= prev_cp) return false;
        prev_cp = cp;
        if (!in_blob(o->points_off, po) || !in_blob(o->points_off + po, npts * 4)) return false;
        if (!in_blob(o->flags_off, fo) || !in_blob(o->flags_off + fo, npts)) return false;
        if (!in_blob(o->ends_off, eo) || !in_blob(o->ends_off + eo, ncont * 2)) return false;
        // Contour ends must be strictly increasing and inside the point array, so the flattener
        // can walk them without re-checking.
        uint32_t prev_end = 0;
        for (uint32_t c = 0; c < ncont; c++) {
            uint32_t e = rd16(o->ends_off + eo + c * 2);
            if (e >= npts || (c && e <= prev_end)) return false;
            prev_end = e;
        }
        if (ncont == 0 && npts != 0) return false;
    }
    return true;
}

bool font_init(const uint8_t *b, uint32_t size) {
    ready = false;
    blob = b;
    blob_size = size;
    if (!b || size < HDR_BYTES) return false;
    if (memcmp(b, "TVFN", 4) != 0 || rd16(4) != 1) return false;
    nfaces = (int)rd16(6);
    uint32_t outlines_off = rd32(8), faces_off = rd32(12);
    if (nfaces < 1 || nfaces > FONT_MAX_FACES) return false;
    if (!in_blob(faces_off, (uint32_t)nfaces * FACE_BYTES) || !in_blob(outlines_off, 2 * OFACE_BYTES)) return false;
    for (int i = 0; i < nfaces; i++)
        if (!validate_face(&faces[i], faces_off + (uint32_t)i * FACE_BYTES)) return false;
    for (int w = 0; w < 2; w++)
        if (!validate_oface(&ofaces[w], outlines_off + (uint32_t)w * OFACE_BYTES)) return false;
    ready = true;
    return true;
}

// ---- lookup ----

// Nearest face of the wanted weight (ties up); falls back to the other weight if none of the
// wanted weight exists. -1 when the blob is not loaded.
static int face_for(int size_px, bool bold) {
    int best = -1, best_d = 0;
    for (int pass = 0; pass < 2 && best < 0; pass++) {
        for (int i = 0; i < nfaces; i++) {
            if ((faces[i].bold != 0) != (bold != 0) && pass == 0) continue;
            int d = faces[i].size - size_px;
            int ad = d < 0 ? -d : d;
            if (best < 0 || ad < best_d || (ad == best_d && faces[i].size > faces[best].size)) { best = i; best_d = ad; }
        }
    }
    return best;
}

int font_bitmap_size_for(int size_px) {
    if (!ready || size_px > KIOSK_FONT_BITMAP_MAX) return -1;
    int f = face_for(size_px, false);
    return f < 0 ? -1 : faces[f].size;
}

// Binary search a glyph table of `stride`-byte records starting with u16 cp. Returns the record
// offset or 0 when absent.
static uint32_t find_glyph(uint32_t table, uint32_t n, uint32_t stride, uint32_t cp) {
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) >> 1;
        uint32_t c = rd16(table + mid * stride);
        if (c == cp) return table + mid * stride;
        if (c < cp) lo = mid + 1; else hi = mid;
    }
    return 0;
}

static uint32_t bitmap_glyph(const face_t *f, uint32_t cp) {
    if (cp > 0xFFFF) return 0;
    return find_glyph(f->glyphs_off, f->nglyphs, GLYPH_BYTES, cp);
}

static uint32_t outline_glyph(const oface_t *o, uint32_t cp) {
    if (cp > 0xFFFF) return 0;
    return find_glyph(o->glyphs_off, o->nglyphs, OGLYPH_BYTES, cp);
}

// Advance in px8 of cp (or of the space when missing).
static int32_t bitmap_adv8(const face_t *f, uint32_t cp) {
    uint32_t g = bitmap_glyph(f, cp);
    if (!g) g = bitmap_glyph(f, SPACE_CP);
    return g ? (int32_t)rd16(g + 6) : 0;
}

// 16.16 px8 per font unit for size_px; one divide per string.
static int32_t outline_scale(const oface_t *o, int size_px) {
    return (int32_t)((((int64_t)size_px * PX8_ONE) << 16) / o->upem);
}
static inline int32_t fu_to_px8(int32_t fu, int32_t scale) { return (int32_t)(((int64_t)fu * scale + 0x8000) >> 16); }

static int32_t outline_adv8(const oface_t *o, uint32_t cp, int32_t scale) {
    uint32_t g = outline_glyph(o, cp);
    if (!g) g = outline_glyph(o, SPACE_CP);
    return g ? fu_to_px8((int32_t)rd16(g + 2), scale) : 0;
}

// ---- UTF-8 ----

uint32_t utf8_next(const char **s, const char *end) {
    const uint8_t *p = (const uint8_t *)*s;
    const uint8_t *e = (const uint8_t *)end;
    if (p >= e) return 0;
    uint32_t c = *p++;
    if (c < 0x80) { *s = (const char *)p; return c; }
    uint32_t cp, min;
    int n;
    if ((c & 0xE0) == 0xC0) { n = 1; cp = c & 0x1F; min = 0x80; }
    else if ((c & 0xF0) == 0xE0) { n = 2; cp = c & 0x0F; min = 0x800; }
    else if ((c & 0xF8) == 0xF0) { n = 3; cp = c & 0x07; min = 0x10000; }
    else { *s = (const char *)p; return 0xFFFD; }   // stray continuation or 0xF8..0xFF lead
    for (int i = 0; i < n; i++) {
        // A truncated or broken sequence yields one U+FFFD for the bytes seen so far and resumes
        // at the offending byte, which may itself start a valid sequence.
        if (p >= e || (*p & 0xC0) != 0x80) { *s = (const char *)p; return 0xFFFD; }
        cp = (cp << 6) | (*p & 0x3F);
        p++;
    }
    *s = (const char *)p;
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0xFFFD;
    return cp;
}

// ---- metrics ----

int32_t font_measure(const char *utf8, size_t len, int size_px, bool bold) {
    if (!ready || size_px <= 0 || !utf8) return 0;
    const char *s = utf8, *end = utf8 + len;
    int32_t w = 0;
    if (size_px <= KIOSK_FONT_BITMAP_MAX) {
        const face_t *f = &faces[face_for(size_px, bold)];
        while (s < end) w += bitmap_adv8(f, utf8_next(&s, end));
    } else {
        const oface_t *o = &ofaces[bold ? 1 : 0];
        int32_t scale = outline_scale(o, size_px);
        while (s < end) w += outline_adv8(o, utf8_next(&s, end), scale);
    }
    return w;
}

void font_extent(int size_px, bool bold, int32_t *ascent8, int32_t *descent8) {
    int32_t a = 0, d = 0;
    if (ready && size_px > 0) {
        if (size_px <= KIOSK_FONT_BITMAP_MAX) {
            const face_t *f = &faces[face_for(size_px, bold)];
            a = f->ascent8;
            d = -f->descent8;
        } else {
            const oface_t *o = &ofaces[bold ? 1 : 0];
            int32_t scale = outline_scale(o, size_px);
            a = fu_to_px8(o->ascent, scale);
            d = -fu_to_px8(o->descent, scale);
        }
    }
    if (ascent8) *ascent8 = a;
    if (descent8) *descent8 = d;
}

// ---- outline flattening ----

typedef struct {
    int16_t *out;
    uint32_t n, max;     // pairs written / capacity in pairs
    bool overflow;
    int32_t ox8, oy8;    // pen x and baseline y, px8
    int32_t scale;       // 16.16 px8 per font unit
    int shift;           // extra right shift on the subdivision count (0 normal, 1 when retrying)
} flat_t;

static void emit(flat_t *f, int32_t x8, int32_t y8) {
    if (f->n >= f->max) { f->overflow = true; return; }
    if (x8 > PX8_MAX) x8 = PX8_MAX; else if (x8 < -PX8_MAX) x8 = -PX8_MAX;
    if (y8 > PX8_MAX) y8 = PX8_MAX; else if (y8 < -PX8_MAX) y8 = -PX8_MAX;
    f->out[f->n * 2] = (int16_t)x8;
    f->out[f->n * 2 + 1] = (int16_t)y8;
    f->n++;
}

static void emit_break(flat_t *f) {
    if (f->n >= f->max) { f->overflow = true; return; }
    f->out[f->n * 2] = GEOM_BREAK;
    f->out[f->n * 2 + 1] = 0;
    f->n++;
}

// Device px8 of a font-unit point: x right, y flipped about the baseline.
static inline int32_t dev_x(const flat_t *f, int32_t fx) { return f->ox8 + fu_to_px8(fx, f->scale); }
static inline int32_t dev_y(const flat_t *f, int32_t fy) { return f->oy8 - fu_to_px8(fy, f->scale); }

// Quadratic p0→p2 with control p1 (px8) as n chords, emitting the interior points and p2. The
// chord deviation of a quadratic is |p0-2p1+p2|/(4n²); the budget here is the looser |d|/(8n²) ≤
// 0.2 px from the design note, i.e. 5|d8| ≤ 64n² in px8. Integer square root by search: n ≤ 16.
static void emit_quad(flat_t *f, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    int32_t dx = x0 - 2 * x1 + x2, dy = y0 - 2 * y1 + y2;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    int32_t d = dx > dy ? dx : dy;
    int32_t n = 1;
    while (n < 16 && 5 * d > 64 * n * n) n++;
    n >>= f->shift;
    if (n < 1) n = 1;
    if (n > 1) {
        int32_t n2 = n * n, half = n2 / 2;
        for (int32_t i = 1; i < n; i++) {
            int32_t a = (n - i) * (n - i), b = 2 * i * (n - i), c = i * i;
            // Coefficients sum to n² (≤ 256) and coordinates are px8 within ±40k, so the products
            // fit int32. Round half away from zero so negative (off-screen) coordinates keep the
            // same rounding as positive ones.
            int32_t nx = a * x0 + b * x1 + c * x2, ny = a * y0 + b * y1 + c * y2;
            emit(f, (nx >= 0 ? nx + half : nx - half) / n2, (ny >= 0 ? ny + half : ny - half) / n2);
        }
    }
    emit(f, x2, y2);
}

// Walks one TrueType contour (points [first..last], flags bit0 = on-curve) with the implied
// on-curve midpoint between consecutive off-curve points. Iterative; no recursion.
static void flatten_contour(flat_t *f, const oface_t *o, uint32_t pts, uint32_t flags, uint32_t first, uint32_t last) {
    uint32_t n = last - first + 1;
    uint32_t P = o->points_off + pts + first * 4, F = o->flags_off + flags + first;
    // Find an on-curve start; an all-off-curve contour starts at the implied midpoint of its last
    // and first points and then visits every point as a control.
    uint32_t s = n;
    for (uint32_t i = 0; i < n; i++)
        if (rd8(F + i) & 1) { s = i; break; }
    int32_t sx, sy;
    uint32_t begin, count;
    if (s == n) {
        sx = (dev_x(f, rds16(P + (n - 1) * 4)) + dev_x(f, rds16(P))) >> 1;
        sy = (dev_y(f, rds16(P + (n - 1) * 4 + 2)) + dev_y(f, rds16(P + 2))) >> 1;
        begin = 0; count = n;
    } else {
        sx = dev_x(f, rds16(P + s * 4));
        sy = dev_y(f, rds16(P + s * 4 + 2));
        begin = s + 1; count = n - 1;
    }
    if (f->n) emit_break(f);
    emit(f, sx, sy);
    int32_t cx = sx, cy = sy;          // current on-curve point
    int32_t qx = 0, qy = 0; bool have_q = false;   // pending off-curve control
    for (uint32_t k = 0; k < count; k++) {
        uint32_t i = begin + k;
        if (i >= n) i -= n;   // wrap once; begin < n and k < n
        uint32_t p = P + i * 4;
        int32_t px = dev_x(f, rds16(p)), py = dev_y(f, rds16(p + 2));
        bool on = rd8(F + i) & 1;
        if (on) {
            if (have_q) emit_quad(f, cx, cy, qx, qy, px, py); else emit(f, px, py);
            cx = px; cy = py; have_q = false;
        } else if (have_q) {
            int32_t mx = (qx + px) >> 1, my = (qy + py) >> 1;
            emit_quad(f, cx, cy, qx, qy, mx, my);
            cx = mx; cy = my; qx = px; qy = py;
        } else {
            qx = px; qy = py; have_q = true;
        }
        if (f->overflow) return;
    }
    // Close: a trailing control curves back to the start; a straight closure is implicit.
    if (have_q) emit_quad(f, cx, cy, qx, qy, sx, sy);
}

// Flattens glyph record g into f->out. Returns false if it does not fit even after coarsening.
static bool flatten_glyph(flat_t *f, const oface_t *o, uint32_t g, uint32_t *ncont_out) {
    uint32_t npts = rd16(g + 12), ncont = rd16(g + 14);
    uint32_t pts = rd32(g + 16), flags = rd32(g + 20), ends = rd32(g + 24);
    if (ncont_out) *ncont_out = ncont;
    if (npts == 0) { f->n = 0; return true; }
    for (f->shift = 0; f->shift <= 1; f->shift++) {
        f->n = 0; f->overflow = false;
        uint32_t first = 0;
        for (uint32_t c = 0; c < ncont && !f->overflow; c++) {
            uint32_t last = rd16(o->ends_off + ends + c * 2);
            flatten_contour(f, o, pts, flags, first, last);
            first = last + 1;
        }
        if (!f->overflow) return true;
    }
    return false;
}

// Test hook (declared by host/tests/test_font.c): flattens cp at size_px with the pen at (0,0)
// into out (max pairs) and returns the pair count, -1 if absent, -2 if it does not fit.
int font_test_flatten_glyph(uint32_t cp, int size_px, bool bold, int16_t *out, uint32_t max_pairs, uint32_t *ncontours) {
    if (!ready) return -1;
    const oface_t *o = &ofaces[bold ? 1 : 0];
    uint32_t g = outline_glyph(o, cp);
    if (!g) return -1;
    flat_t f = { .out = out, .max = max_pairs, .scale = outline_scale(o, size_px) };
    if (!flatten_glyph(&f, o, g, ncontours)) return -2;
    return (int)f.n;
}

// ---- drawing ----

static void draw_bitmap(raster_t *r, int32_t pen8, int32_t baseline8, const char *s, const char *end, const face_t *f, uint8_t idx, uint8_t alpha) {
    int32_t baseline_px = px8_round(baseline8);
    while (s < end) {
        uint32_t cp = utf8_next(&s, end);
        uint32_t g = bitmap_glyph(f, cp);
        if (!g) { pen8 += bitmap_adv8(f, SPACE_CP); continue; }
        uint32_t w = rd8(g + 2), h = rd8(g + 3);
        if (w && h) {
            int32_t x = px8_round(pen8) + rds8(g + 4), y = baseline_px + rds8(g + 5);
            if (raster_band_intersects(r, x, y, x + (int32_t)w, y + (int32_t)h))
                raster_blit_glyph2(r, x, y, blob + f->bits_off + rd32(g + 8), (uint16_t)w, (uint16_t)h, (uint16_t)((w * 2 + 7) / 8), idx, alpha);
        }
        pen8 += (int32_t)rd16(g + 6);
    }
}

static void draw_outline(raster_t *r, int32_t pen8, int32_t baseline8, const char *s, const char *end, const oface_t *o, int size_px, uint8_t idx, uint8_t alpha) {
    int32_t scale = outline_scale(o, size_px);
    uint8_t ss = size_px < 40 ? OUTLINE_SS_SMALL : OUTLINE_SS_LARGE;
    while (s < end) {
        uint32_t cp = utf8_next(&s, end);
        uint32_t g = outline_glyph(o, cp);
        if (!g) { pen8 += outline_adv8(o, SPACE_CP, scale); continue; }
        uint32_t npts = rd16(g + 12);
        if (npts) {
            // Cull on the font-unit bbox before flattening; y1 (top) maps to the smaller device y.
            int32_t x0 = (pen8 + fu_to_px8(rds16(g + 4), scale)) >> PX8_SHIFT;
            int32_t x1 = ((pen8 + fu_to_px8(rds16(g + 8), scale) + PX8_ONE - 1) >> PX8_SHIFT);
            int32_t y0 = (baseline8 - fu_to_px8(rds16(g + 10), scale)) >> PX8_SHIFT;
            int32_t y1 = ((baseline8 - fu_to_px8(rds16(g + 6), scale) + PX8_ONE - 1) >> PX8_SHIFT);
            if (raster_band_intersects(r, x0, y0, x1 + 1, y1 + 1)) {
                flat_t f = { .out = verts, .max = KIOSK_GLYPH_MAX_VERTS, .ox8 = pen8, .oy8 = baseline8, .scale = scale };
                if (flatten_glyph(&f, o, g, NULL) && f.n >= 3)
                    raster_fill_poly_aa(r, verts, f.n, FILL_NONZERO, ss, idx, alpha);
            }
        }
        pen8 += fu_to_px8((int32_t)rd16(g + 2), scale);
    }
}

void font_draw(raster_t *r, int32_t x8, int32_t baseline8, const char *utf8, size_t len, int size_px, bool bold, uint8_t align, uint8_t idx, uint8_t alpha) {
    if (!ready || !r || !utf8 || len == 0 || size_px <= 0) return;
    if (align) {
        int32_t w8 = font_measure(utf8, len, size_px, bold);
        x8 -= align == 1 ? w8 / 2 : w8;
    }
    if (size_px <= KIOSK_FONT_BITMAP_MAX)
        draw_bitmap(r, x8, baseline8, utf8, utf8 + len, &faces[face_for(size_px, bold)], idx, alpha);
    else
        draw_outline(r, x8, baseline8, utf8, utf8 + len, &ofaces[bold ? 1 : 0], size_px, idx, alpha);
}
