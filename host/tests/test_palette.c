// palette.c tests: quantisation error bound, find-or-add, overflow to the nearest entry, blend
// endpoints/monotonicity/cache, and reset semantics. Links against tmds.c for the level table.
#include "palette.h"
#include "tmds.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static int R(uint32_t c) { return (int)(c >> 16) & 0xff; }
static int G(uint32_t c) { return (int)(c >> 8) & 0xff; }
static int B(uint32_t c) { return (int)c & 0xff; }
static uint32_t RGB(int r, int g, int b) { return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b; }

static void test_quantise(void) {
    for (int v = 0; v < 256; v++) {
        uint8_t q = palette_quantize_component((uint8_t)v);
        CHECK(q == tmds_nearest_balanced((uint8_t)v));
        CHECK(tmds_is_balanced(q));
        // The gaps in the balanced set are at most 17 wide (17..34, 221..238), so the nearest
        // level is never more than 8 away; the ends (0..16, 239..255) are bounded by 16.
        CHECK(abs((int)q - v) <= 16);
        if (v >= 8 && v <= 247) CHECK(abs((int)q - v) <= 8);
    }
    // Per-component, independent, and the top byte is dropped.
    uint32_t q = palette_quantize_rgb(0xff000000u | RGB(0, 128, 255));
    CHECK(R(q) == 16 && G(q) == 136 && B(q) == 239);
    CHECK((q >> 24) == 0);
    // Idempotent on balanced input.
    CHECK(palette_quantize_rgb(q) == q);
}

static void test_add_find(void) {
    static palette_t p;
    palette_init(&p);
    CHECK(p.count == 0 && p.dirty_from == 0 && p.blend_count == 0 && p.stats_overflow == 0);
    CHECK(palette_find(&p, 0) == -1);

    uint8_t i0 = palette_add(&p, 0x000000);
    uint8_t i1 = palette_add(&p, 0xffffff);
    uint8_t i2 = palette_add(&p, 0x1e90ff);
    CHECK(i0 == 0 && i1 == 1 && i2 == 2 && p.count == 3);
    CHECK(palette_rgb(&p, i0) == RGB(16, 16, 16));
    CHECK(palette_rgb(&p, i1) == RGB(239, 239, 239));
    // Stored colours are quantised, so find must match on the quantised value from either side.
    CHECK(palette_find(&p, 0x000000) == 0);
    CHECK(palette_find(&p, RGB(16, 16, 16)) == 0);
    CHECK(palette_find(&p, 0x030303) == 0);          // quantises to the same entry
    CHECK(palette_find(&p, 0x1e90ff) == 2);
    CHECK(palette_find(&p, 0x123456) == -1);
    // Re-adding does not grow the palette; alpha/top byte is ignored.
    CHECK(palette_add(&p, 0xffffffff) == 1);
    CHECK(palette_add(&p, 0x010101) == 0);
    CHECK(p.count == 3);
    // dirty_from is the consumer's cursor: adding never moves it; the uploader sets it.
    CHECK(p.dirty_from == 0);
    p.dirty_from = p.count;
    palette_add(&p, 0x808080);
    CHECK(p.dirty_from == 3 && p.count == 4);
    CHECK(p.stats_overflow == 0);
    {
        // Distinct light colours stay distinct even where the balanced levels are sparse.
        palette_t lp;
        palette_init(&lp);
        uint8_t white = palette_add(&lp, 0xffffff);
        uint8_t cream = palette_add(&lp, 0xf8f4ec);
        CHECK(white != cream);
        CHECK(palette_rgb(&lp, white) == RGB(239, 239, 239));
        CHECK(palette_rgb(&lp, cream) == RGB(239, 239, 221));
        CHECK(palette_add(&lp, 0xf8f4ec) == cream);        // the same source finds its entry again
        CHECK(palette_add(&lp, 0xfdfdfd) == white);        // near-identical sources still share one
        palette_t order;                                   // the same result when cream arrives first
        palette_init(&order);
        uint8_t cream_first = palette_add(&order, 0xf8f4ec);
        uint8_t white_second = palette_add(&order, 0xffffff);
        CHECK(cream_first != white_second);
        CHECK(palette_rgb(&order, cream_first) == RGB(239, 239, 221));
        CHECK(palette_rgb(&order, white_second) == RGB(239, 239, 239));
        CHECK(palette_rgb(&order, palette_add(&order, 0xf2f2f2)) == RGB(221, 221, 221));   // neutral grey stays neutral
        uint16_t gen = lp.generation;
        palette_reset(&lp);
        CHECK(lp.generation != gen);
    }
}

