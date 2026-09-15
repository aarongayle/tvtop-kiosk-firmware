// font_draw through the raster + palette into a small image assembled band by band. Checks
// placement (baseline, anchor, bbox), the bitmap/outline consistency at the size boundary, and
// that outline text is anti-aliased (more than two palette entries).
//
// Integration build links the real raster.c/palette.c/tmds.c. Standalone build (raster.c absent):
//   cc -std=c11 -DSTANDALONE_TEST -Isrc/common host/tests/test_font_draw.c src/common/font.c \
//      src/common/font_blob.c src/common/palette.c src/common/tmds.c -lm
// The standalone raster below is a deliberately naive reference (point-sampled winding, no
// clipping beyond the band) — enough to validate what font.c hands to it.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "font.h"
#include "geom.h"
#include "palette.h"
#include "raster.h"

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#ifdef STANDALONE_TEST
void raster_init(raster_t *r, uint8_t *band, uint8_t *scratch, uint16_t width, uint16_t height, palette_t *pal) {
    memset(r, 0, sizeof *r);
    r->band = band; r->width = width; r->height = height; r->pal = pal;
    r->cross = (int16_t *)scratch;
    r->ncross = scratch + KIOSK_BAND_LINES * KIOSK_MAX_CROSSINGS * 2;
    r->cov = r->ncross + KIOSK_BAND_LINES;
    r->clip_x0 = 0; r->clip_y0 = 0; r->clip_x1 = (int16_t)width; r->clip_y1 = (int16_t)height;
}
void raster_begin_band(raster_t *r, int16_t y0, int16_t lines, uint8_t bg_idx) {
    r->y0 = y0; r->lines = lines;
    memset(r->band, bg_idx, (size_t)lines * r->width);
}
bool raster_band_intersects(const raster_t *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    return x1 > 0 && x0 < r->width && y1 > r->y0 && y0 < r->y0 + r->lines;
}
static void put(raster_t *r, int32_t x, int32_t y, uint8_t idx, unsigned level) {
    if (x < 0 || x >= r->width || y < r->y0 || y >= r->y0 + r->lines || level == 0) return;
    uint8_t *p = r->band + (y - r->y0) * r->width + x;
    *p = level >= 255 ? idx : palette_blend(r->pal, *p, idx, (uint8_t)level);
}
void raster_blit_glyph2(raster_t *r, int32_t x_px, int32_t y_px, const uint8_t *bits, uint16_t w, uint16_t h, uint16_t stride, uint8_t idx, uint8_t alpha) {
    (void)alpha;
    for (int row = 0; row < h; row++)
        for (int col = 0; col < w; col++) {
            unsigned q = (bits[row * stride + (col >> 2)] >> (6 - 2 * (col & 3))) & 3;
            put(r, x_px + col, y_px + row, idx, q * 85);
        }
}
// Point-sampled nonzero winding at ss sub-rows × 4 sub-columns per pixel.
void raster_fill_poly_aa(raster_t *r, const int16_t *verts, uint32_t count, uint8_t rule, uint8_t ss, uint8_t idx, uint8_t alpha) {
    (void)rule; (void)alpha;
    static int32_t ex0[KIOSK_GLYPH_MAX_VERTS], ey0[KIOSK_GLYPH_MAX_VERTS], ex1[KIOSK_GLYPH_MAX_VERTS], ey1[KIOSK_GLYPH_MAX_VERTS];
    int ne = 0;
    int32_t minx = INT32_MAX, maxx = INT32_MIN, miny = INT32_MAX, maxy = INT32_MIN;
    uint32_t start = 0;
    for (uint32_t i = 0; i <= count; i++) {
        bool brk = i == count || verts[i * 2] == GEOM_BREAK;
        if (!brk) {
            if (verts[i * 2] < minx) minx = verts[i * 2];
            if (verts[i * 2] > maxx) maxx = verts[i * 2];
            if (verts[i * 2 + 1] < miny) miny = verts[i * 2 + 1];
            if (verts[i * 2 + 1] > maxy) maxy = verts[i * 2 + 1];
            continue;
        }
        for (uint32_t k = start; k < i; k++) {
            uint32_t n = k + 1 < i ? k + 1 : start;
            if (ne >= KIOSK_GLYPH_MAX_VERTS) break;
            ex0[ne] = verts[k * 2]; ey0[ne] = verts[k * 2 + 1];
            ex1[ne] = verts[n * 2]; ey1[ne] = verts[n * 2 + 1];
            ne++;
        }
        start = i + 1;
    }
    if (ne == 0) return;
    int32_t px0 = minx >> 3, px1 = (maxx + 7) >> 3, py0 = miny >> 3, py1 = (maxy + 7) >> 3;
    for (int32_t y = py0; y <= py1; y++) {
        if (y < r->y0 || y >= r->y0 + r->lines) continue;
        for (int32_t x = px0; x <= px1; x++) {
            int inside = 0, total = 0;
            for (int s = 0; s < ss; s++) {
                double sy = y * 8.0 + (2 * s + 1) * 4.0 / ss;
                for (int c = 0; c < 4; c++) {
                    double sx = x * 8.0 + 2 * c + 1;
                    int wind = 0;
                    for (int e = 0; e < ne; e++) {
                        if ((ey0[e] <= sy) == (ey1[e] <= sy)) continue;
                        double xi = ex0[e] + (sy - ey0[e]) * (double)(ex1[e] - ex0[e]) / (double)(ey1[e] - ey0[e]);
                        if (xi > sx) wind += ey1[e] > ey0[e] ? 1 : -1;
                    }
                    inside += wind != 0;
                    total++;
                }
            }
            put(r, x, y, idx, (unsigned)(inside * 255 / total));
        }
    }
}
#endif

