// raster.c tests: coverage rules (pixel-centre sampling), winding rules, clipping, band
// splitting, crossing overflow safety, strokes, bitmaps and glyph blits.
//
// Frames are rendered band by band into a full canvas exactly as frame_render does, so every
// primitive is exercised across band boundaries. With -DSTANDALONE_TEST a minimal palette is
// compiled in (palette.c/tmds.c not linked); otherwise the real palette is used. The tests map
// blended indices back to levels by asking the palette for the same blend, so they hold for both.
#include "raster.h"
#include "geom.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_MSG(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

extern uint32_t raster_stats_cross_dropped;

#ifdef STANDALONE_TEST
// Exact-mix palette stand-in with the same contract as palette.h (find-or-add, blend = new entry).
void palette_init(palette_t *p) { memset(p, 0, sizeof *p); }
uint8_t palette_add(palette_t *p, uint32_t rgb) {
    for (uint16_t i = 0; i < p->count; i++) if (p->rgb[i] == rgb) return (uint8_t)i;
    if (p->count >= PALETTE_SIZE) return 0;
    p->rgb[p->count] = rgb;
    return (uint8_t)p->count++;
}
uint8_t palette_blend(palette_t *p, uint8_t bg, uint8_t fg, uint8_t level) {
    if (level == 0 || bg == fg) return bg;
    if (level == 255) return fg;
    uint32_t cb = p->rgb[bg], cf = p->rgb[fg], m = 0;
    for (int s = 16; s >= 0; s -= 8) {
        unsigned a = (cb >> s) & 0xff, b = (cf >> s) & 0xff;
        m |= (uint32_t)((a * (255 - level) + b * level + 127) / 255) << s;
    }
    return palette_add(p, m);
}
#endif

// ---- band-by-band frame renderer ----

#define W 480
#define H 480

static uint8_t fb[W * H];
static palette_t pal;
static uint8_t band_buf[KIOSK_BAND_LINES * OUT_MAX_W];
static uint8_t scratch[RASTER_SCRATCH_BYTES];
static uint8_t BG, FG, FG2;

typedef void (*draw_fn)(raster_t *r, void *ctx);

// Renders the whole canvas with bands of `lines` rows, the first band starting at row `first`
// (bands before it are drawn shorter) so band alignment can be varied.
static void render_aligned(draw_fn draw, void *ctx, int lines, int first) {
    raster_t r;
    raster_init(&r, band_buf, scratch, W, H, &pal);
    memset(fb, BG, sizeof fb);
    int y = 0;
    while (y < H) {
        int n = lines;
        if (y < first && first - y < n) n = first - y;
        if (y + n > H) n = H - y;
        raster_begin_band(&r, (int16_t)y, (int16_t)n, BG);
        draw(&r, ctx);
        memcpy(fb + y * W, band_buf, (size_t)n * W);
        y += n;
    }
}
static void render(draw_fn draw, void *ctx) { render_aligned(draw, ctx, KIOSK_BAND_LINES, 0); }

static long count_idx(uint8_t idx) {
    long n = 0;
    for (long i = 0; i < W * H; i++) if (fb[i] == idx) n++;
    return n;
}
static long count_not_bg(void) { return W * H - count_idx(BG); }
static int px(int x, int y) { return fb[y * W + x]; }
// Level (0/85/170/255) that produced pixel index v over BG with FG, or -1 for anything else.
static int level_of(uint8_t v) {
    if (v == BG) return 0;
    if (v == FG) return 255;
    if (v == palette_blend(&pal, BG, FG, 85)) return 85;
    if (v == palette_blend(&pal, BG, FG, 170)) return 170;
    return -1;
}
static void bbox_not_bg(int *x0, int *y0, int *x1, int *y1) {
    *x0 = W; *y0 = H; *x1 = -1; *y1 = -1;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (px(x, y) != BG) {
                if (x < *x0) *x0 = x;
                if (x > *x1) *x1 = x;
                if (y < *y0) *y0 = y;
                if (y > *y1) *y1 = y;
            }
    (*x1)++; (*y1)++;
}

// ---- rects ----

typedef struct { int32_t x, y, w, h, rad; uint8_t alpha; } rect_arg;
static void draw_rect(raster_t *r, void *c) { rect_arg *a = c; raster_fill_rect(r, a->x, a->y, a->w, a->h, a->rad, FG, a->alpha); }