// Fills the palette with 256 distinct quantised colours (there are 52^3 to choose from).
static void fill(palette_t *p) {
    int n = 0;
    for (int r = 0; r < TMDS_BALANCED_COUNT && n < PALETTE_SIZE; r += 4)
        for (int g = 0; g < TMDS_BALANCED_COUNT && n < PALETTE_SIZE; g += 4)
            for (int b = 0; b < TMDS_BALANCED_COUNT && n < PALETTE_SIZE; b += 4) {
                uint32_t c = RGB(tmds_balanced_levels[r], tmds_balanced_levels[g], tmds_balanced_levels[b]);
                uint8_t i = palette_add(p, c);
                CHECK(i == n);
                n++;
            }
    CHECK(p->count == PALETTE_SIZE);
}

static int nearest_index(const palette_t *p, uint32_t q) {
    long best = -1; int bi = -1;
    for (int i = 0; i < PALETTE_SIZE; i++) {
        long dr = R(q) - R(p->rgb[i]), dg = G(q) - G(p->rgb[i]), db = B(q) - B(p->rgb[i]);
        long d = dr * dr + dg * dg + db * db;
        if (best < 0 || d < best) { best = d; bi = i; }
    }
    return bi;
}

static void test_overflow(void) {
    static palette_t p;
    palette_init(&p);
    fill(&p);
    // An existing colour still finds its own slot, without counting as an overflow.
    CHECK(palette_add(&p, p.rgb[200]) == 200);
    CHECK(p.stats_overflow == 0);
    // Absent colours map to the nearest entry by squared distance; the count does not grow.
    uint32_t probes[] = { 0x000000, 0xffffff, 0xff0000, 0x00ff00, 0x0000ff, 0x123456, 0xfedcba, 0x7f7f7f };
    for (size_t k = 0; k < sizeof probes / sizeof probes[0]; k++) {
        uint32_t q = palette_quantize_rgb(probes[k]);
        int want = palette_find(&p, probes[k]);
        if (want < 0) want = nearest_index(&p, q);
        uint8_t got = palette_add(&p, probes[k]);
        if (got != want) { failures++; printf("FAIL overflow %06x -> %u, want %d\n", probes[k], got, want); }
        // Ties are possible in principle; check the distance rather than the index in that case.
        long dg = 0, dw = 0;
        long a, b, c;
        a = R(q) - R(p.rgb[got]); b = G(q) - G(p.rgb[got]); c = B(q) - B(p.rgb[got]); dg = a * a + b * b + c * c;
        a = R(q) - R(p.rgb[want]); b = G(q) - G(p.rgb[want]); c = B(q) - B(p.rgb[want]); dw = a * a + b * b + c * c;
        CHECK(dg == dw);
    }
    CHECK(p.count == PALETTE_SIZE);
    CHECK(p.stats_overflow > 0 && p.stats_overflow <= sizeof probes / sizeof probes[0]);
    // A blend in a full palette must also resolve (to a nearest entry), never fail.
    uint32_t ov = p.stats_overflow;
    uint8_t bi = palette_blend(&p, 0, 255, 128);
    CHECK(p.count == PALETTE_SIZE);
    CHECK(p.stats_overflow == ov + 1 || palette_find(&p, palette_rgb(&p, bi)) == bi);
}