#define W 200
#define H 100

static palette_t pal;
static raster_t r;
static uint8_t band[KIOSK_BAND_LINES * W];
static uint8_t scratch[RASTER_SCRATCH_BYTES];
static uint8_t image[H][W];
static uint8_t bg, fg;

static void render(const char *text, int size, int x, int baseline, bool bold, uint8_t align) {
    memset(image, bg, sizeof image);
    for (int y0 = 0; y0 < H; y0 += KIOSK_BAND_LINES) {
        int lines = H - y0 < KIOSK_BAND_LINES ? H - y0 : KIOSK_BAND_LINES;
        raster_begin_band(&r, (int16_t)y0, (int16_t)lines, bg);
        font_draw(&r, x * PX8_ONE, baseline * PX8_ONE, text, strlen(text), size, bold, align, fg, 255);
        memcpy(image[y0], band, (size_t)lines * W);
    }
}

typedef struct { int minx, maxx, miny, maxy, count, distinct; } ink_t;

// Ink statistics over columns [cx0, cx1).
static ink_t ink(int cx0, int cx1) {
    ink_t k = { W, -1, H, -1, 0, 0 };
    bool seen[PALETTE_SIZE] = { 0 };
    for (int y = 0; y < H; y++)
        for (int x = cx0; x < cx1; x++) {
            uint8_t v = image[y][x];
            if (!seen[v]) { seen[v] = true; k.distinct++; }
            if (v == bg) continue;
            k.count++;
            if (x < k.minx) k.minx = x;
            if (x > k.maxx) k.maxx = x;
            if (y < k.miny) k.miny = y;
            if (y > k.maxy) k.maxy = y;
        }
    return k;
}

static void check_ag(int size, int x, int baseline) {
    int32_t w8 = font_measure("Ag", 2, size, false), a8, d8;
    font_extent(size, false, &a8, &d8);
    int adv_a = (int)(font_measure("A", 1, size, false) >> PX8_SHIFT);
    render("Ag", size, x, baseline, false, 0);
    ink_t all = ink(0, W);
    CHECK(all.count > 0, "%d px: no ink", size);
    if (!all.count) return;
    int right = x + (int)((w8 + 7) >> PX8_SHIFT), top = baseline - (int)((a8 + 7) >> PX8_SHIFT), bottom = baseline + (int)((d8 + 7) >> PX8_SHIFT);
    CHECK(all.minx >= x - 1 && all.maxx <= right + 1, "%d px: ink x %d..%d outside %d..%d", size, all.minx, all.maxx, x, right);
    CHECK(all.miny >= top - 1 && all.maxy <= bottom + 1, "%d px: ink y %d..%d outside %d..%d", size, all.miny, all.maxy, top, bottom);
    CHECK(all.miny < baseline && all.maxy >= baseline, "%d px: expected ink above and below the baseline, got %d..%d", size, all.miny, all.maxy);
    // 'A' sits on the baseline: its lowest ink row is the one just above it. Both the bitmap and
    // the outline path must agree on this, or text jumps by a pixel across the size boundary.
    ink_t a = ink(x, x + adv_a);
    CHECK(a.count > 0 && a.maxy == baseline - 1, "%d px: 'A' bottom row %d, baseline %d", size, a.maxy, baseline);
    CHECK(a.miny <= baseline - size * 6 / 10, "%d px: 'A' top row %d too low for cap height", size, a.miny);
    // 'g' descends.
    ink_t g = ink(x + adv_a, W);
    CHECK(g.count > 0 && g.maxy >= baseline + size / 10, "%d px: 'g' bottom row %d, baseline %d", size, g.maxy, baseline);
}