static void test_rects(void) {
    rect_arg a = { 10 * 8, 20 * 8, 30 * 8, 15 * 8, 0, 255 };
    render(draw_rect, &a);
    CHECK(count_not_bg() == 30 * 15);
    int x0, y0, x1, y1;
    bbox_not_bg(&x0, &y0, &x1, &y1);
    CHECK(x0 == 10 && y0 == 20 && x1 == 40 && y1 == 35);

    // Half-pixel offset keeps an integer width: centres in [10.5, 40.5) are 10..39 (pixel 10's
    // centre is 10.5, and the interval is closed at the start) → 30 px.
    a.x = 10 * 8 + 4;
    render(draw_rect, &a);
    CHECK(count_not_bg() == 30 * 15);
    bbox_not_bg(&x0, &y0, &x1, &y1);
    CHECK(x0 == 10 && x1 == 40);
    // x = 10.5, w = 30.25: [84, 326) contains centre 324 (pixel 40) → 31 px wide.
    a.w = 30 * 8 + 2;
    render(draw_rect, &a);
    CHECK(count_not_bg() == 31 * 15);
    // A rect thinner than a pixel that straddles a centre covers one column; one that misses
    // every centre covers nothing.
    rect_arg thin = { 100 * 8 + 2, 50 * 8, 4, 10 * 8, 0, 255 };   // [802, 806) contains 804
    render(draw_rect, &thin);
    CHECK(count_not_bg() == 10);
    thin.x = 100 * 8 + 5;                                          // [805, 809): no centre
    render(draw_rect, &thin);
    CHECK(count_not_bg() == 0);
    // Zero/negative sizes and alpha 0 draw nothing.
    rect_arg z = { 0, 0, 0, 80, 0, 255 };
    render(draw_rect, &z);
    CHECK(count_not_bg() == 0);
    z = (rect_arg){ 0, 0, 80, 80, 0, 0 };
    render(draw_rect, &z);
    CHECK(count_not_bg() == 0);
    // Alpha blends every pixel and the memo does not stick a wrong colour: all pixels equal.
    rect_arg half = { 8, 8, 20 * 8, 20 * 8, 0, 128 };
    render(draw_rect, &half);
    uint8_t v = (uint8_t)px(1, 1);
    CHECK(v != BG && v != FG);
    CHECK(count_idx(v) == 400 && count_not_bg() == 400);
}

static void test_rounded_rect(void) {
    int w = 60, h = 40, rad = 12;
    rect_arg a = { 20 * 8, 30 * 8, w * 8, h * 8, rad * 8, 255 };
    render(draw_rect, &a);
    long n = count_not_bg();
    // Corners removed: each corner loses (1 - pi/4) r² ≈ 0.2146 r².
    double expect = w * h - 4 * (1 - M_PI / 4) * rad * rad;
    CHECK_MSG(fabs(n - expect) < 0.03 * w * h, "n=%ld expect=%.0f", n, expect);
    CHECK(px(20, 30) == BG && px(79, 69) == BG && px(20, 69) == BG && px(79, 30) == BG);
    CHECK(px(20 + rad, 30) != BG && px(20, 30 + rad) != BG);
    // Symmetric about both axes (integer geometry).
    for (int y = 30; y < 70; y++)
        for (int x = 20; x < 80; x++) {
            CHECK(px(x, y) == px(20 + 79 - x, y));
            CHECK(px(x, y) == px(x, 30 + 69 - y));
        }
    // Radius clamped to min(w,h)/2: a "pill" with an absurd radius is a rounded rect, not empty.
    rect_arg pill = { 8 * 8, 8 * 8, 80 * 8, 20 * 8, 500 * 8, 255 };
    render(draw_rect, &pill);
    n = count_not_bg();
    expect = 80 * 20 - 4 * (1 - M_PI / 4) * 100;
    CHECK_MSG(fabs(n - expect) < 0.05 * 80 * 20, "n=%ld expect=%.0f", n, expect);
    CHECK(px(8 + 10, 8) != BG && px(8, 8) == BG);
}

// ---- circles ----

typedef struct { int32_t cx, cy, r, w; } circ_arg;
static void draw_circle(raster_t *r, void *c) { circ_arg *a = c; raster_fill_circle(r, a->cx, a->cy, a->r, FG, 255); }
static void draw_ring(raster_t *r, void *c) { circ_arg *a = c; raster_stroke_circle(r, a->cx, a->cy, a->r, a->w, FG, 255); }

static void test_circles(void) {
    static const int radii[] = { 10, 50, 200 };
    for (int k = 0; k < 3; k++) {
        int rad = radii[k];
        circ_arg a = { 240 * 8 + 4, 240 * 8 + 4, rad * 8, 0 };   // centre on a pixel centre
        render(draw_circle, &a);
        long n = count_not_bg();
        double expect = M_PI * rad * rad;
        CHECK_MSG(fabs(n - expect) < 0.03 * expect, "r=%d n=%ld expect=%.0f", rad, n, expect);
        // The centre sits on pixel 240's centre, so the mirror of pixel 240-k is 240+k.
        for (int y = 0; y < H; y++)
            for (int x = 1; x < 240; x++)
                if (px(x, y) != px(480 - x, y)) { CHECK_MSG(0, "asymmetry r=%d at %d,%d", rad, x, y); y = H; break; }
        CHECK(px(240, 240) != BG && px(240 + rad, 240) != BG && px(240 + rad + 1, 240) == BG);
    }
    circ_arg z = { 100 * 8, 100 * 8, 0, 0 };
    render(draw_circle, &z);
    CHECK(count_not_bg() == 0);
    // Ring: area ≈ pi (ro² - ri²); the hole is empty.
    circ_arg ring = { 240 * 8 + 4, 240 * 8 + 4, 100 * 8, 10 * 8 };
    render(draw_ring, &ring);
    long n = count_not_bg();
    double expect = M_PI * (105.0 * 105 - 95.0 * 95);
    CHECK_MSG(fabs(n - expect) < 0.03 * expect, "ring n=%ld expect=%.0f", n, expect);
    CHECK(px(240, 240) == BG && px(240 + 100, 240) != BG && px(240, 240 - 100) != BG);
    // Width larger than the diameter degenerates to a disc.
    circ_arg fat = { 240 * 8 + 4, 240 * 8 + 4, 5 * 8, 30 * 8 };
    render(draw_ring, &fat);
    CHECK(px(240, 240) != BG);
}