static void test_blend(void) {
    static palette_t p;
    palette_init(&p);
    uint8_t bg = palette_add(&p, 0x000000);
    uint8_t fg = palette_add(&p, 0xffffff);
    uint8_t red = palette_add(&p, 0xff0000);
    uint16_t before = p.count;

    // Endpoints: level 0 -> bg, 255 -> fg, and nothing is allocated or cached for them.
    CHECK(palette_blend(&p, bg, fg, 0) == bg);
    CHECK(palette_blend(&p, bg, fg, 255) == fg);
    CHECK(palette_blend(&p, fg, bg, 255) == bg);
    CHECK(p.count == before && p.blend_count == 0);
    // Levels quantise to the nearest of 0,32,...,224,255: 15 is still bg, 240 is already fg.
    CHECK(palette_blend(&p, bg, fg, 15) == bg);
    CHECK(palette_blend(&p, bg, fg, 240) == fg);
    CHECK(p.count == before && p.blend_count == 0);
    CHECK(palette_blend(&p, bg, fg, 16) != bg);
    CHECK(palette_blend(&p, bg, fg, 239) != fg);
    // Same colour both sides: nothing to mix.
    CHECK(palette_blend(&p, red, red, 100) == red);

    // Monotone in level per component, and the two 1/8 steps around each boundary agree.
    uint32_t prev = palette_rgb(&p, bg);
    uint8_t prev_idx = bg;
    for (int level = 0; level < 256; level++) {
        uint8_t idx = palette_blend(&p, bg, fg, (uint8_t)level);
        uint32_t c = palette_rgb(&p, idx);
        if (!(R(c) >= R(prev) && G(c) >= G(prev) && B(c) >= B(prev))) {
            failures++;
            printf("FAIL blend not monotone at level %d: %06x after %06x\n", level, c, prev);
        }
        // Within one quantised step the index must not change.
        if (((level + 16) >> 5) == ((level - 1 + 16) >> 5) && level > 0) CHECK(idx == prev_idx);
        prev = c; prev_idx = idx;
    }
    // Exactly the 7 intermediate steps were allocated for this pair.
    CHECK(p.count == before + 7);
    CHECK(p.blend_count == 7);
    // Mixing is rounded: 128/255 of white over black at step 4 (weight 128/256) is 0x80 -> 136.
    uint32_t mid = palette_rgb(&p, palette_blend(&p, bg, fg, 128));
    CHECK(R(mid) == tmds_nearest_balanced(((16 * 128) + (239 * 128) + 128) >> 8));
    CHECK(R(mid) == G(mid) && G(mid) == B(mid));

    // Cache hit: the same triple returns the same index and allocates nothing.
    uint8_t a = palette_blend(&p, bg, red, 96);
    uint16_t cnt = p.count; uint8_t bc = p.blend_count;
    CHECK(palette_blend(&p, bg, red, 96) == a);
    CHECK(palette_blend(&p, bg, red, 100) == a);   // same quantised level
    CHECK(p.count == cnt && p.blend_count == bc);
    // bg/fg order matters: fg over bg at level L equals bg over fg at 255-L only up to rounding,
    // but the cache key must not conflate them.
    uint8_t b = palette_blend(&p, red, bg, 96);
    CHECK(b != a);
    uint32_t ca = palette_rgb(&p, a), cb = palette_rgb(&p, b);
    CHECK(R(ca) < R(cb));   // a is mostly black, b mostly red

    // Cache pressure: more distinct triples than PALETTE_BLEND_CACHE still resolve correctly
    // (evicted entries are simply recomputed to the same palette index).
    // Distinct bg colours (r and g vary) so every triple is new.
    uint8_t firsts[PALETTE_BLEND_CACHE + 8];
    for (int i = 0; i < PALETTE_BLEND_CACHE + 8; i++) {
        uint32_t c = RGB(tmds_balanced_levels[i % TMDS_BALANCED_COUNT], tmds_balanced_levels[(i / TMDS_BALANCED_COUNT) * 20 + 5], 239);
        firsts[i] = palette_blend(&p, palette_add(&p, c), fg, 96);
    }
    CHECK(p.blend_count == PALETTE_BLEND_CACHE);
    for (int i = 0; i < PALETTE_BLEND_CACHE + 8; i++) {
        uint32_t c = RGB(tmds_balanced_levels[i % TMDS_BALANCED_COUNT], tmds_balanced_levels[(i / TMDS_BALANCED_COUNT) * 20 + 5], 239);
        CHECK(palette_blend(&p, palette_add(&p, c), fg, 96) == firsts[i]);
    }
    CHECK(p.count < PALETTE_SIZE);
}

static void test_reset(void) {
    static palette_t p;
    palette_init(&p);
    palette_add(&p, 0x123456);
    palette_blend(&p, 0, palette_add(&p, 0xffffff), 128);
    p.dirty_from = p.count;
    p.stats_overflow = 5;
    palette_reset(&p);
    CHECK(p.count == 0 && p.dirty_from == 0 && p.blend_count == 0 && p.blend_next == 0);
    CHECK(p.stats_overflow == 0);
    CHECK(palette_find(&p, 0x123456) == -1);
    // A blend after reset must not return a stale cached index.
    uint8_t w = palette_add(&p, 0xffffff);   // index 0 now
    uint8_t k = palette_add(&p, 0x000000);   // index 1
    CHECK(w == 0 && k == 1);
    uint8_t m = palette_blend(&p, w, k, 128);
    CHECK(m == 2);
}

int main(void) {
    test_quantise();
    test_add_find();
    test_overflow();
    test_blend();
    test_reset();
    if (failures) { printf("test_palette: %d failure(s)\n", failures); return 1; }
    printf("test_palette: OK\n");
    return 0;
}