int main(void) {
    if (!font_init(font_blob, font_blob_size)) { printf("FAIL font_init\n"); return 1; }
    palette_init(&pal);
    bg = palette_add(&pal, 0x000000);
    fg = palette_add(&pal, 0xffffff);
    raster_init(&r, band, scratch, W, H, &pal);

    check_ag(13, 10, 20);    // bitmap path
    check_ag(48, 10, 70);    // outline path

    // Anti-aliasing: the outline glyph blends, so more than {bg, fg} appear.
    render("Ag", 48, 10, 70, false, 0);
    ink_t k = ink(0, W);
    CHECK(k.distinct > 2, "48 px: only %d distinct palette indices (no anti-aliasing)", k.distinct);
    int solid = 0;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) solid += image[y][x] == fg;
    CHECK(solid > 200, "48 px: only %d fully covered pixels", solid);

    // Anchors: right-aligned ink ends at the anchor, centred ink straddles it.
    int32_t w8 = font_measure("Ag", 2, 48, false);
    render("Ag", 48, 150, 70, false, 2);
    k = ink(0, W);
    CHECK(k.count && k.maxx <= 150 && k.maxx >= 150 - 4, "right align: ink ends at %d, anchor 150", k.maxx);
    CHECK(k.minx >= 150 - (int)(w8 >> PX8_SHIFT) - 1, "right align: ink starts at %d, width %d", k.minx, (int)(w8 >> PX8_SHIFT));
    render("Ag", 48, 100, 70, false, 1);
    k = ink(0, W);
    CHECK(k.count && abs((k.minx + k.maxx) / 2 - 100) <= 3, "centre align: ink %d..%d around 100", k.minx, k.maxx);
    render("Ag", 13, 150, 20, false, 2);
    k = ink(0, W);
    CHECK(k.count && k.maxx <= 150 && k.maxx >= 150 - 3, "right align 13 px: ink ends at %d", k.maxx);

    // Bold is heavier.
    render("Ag", 48, 10, 70, false, 0);
    int regular = ink(0, W).count;
    render("Ag", 48, 10, 70, true, 0);
    CHECK(ink(0, W).count > regular, "bold not heavier than regular");

    // Partly off-image text (baseline near the top / left edge / below the bottom) draws what is
    // visible and never crashes; text entirely outside draws nothing.
    render("Ag", 48, 10, 5, false, 0);
    CHECK(ink(0, W).count > 0, "clipped top: no ink");
    render("Ag", 48, -20, 70, false, 0);
    CHECK(ink(0, W).count > 0, "clipped left: no ink");
    render("Ag", 132, 10, 90, false, 0);
    CHECK(ink(0, W).count > 0, "132 px: no ink");
    render("Ag", 48, 10, -50, false, 0);
    CHECK(ink(0, W).count == 0, "off-image text drew %d pixels", ink(0, W).count);
    render("Ag", 13, 10, 200, false, 0);
    CHECK(ink(0, W).count == 0, "off-image bitmap text drew %d pixels", ink(0, W).count);

    // A missing glyph advances (by the space) without drawing.
    render("A\xF0\x9F\x98\x94g", 13, 10, 20, false, 0);
    k = ink(0, W);
    int32_t wide = font_measure("A\xF0\x9F\x98\x94g", 6, 13, false);
    CHECK(k.count > 0 && wide > font_measure("Ag", 2, 13, false), "missing glyph should still advance");
    int adv_a = (int)(font_measure("A", 1, 13, false) >> PX8_SHIFT), adv_sp = (int)(font_measure(" ", 1, 13, false) >> PX8_SHIFT);
    CHECK(ink(10 + adv_a + 1, 10 + adv_a + adv_sp - 1).count == 0, "missing glyph drew something");

    // Empty string and size 0 are no-ops.
    render("", 13, 10, 20, false, 0);
    CHECK(ink(0, W).count == 0, "empty string drew");
    render("Ag", 0, 10, 20, false, 0);
    CHECK(ink(0, W).count == 0, "size 0 drew");

    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("test_font_draw: ok\n");
    return 0;
}