// ---- polygons ----

typedef struct { const int16_t *v; uint32_t n; uint8_t rule, ss, alpha; } poly_arg;
static void draw_poly(raster_t *r, void *c) { poly_arg *a = c; raster_fill_poly(r, a->v, a->n, a->rule, FG, a->alpha); }
static void draw_poly_aa(raster_t *r, void *c) { poly_arg *a = c; raster_fill_poly_aa(r, a->v, a->n, a->rule, a->ss, FG, a->alpha); }

static void test_pentagram(void) {
    // Regular pentagram: vertices in order 0,2,4,1,3 of a pentagon → self-intersecting.
    int16_t v[12];
    v[0] = GEOM_BREAK; v[1] = 0;
    for (int i = 0; i < 5; i++) {
        double ang = -M_PI / 2 + i * 2 * M_PI * 2 / 5;
        v[2 + i * 2] = (int16_t)lround(8 * (240 + 150 * cos(ang)));
        v[3 + i * 2] = (int16_t)lround(8 * (240 + 150 * sin(ang)));
    }
    poly_arg nz = { v, 6, FILL_NONZERO, 1, 255 }, eo = { v, 6, FILL_EVENODD, 1, 255 };
    render(draw_poly, &nz);
    long n_nz = count_not_bg();
    CHECK(px(240, 240) != BG);
    render(draw_poly, &eo);
    long n_eo = count_not_bg();
    CHECK(px(240, 240) == BG);          // the central pentagon is a hole under even-odd
    CHECK(n_eo < n_nz && n_eo > 0);
    // The star tips are filled under both rules.
    CHECK(px(240, 92) != BG);
    render(draw_poly, &nz);
    CHECK(px(240, 92) != BG);
}

static void test_triangle(void) {
    int16_t v[] = { GEOM_BREAK, 0, 37 * 8 + 3, 21 * 8 + 5, 301 * 8 + 1, 60 * 8 + 6, 120 * 8 + 7, 333 * 8 + 2 };
    poly_arg a = { v, 4, FILL_NONZERO, 1, 255 };
    render(draw_poly, &a);
    double x0 = v[2] / 8.0, y0 = v[3] / 8.0, x1 = v[4] / 8.0, y1 = v[5] / 8.0, x2 = v[6] / 8.0, y2 = v[7] / 8.0;
    double area = fabs((x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)) / 2;
    double perim = hypot(x1 - x0, y1 - y0) + hypot(x2 - x1, y2 - y1) + hypot(x0 - x2, y0 - y2);
    long n = count_not_bg();
    CHECK_MSG(fabs(n - area) <= perim, "n=%ld area=%.1f perim=%.1f", n, area, perim);
    // Reverse winding gives the same pixels; a stream without a leading marker too.
    render(draw_poly, &a);
    static uint8_t ref[W * H];
    memcpy(ref, fb, sizeof ref);
    int16_t rv[] = { v[6], v[7], v[4], v[5], v[2], v[3] };
    poly_arg b = { rv, 3, FILL_NONZERO, 1, 255 };
    render(draw_poly, &b);
    CHECK(memcmp(ref, fb, sizeof ref) == 0);
    // Degenerate input: fewer than 3 vertices, all-collinear, all-markers.
    int16_t two[] = { 10, 10, 100, 100 };
    poly_arg t = { two, 2, FILL_NONZERO, 1, 255 };
    render(draw_poly, &t);
    CHECK(count_not_bg() == 0);
    int16_t col[] = { 80, 80, 800, 800, 1600, 1600 };
    poly_arg cl = { col, 3, FILL_NONZERO, 1, 255 };
    render(draw_poly, &cl);
    CHECK(count_not_bg() == 0);
    int16_t marks[] = { GEOM_BREAK, 0, GEOM_BREAK, 0, GEOM_BREAK, 0 };
    poly_arg m = { marks, 3, FILL_NONZERO, 1, 255 };
    render(draw_poly, &m);
    CHECK(count_not_bg() == 0);
    // Two contours in one stream, second one a hole under even-odd.
    int16_t two_c[] = { GEOM_BREAK, 0, 80, 80, 1600, 80, 1600, 1600, 80, 1600,
                        GEOM_BREAK, 0, 400, 400, 1200, 400, 1200, 1200, 400, 1200 };
    poly_arg tc = { two_c, 10, FILL_EVENODD, 1, 255 };
    render(draw_poly, &tc);
    CHECK(count_not_bg() == 190L * 190 - 100L * 100);
    tc.rule = FILL_NONZERO;
    render(draw_poly, &tc);
    CHECK(count_not_bg() == 190L * 190);
}

typedef struct { draw_fn inner; void *ctx; int x0, y0, x1, y1; } clip_arg;
static void draw_clipped(raster_t *r, void *c) {
    clip_arg *a = c;
    raster_set_clip(r, (int16_t)a->x0, (int16_t)a->y0, (int16_t)a->x1, (int16_t)a->y1);
    a->inner(r, a->ctx);
    raster_clear_clip(r);
}

static void test_clip(void) {
    rect_arg big = { -50 * 8, -50 * 8, 800 * 8, 800 * 8, 0, 255 };
    clip_arg c = { draw_rect, &big, 100, 50, 300, 275 };
    render(draw_clipped, &c);
    CHECK(count_not_bg() == 200L * 225);
    int x0, y0, x1, y1;
    bbox_not_bg(&x0, &y0, &x1, &y1);
    CHECK(x0 == 100 && y0 == 50 && x1 == 300 && y1 == 275);
    // Same for a circle and a polygon and a line.
    circ_arg ci = { 200 * 8, 200 * 8, 180 * 8, 0 };
    c.inner = draw_circle; c.ctx = &ci;
    render(draw_clipped, &c);
    bbox_not_bg(&x0, &y0, &x1, &y1);
    CHECK(x0 >= 100 && y0 >= 50 && x1 <= 300 && y1 <= 275 && count_not_bg() > 0);
    int16_t tri[] = { -3000, -3000, 3800, 100, 200, 3800 };
    poly_arg p = { tri, 3, FILL_NONZERO, 1, 255 };
    c.inner = draw_poly; c.ctx = &p;
    render(draw_clipped, &c);
    bbox_not_bg(&x0, &y0, &x1, &y1);
    CHECK(x0 >= 100 && y0 >= 50 && x1 <= 300 && y1 <= 275 && count_not_bg() > 0);
    c.inner = draw_poly_aa; p.ss = 4;
    render(draw_clipped, &c);
    bbox_not_bg(&x0, &y0, &x1, &y1);
    CHECK(x0 >= 100 && y0 >= 50 && x1 <= 300 && y1 <= 275 && count_not_bg() > 0);
    // Empty and inverted clips draw nothing; a clip larger than the screen is harmless.
    c.inner = draw_rect; c.ctx = &big; c.x0 = 200; c.x1 = 200;
    render(draw_clipped, &c);
    CHECK(count_not_bg() == 0);
    c.x0 = 300; c.x1 = 100;
    render(draw_clipped, &c);
    CHECK(count_not_bg() == 0);
    c.x0 = -5000; c.y0 = -5000; c.x1 = 5000; c.y1 = 5000;
    render(draw_clipped, &c);
    CHECK(count_not_bg() == (long)W * H);
    // raster_band_intersects agrees with the clip.
    raster_t r;
    raster_init(&r, band_buf, scratch, W, H, &pal);
    raster_begin_band(&r, 40, 8, BG);
    raster_set_clip(&r, 100, 0, 200, 44);
    CHECK(raster_band_intersects(&r, 150, 30, 160, 41));
    CHECK(!raster_band_intersects(&r, 150, 44, 160, 60));    // below the clip
    CHECK(!raster_band_intersects(&r, 150, 30, 160, 40));    // above the band
    CHECK(!raster_band_intersects(&r, 200, 40, 300, 48));    // right of the clip
    CHECK(raster_band_intersects(&r, -100, -100, 101, 41));
}

static void test_partial_band(void) {
    // A triangle whose vertices lie far outside the canvas on every side: only its intersection
    // with the canvas is drawn and the count matches the analytic clipped area (a big wedge).
    // (-500,240) px, (500,-500) px and (500,240) px: the wedge covers rows 0..239 of the canvas.
    int16_t v[] = { GEOM_BREAK, 0, -4000, 240 * 8, 4000, -4000, 4000, 240 * 8 };
    poly_arg a = { v, 4, FILL_NONZERO, 1, 255 };
    render(draw_poly, &a);
    long n = count_not_bg();
    CHECK(n > 0 && n < (long)W * H);
    // Rows fully inside the wedge are full rows; the wedge is convex so every row is one span.
    for (int y = 0; y < H; y++) {
        int first = -1, last = -1;
        for (int x = 0; x < W; x++) if (px(x, y) != BG) { if (first < 0) first = x; last = x; }
        if (first >= 0) for (int x = first; x <= last; x++) if (px(x, y) == BG) { CHECK_MSG(0, "gap at row %d", y); break; }
    }
    // Coordinates at the int16 extremes must not overflow the interpolation.
    int16_t ext[] = { -32767, -32767, 32767, -32767, 32767, 32767, -32767, 32767 };
    poly_arg e = { ext, 4, FILL_NONZERO, 1, 255 };
    render(draw_poly, &e);
    CHECK(count_not_bg() == (long)W * H);
    int16_t sliver[] = { -32767, -32767, 32767, 32767, 32767, 32760 };
    poly_arg s = { sliver, 3, FILL_NONZERO, 1, 255 };
    render(draw_poly, &s);   // must not crash; content is whatever the sliver covers
}

static void test_crossing_overflow(void) {
    // 400 spokes: ~800 crossings on the central rows, far more than KIOSK_MAX_CROSSINGS. Canaries
    // around the scratch and band buffers must survive.
    enum { SPOKES = 400 };
    static int16_t v[2 + SPOKES * 4];
    v[0] = GEOM_BREAK; v[1] = 0;
    for (int i = 0; i < SPOKES; i++) {
        double a0 = i * 2 * M_PI / SPOKES, a1 = a0 + 0.3 * 2 * M_PI / SPOKES;
        v[2 + i * 4] = (int16_t)lround(8 * (240 + 230 * cos(a0)));
        v[3 + i * 4] = (int16_t)lround(8 * (240 + 230 * sin(a0)));
        v[4 + i * 4] = (int16_t)lround(8 * (240 + 20 * cos(a1)));
        v[5 + i * 4] = (int16_t)lround(8 * (240 + 20 * sin(a1)));
    }
    static uint8_t guarded[64 + RASTER_SCRATCH_BYTES + 64];
    static uint8_t gband[64 + KIOSK_BAND_LINES * OUT_MAX_W + 64];
    memset(guarded, 0xA5, sizeof guarded);
    memset(gband, 0xA5, sizeof gband);
    raster_t r;
    raster_init(&r, gband + 64, guarded + 64, W, H, &pal);
    raster_stats_cross_dropped = 0;
    for (int y = 0; y < H; y += KIOSK_BAND_LINES) {
        raster_begin_band(&r, (int16_t)y, KIOSK_BAND_LINES, BG);
        raster_fill_poly(&r, v, 1 + SPOKES * 2, FILL_NONZERO, FG, 255);
        raster_fill_poly_aa(&r, v, 1 + SPOKES * 2, FILL_NONZERO, 4, FG, 255);
    }
    CHECK(raster_stats_cross_dropped > 0);
    for (int i = 0; i < 64; i++) {
        CHECK(guarded[i] == 0xA5 && guarded[64 + RASTER_SCRATCH_BYTES + i] == 0xA5);
        CHECK(gband[i] == 0xA5 && gband[64 + KIOSK_BAND_LINES * OUT_MAX_W + i] == 0xA5);
    }
    // The last width..OUT_MAX_W bytes of each band row are never written (width < OUT_MAX_W).
    CHECK(r.stats_edge_visits > 0);
}

// ---- lines and strokes ----

typedef struct { int32_t x0, y0, x1, y1, w; } line_arg;
static void draw_line(raster_t *r, void *c) { line_arg *a = c; raster_line(r, a->x0, a->y0, a->x1, a->y1, a->w, FG, 255); }

static void test_lines(void) {
    // Vertical 1 px line at integer x = 100 spans [99.5, 100.5): column 99 only.
    line_arg v = { 100 * 8, 20 * 8, 100 * 8, 120 * 8, 8 };
    render(draw_line, &v);
    CHECK(count_not_bg() == 100);
    int x0, y0, x1, y1;
    bbox_not_bg(&x0, &y0, &x1, &y1);
    CHECK(x1 - x0 == 1 && y0 == 20 && y1 == 120);
    // Horizontal 1 px line at integer y = 50: one row.
    line_arg h = { 20 * 8, 50 * 8, 220 * 8, 50 * 8, 8 };
    render(draw_line, &h);
    CHECK(count_not_bg() == 200);
    bbox_not_bg(&x0, &y0, &x1, &y1);
    CHECK(y1 - y0 == 1 && x0 == 20 && x1 == 220);
    // Width below 1 px is promoted to 1 px so hairlines survive.
    line_arg hair = { 100 * 8, 20 * 8, 100 * 8, 120 * 8, 1 };
    render(draw_line, &hair);
    CHECK(count_not_bg() == 100);
    // Zero length draws nothing.
    line_arg z = { 50 * 8, 50 * 8, 50 * 8, 50 * 8, 40 };
    render(draw_line, &z);
    CHECK(count_not_bg() == 0);
    // Thin diagonal lines (several slopes) have no empty rows in their interior.
    static const int ends[][2] = { { 300, 300 }, { 300, 90 }, { 90, 300 }, { 250, 400 }, { 401, 33 } };
    for (int k = 0; k < 5; k++) {
        line_arg d = { 20 * 8 + 3, 20 * 8 + 5, ends[k][0] * 8 + 1, ends[k][1] * 8 + 6, 8 };
        render(draw_line, &d);
        for (int y = 22; y < ends[k][1] - 2; y++) {
            int any = 0;
            for (int x = 0; x < W; x++) if (px(x, y) != BG) { any = 1; break; }
            if (!any) { CHECK_MSG(0, "diagonal %d: empty row %d", k, y); break; }
        }
        long n = count_not_bg();
        // A 1 px line yields about one pixel per row (or column) along its major axis.
        double len = hypot(ends[k][0] - 20, ends[k][1] - 20);
        double major = fabs(ends[k][0] - 20) > fabs(ends[k][1] - 20) ? fabs(ends[k][0] - 20) : fabs(ends[k][1] - 20);
        CHECK_MSG(n >= major * 0.9 && n <= len * 1.6, "diagonal %d: n=%ld len=%.0f", k, n, len);
    }
    // Wide line: area ≈ length × width.
    line_arg wide = { 40 * 8, 60 * 8, 400 * 8, 300 * 8, 12 * 8 };
    render(draw_line, &wide);
    double len = hypot(360, 240);
    long n = count_not_bg();
    CHECK_MSG(fabs(n - len * 12) < len * 2, "wide n=%ld expect=%.0f", n, len * 12);
}

typedef struct { const int16_t *v; uint32_t n; int32_t w; bool closed, round; } stroke_arg;
static void draw_stroke(raster_t *r, void *c) { stroke_arg *a = c; raster_stroke_poly(r, a->v, a->n, a->w, a->closed, a->round, FG, 255); }

static void test_stroke_poly(void) {
    // Open horizontal polyline, width 5: the stroke is exactly 5 rows × 200 columns.
    int16_t seg[] = { GEOM_BREAK, 0, 20 * 8, 100 * 8, 220 * 8, 100 * 8 };
    stroke_arg a = { seg, 3, 5 * 8, false, false };
    render(draw_stroke, &a);
    int x0, y0, x1, y1;
    bbox_not_bg(&x0, &y0, &x1, &y1);
    CHECK(y1 - y0 == 5 && x1 - x0 == 200 && count_not_bg() == 1000);
    CHECK(y0 == 97 && y1 == 102);   // centred on y = 100: [97.5, 102.5), closed at the start
    // Closed square outline, width 4 at integer coordinates: every side is 4 px thick, and the
    // outer bbox is the square grown by 2.
    int16_t sq[] = { GEOM_BREAK, 0, 50 * 8, 50 * 8, 250 * 8, 50 * 8, 250 * 8, 250 * 8, 50 * 8, 250 * 8 };
    stroke_arg s = { sq, 5, 4 * 8, true, false };
    render(draw_stroke, &s);
    bbox_not_bg(&x0, &y0, &x1, &y1);
    CHECK(x0 == 48 && y0 == 48 && x1 == 252 && y1 == 252);
    CHECK(px(150, 150) == BG);
    for (int y = 60; y < 240; y++) {
        int n = 0;
        for (int x = 40; x < 60; x++) if (px(x, y) != BG) n++;
        if (n != 4) { CHECK_MSG(0, "side thickness %d at row %d", n, y); break; }
    }
    CHECK(count_not_bg() == 204L * 204 - 196L * 196);
    // Open: the closing side is missing.
    s.closed = false;
    render(draw_stroke, &s);
    CHECK(px(49, 150) == BG && px(251, 150) != BG);
    // Round joins add discs at the vertices: the corner outside the mitre gets filled and the
    // open ends get round caps.
    s.round = true;
    render(draw_stroke, &s);
    CHECK(px(49, 51) != BG && px(48, 48) == BG);
    // Sub-pixel width promoted to 1 px; a stream of one vertex with round joins is a dot.
    int16_t dot[] = { 100 * 8 + 4, 100 * 8 + 4 };
    stroke_arg d = { dot, 1, 6 * 8, false, true };
    render(draw_stroke, &d);
    long n = count_not_bg();
    CHECK_MSG(n > 20 && n < 40, "dot n=%ld", n);
    stroke_arg nothing = { dot, 1, 6 * 8, false, false };
    render(draw_stroke, &nothing);
    CHECK(count_not_bg() == 0);
}

// ---- bitmap ----

typedef struct { const uint8_t *bits; int modules; int x, y, size; } bmp_arg;
static void draw_bitmap(raster_t *r, void *c) { bmp_arg *a = c; raster_bitmap(r, a->x, a->y, a->size, (uint16_t)a->modules, a->bits, FG); }

static void test_bitmap(void) {
    enum { M = 29, STRIDE = (M + 7) / 8 };
    static uint8_t bits[M * STRIDE], inv[M * STRIDE];
    uint32_t seed = 12345;
    for (int i = 0; i < M * STRIDE; i++) { seed = seed * 1103515245u + 12345u; bits[i] = (uint8_t)(seed >> 16); inv[i] = (uint8_t)~bits[i]; }
    // Fractional origin and a size that does not divide by 29.
    bmp_arg a = { bits, M, 30 * 8 + 3, 40 * 8 + 6, 220 * 8 };
    render(draw_bitmap, &a);
    static uint8_t set_fb[W * H];
    memcpy(set_fb, fb, sizeof set_fb);
    a.bits = inv;
    render(draw_bitmap, &a);
    // Union of set and clear cells is the 220 × 220 square (pixel centres in the square), the
    // intersection is empty.
    int px0 = (30 * 8 + 3 + 3) / 8, py0 = (40 * 8 + 6 + 3) / 8;   // px_first of the origin
    long both = 0, either = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int s = set_fb[y * W + x] != BG, c = px(x, y) != BG;
            if (s && c) both++;
            if (s || c) either++;
            int in = x >= px0 && x < px0 + 220 && y >= py0 && y < py0 + 220;
            if ((s || c) != in) { CHECK_MSG(0, "cell tiling mismatch at %d,%d", x, y); y = H; break; }
        }
    CHECK(both == 0);
    CHECK(either == 220L * 220);
    // Each set cell is a 7- or 8-pixel wide block: check the first row of modules against the
    // computed edges.
    for (int j = 0; j < M; j++) {
        int e0 = (30 * 8 + 3 + (j * 220 * 8) / M + 3) / 8, e1 = (30 * 8 + 3 + ((j + 1) * 220 * 8) / M + 3) / 8;
        int set = (bits[j >> 3] >> (7 - (j & 7))) & 1;
        for (int x = e0; x < e1; x++) if ((set_fb[py0 * W + x] != BG) != set) { CHECK_MSG(0, "cell %d wrong at x=%d", j, x); break; }
    }
    // Bounds: zero modules, absurd modules and NULL bits are ignored.
    bmp_arg bad = { bits, 0, 0, 0, 100 * 8 };
    render(draw_bitmap, &bad);
    CHECK(count_not_bg() == 0);
    bad.modules = 5000;
    render(draw_bitmap, &bad);
    CHECK(count_not_bg() == 0);
    bad.modules = M; bad.bits = NULL;
    render(draw_bitmap, &bad);
    CHECK(count_not_bg() == 0);
    // A 1-module bitmap is a solid square.
    uint8_t one = 0x80;
    bmp_arg solid = { &one, 1, 10 * 8, 10 * 8, 50 * 8 };
    render(draw_bitmap, &solid);
    CHECK(count_not_bg() == 2500);
}

// ---- anti-aliased fill ----

static void test_aa(void) {
    // A 100 × 100 square rotated 30° about (240, 240): coverage sum ≈ area × 255.
    int16_t v[10];
    v[0] = GEOM_BREAK; v[1] = 0;
    double c = cos(M_PI / 6), s = sin(M_PI / 6);
    static const double corners[4][2] = { { -50, -50 }, { 50, -50 }, { 50, 50 }, { -50, 50 } };
    for (int i = 0; i < 4; i++) {
        v[2 + i * 2] = (int16_t)lround(8 * (240 + corners[i][0] * c - corners[i][1] * s));
        v[3 + i * 2] = (int16_t)lround(8 * (240 + corners[i][0] * s + corners[i][1] * c));
    }
    for (int ss = 1; ss <= 4; ss++) {
        poly_arg a = { v, 5, FILL_NONZERO, (uint8_t)ss, 255 };
        render(draw_poly_aa, &a);
        double sum = 0;
        int bad = 0;
        for (int i = 0; i < W * H; i++) { int l = level_of(fb[i]); if (l < 0) bad++; else sum += l; }
        CHECK(bad == 0);
        double expect = 10000.0 * 255;
        CHECK_MSG(fabs(sum - expect) < 0.02 * expect, "ss=%d sum=%.0f expect=%.0f", ss, sum, expect);
        // Edge pixels carry intermediate levels; the interior is solid.
        CHECK(px(240, 240) == FG);
        long partial = 0;
        for (int i = 0; i < W * H; i++) { int l = level_of(fb[i]); if (l == 85 || l == 170) partial++; }
        CHECK(partial > 100);
    }
    // Band alignment must not change the output (the glyph split across bands case): render with
    // bands starting at rows 0, 3 and 5 and with a band height of 1.
    static uint8_t ref[W * H];
    poly_arg a = { v, 5, FILL_NONZERO, 3, 255 };
    render_aligned(draw_poly_aa, &a, KIOSK_BAND_LINES, 0);
    memcpy(ref, fb, sizeof ref);
    render_aligned(draw_poly_aa, &a, KIOSK_BAND_LINES, 3);
    CHECK(memcmp(ref, fb, sizeof ref) == 0);
    render_aligned(draw_poly_aa, &a, KIOSK_BAND_LINES, 5);
    CHECK(memcmp(ref, fb, sizeof ref) == 0);
    render_aligned(draw_poly_aa, &a, 1, 0);
    CHECK(memcmp(ref, fb, sizeof ref) == 0);
    // Same for the non-AA filler.
    render_aligned(draw_poly, &a, KIOSK_BAND_LINES, 0);
    memcpy(ref, fb, sizeof ref);
    render_aligned(draw_poly, &a, KIOSK_BAND_LINES, 6);
    CHECK(memcmp(ref, fb, sizeof ref) == 0);
    // Integer-aligned square: exact, no partial pixels anywhere.
    int16_t sq[] = { 80 * 8, 80 * 8, 180 * 8, 80 * 8, 180 * 8, 180 * 8, 80 * 8, 180 * 8 };
    poly_arg q = { sq, 4, FILL_NONZERO, 4, 255 };
    render(draw_poly_aa, &q);
    CHECK(count_idx(FG) == 10000 && count_not_bg() == 10000);
    // Half-pixel offset: the two edge columns are half covered (level 85 or 170, never 255/0).
    int16_t hs[] = { 80 * 8 + 4, 80 * 8, 180 * 8 + 4, 80 * 8, 180 * 8 + 4, 180 * 8, 80 * 8 + 4, 180 * 8 };
    poly_arg hq = { hs, 4, FILL_NONZERO, 2, 255 };
    render(draw_poly_aa, &hq);
    int l0 = level_of((uint8_t)px(80, 100)), l1 = level_of((uint8_t)px(180, 100));
    CHECK((l0 == 85 || l0 == 170) && l0 == l1 && px(81, 100) == FG && px(179, 100) == FG);
    // Even-odd with a hole; alpha scales the levels.
    int16_t two_c[] = { GEOM_BREAK, 0, 80, 80, 1600, 80, 1600, 1600, 80, 1600,
                        GEOM_BREAK, 0, 400, 400, 1200, 400, 1200, 1200, 400, 1200 };
    poly_arg tc = { two_c, 10, FILL_EVENODD, 4, 255 };
    render(draw_poly_aa, &tc);
    CHECK(count_idx(FG) == 190L * 190 - 100L * 100 && px(100, 100) == BG);
    tc.alpha = 128;
    render(draw_poly_aa, &tc);
    CHECK(count_idx(FG) == 0 && count_not_bg() == 190L * 190 - 100L * 100);
    // The coverage row is left clean for the next call: a second small shape does not inherit.
    poly_arg small = { hs, 4, FILL_NONZERO, 2, 255 };
    render(draw_poly_aa, &small);
    memcpy(ref, fb, sizeof ref);
    render(draw_poly_aa, &tc);
    render(draw_poly_aa, &small);
    CHECK(memcmp(ref, fb, sizeof ref) == 0);
}

// ---- glyph blit ----

typedef struct { const uint8_t *bits; int x, y, w, h, stride; uint8_t alpha; } glyph_arg;
static void draw_glyph(raster_t *r, void *c) { glyph_arg *a = c; raster_blit_glyph2(r, a->x, a->y, a->bits, (uint16_t)a->w, (uint16_t)a->h, (uint16_t)a->stride, FG, a->alpha); }

static void test_glyph2(void) {
    // 5 × 2 glyph, stride 2: row 0 levels 0,1,2,3,3; row 1 levels 3,0,0,0,1.
    static const uint8_t bits[4] = { 0x1B, 0xC0, 0xC0, 0x40 };
    glyph_arg a = { bits, 10, 20, 5, 2, 2, 255 };
    render(draw_glyph, &a);
    CHECK(level_of((uint8_t)px(10, 20)) == 0 && level_of((uint8_t)px(11, 20)) == 85);
    CHECK(level_of((uint8_t)px(12, 20)) == 170 && level_of((uint8_t)px(13, 20)) == 255 && level_of((uint8_t)px(14, 20)) == 255);
    CHECK(level_of((uint8_t)px(10, 21)) == 255 && level_of((uint8_t)px(11, 21)) == 0 && level_of((uint8_t)px(14, 21)) == 85);
    CHECK(count_not_bg() == 6);
    // Alpha halves the levels: nothing reaches FG.
    a.alpha = 128;
    render(draw_glyph, &a);
    CHECK(count_idx(FG) == 0 && count_not_bg() == 6);
    // Clipped at the canvas edges and by a clip rect; split across a band boundary.
    glyph_arg edge = { bits, -3, H - 1, 5, 2, 2, 255 };
    render(draw_glyph, &edge);
    CHECK(count_not_bg() == 2 && level_of((uint8_t)px(0, H - 1)) == 255);   // columns 3,4 → x 0,1
    glyph_arg span = { bits, 100, KIOSK_BAND_LINES - 1, 5, 2, 2, 255 };
    render(draw_glyph, &span);
    CHECK(count_not_bg() == 6 && level_of((uint8_t)px(100, KIOSK_BAND_LINES)) == 255);
    clip_arg c = { draw_glyph, &a, 12, 0, 14, H };
    a.alpha = 255;
    render(draw_clipped, &c);
    CHECK(count_not_bg() == 2 && level_of((uint8_t)px(12, 20)) == 170 && level_of((uint8_t)px(13, 20)) == 255);
    // Blitting over a coloured background blends against that colour, not BG.
    render(draw_glyph, &a);
}

static void test_span_basic(void) {
    raster_t r;
    raster_init(&r, band_buf, scratch, W, H, &pal);
    raster_begin_band(&r, 16, 8, BG);
    raster_span(&r, 15, 0, 10, FG, 255);     // above the band
    raster_span(&r, 24, 0, 10, FG, 255);     // below the band
    raster_span(&r, 20, -50, 5, FG, 255);    // clipped left
    raster_span(&r, 21, W - 5, W + 50, FG, 255);   // clipped right
    raster_span(&r, 22, 30, 30, FG, 255);    // empty
    raster_span(&r, 22, 40, 30, FG, 255);    // inverted
    raster_span(&r, 23, 0, W, FG, 0);        // alpha 0
    long n = 0;
    for (int i = 0; i < 8 * W; i++) if (band_buf[i] != BG) n++;
    CHECK(n == 10);
    CHECK(band_buf[4 * W + 0] == FG && band_buf[4 * W + 4] == FG && band_buf[4 * W + 5] == BG);
    CHECK(band_buf[5 * W + W - 5] == FG && band_buf[5 * W + W - 1] == FG);
    CHECK(r.stats_spans == 2);
    // Blend over a mixed background: each pixel blends against its own colour.
    raster_span(&r, 18, 0, 4, FG2, 255);
    raster_span(&r, 18, 0, 8, FG, 128);
    CHECK(band_buf[2 * W + 0] == palette_blend(&pal, FG2, FG, 128));
    CHECK(band_buf[2 * W + 5] == palette_blend(&pal, BG, FG, 128));
    CHECK(band_buf[2 * W + 0] != band_buf[2 * W + 5]);
}

int main(void) {
    palette_init(&pal);
    BG = palette_add(&pal, 0x000000);
    FG = palette_add(&pal, 0xffffff);
    FG2 = palette_add(&pal, 0xff0000);
    test_span_basic();
    test_rects();
    test_rounded_rect();
    test_circles();
    test_pentagram();
    test_triangle();
    test_clip();
    test_partial_band();
    test_crossing_overflow();
    test_lines();
    test_stroke_poly();
    test_bitmap();
    test_aa();
    test_glyph2();
    if (failures) { printf("test_raster: %d failure(s)\n", failures); return 1; }
    printf("test_raster: all passed (band lines %d)\n", KIOSK_BAND_LINES);
    return 0;
}
