// Font blob + metrics + UTF-8 + outline flattening. Standalone build (no raster.c/palette.c):
//   cc -std=c11 -DSTANDALONE_TEST -Isrc/common host/tests/test_font.c src/common/font.c src/common/font_blob.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "font.h"
#include "geom.h"

// Internal hook from font.c (not in font.h on purpose).
int font_test_flatten_glyph(uint32_t cp, int size_px, bool bold, int16_t *out, uint32_t max_pairs, uint32_t *ncontours);

#ifdef STANDALONE_TEST
// font.c calls these; the real ones live in raster.c which is not linked here.
bool raster_band_intersects(const raster_t *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1) { (void)r; (void)x0; (void)y0; (void)x1; (void)y1; return false; }
void raster_blit_glyph2(raster_t *r, int32_t x, int32_t y, const uint8_t *b, uint16_t w, uint16_t h, uint16_t s, uint8_t i, uint8_t a) { (void)r; (void)x; (void)y; (void)b; (void)w; (void)h; (void)s; (void)i; (void)a; }
void raster_fill_poly_aa(raster_t *r, const int16_t *v, uint32_t n, uint8_t rule, uint8_t ss, uint8_t i, uint8_t a) { (void)r; (void)v; (void)n; (void)rule; (void)ss; (void)i; (void)a; }
#endif

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// Must match tools/fontgen/fontgen.c.
static const int SIZES[] = { 7, 8, 9, 10, 11, 12, 13, 14, 16, 19, 22, 24, 26 };
#define NSIZES ((int)(sizeof SIZES / sizeof SIZES[0]))
#define LARGE_MIN 22

static int charset_next(int cp) {
    if (cp < 0x20) return 0x20;
    if (cp < 0x7E) return cp + 1;
    if (cp < 0xA0) return 0xA0;
    if (cp < 0xFF) return cp + 1;
    if (cp < 0x2010) return 0x2010;
    if (cp < 0x2027) return cp + 1;
    if (cp < 0x2212) return 0x2212;
    return -1;
}
static int in_large_charset(int cp) {
    if (cp <= 0x7E) return 1;
    if (cp >= 0xC0 && cp <= 0xFF && cp != 0xD7 && cp != 0xF7) return 1;
    switch (cp) {
    case 0xB0: case 0xB7: case 0x2013: case 0x2014: case 0x2022: case 0x2026: case 0x2212: return 1;
    default: return 0;
    }
}

// Reads the same blob the way font.c does, to cross-check glyph presence independently.
static uint32_t rd16(const uint8_t *b, uint32_t o) { return b[o] | (b[o + 1] << 8); }
static uint32_t rd32(const uint8_t *b, uint32_t o) { return rd16(b, o) | (rd16(b, o + 2) << 16); }

static bool face_has(const uint8_t *b, uint32_t goff, uint32_t n, int cp) {
    for (uint32_t k = 0; k < n; k++)
        if ((int)rd16(b, goff + k * 12) == cp) return true;
    return false;
}

static void test_blob_layout(void) {
    const uint8_t *b = font_blob;
    CHECK(font_blob_size > 1000, "blob size %u", (unsigned)font_blob_size);
    CHECK(memcmp(b, "TVFN", 4) == 0, "magic");
    uint32_t nfaces = rd16(b, 6), faces_off = rd32(b, 12), outlines_off = rd32(b, 8);
    CHECK(nfaces == (uint32_t)(2 * NSIZES), "nfaces %u", nfaces);
    // Face order: regular sizes ascending, then bold.
    for (uint32_t i = 0; i < nfaces && i < (uint32_t)(2 * NSIZES); i++) {
        uint32_t rec = faces_off + i * 16;
        CHECK(b[rec] == SIZES[i % NSIZES] && b[rec + 1] == (i >= (uint32_t)NSIZES), "face %u is %u px bold=%u", i, b[rec], b[rec + 1]);
    }
    // Face 0 (10 px regular) defines what Roboto has: every charset codepoint except a handful of
    // the U+2010 block. Small faces must match it exactly; large faces must hold exactly the
    // large subset of it. The space is present everywhere.
    uint32_t n0 = rd16(b, faces_off + 2), g0 = rd32(b, faces_off + 8);
    int missing0 = 0;
    for (int cp = charset_next(0); cp > 0; cp = charset_next(cp))
        if (!face_has(b, g0, n0, cp)) { missing0++; CHECK(cp >= 0x2010 && cp <= 0x2027, "face 0 lacks U+%04X", cp); }
    CHECK(missing0 <= 6, "face 0 misses %d codepoints", missing0);
    for (uint32_t i = 0; i < nfaces; i++) {
        uint32_t rec = faces_off + i * 16;
        uint32_t n = rd16(b, rec + 2), goff = rd32(b, rec + 8);
        int size = b[rec];
        uint32_t expect_n = 0;
        for (int cp = charset_next(0); cp > 0; cp = charset_next(cp)) {
            bool want = face_has(b, g0, n0, cp) && (size < LARGE_MIN || in_large_charset(cp));
            bool present = face_has(b, goff, n, cp);
            CHECK(want == present, "face %u (%d px): U+%04X %s", i, size, cp, want ? "missing" : "unexpected");
            expect_n += want;
        }
        CHECK(n == expect_n, "face %u has %u glyphs, expected %u", i, n, expect_n);
        CHECK(face_has(b, goff, n, ' '), "face %u lacks the space", i);
        CHECK(rd16(b, rec + 4) > 0 && (int16_t)rd16(b, rec + 6) < 0, "face %u ascent/descent signs", i);
        // Sorted by codepoint (binary search relies on it).
        for (uint32_t k = 1; k < n; k++)
            CHECK(rd16(b, goff + k * 12) > rd16(b, goff + (k - 1) * 12), "face %u glyph table not sorted at %u", i, k);
    }
    for (int w = 0; w < 2; w++) {
        uint32_t rec = outlines_off + w * 24;
        CHECK(rd16(b, rec) == 2048, "upem %u", rd16(b, rec));
        CHECK(rd16(b, rec + 6) == n0, "outline glyph count %u vs %u", rd16(b, rec + 6), n0);
    }
}

static void test_measure(void) {
    // Server em widths (tvtop-kiosk-server/src/render/text.js): H .724 e .545 l .253 l .253 o .571
    double expect = 13.0 * (0.724 + 0.545 + 0.253 + 0.253 + 0.571) * 8;
    int32_t w = font_measure("Hello", 5, 13, false);
    CHECK(w > expect * 0.97 && w < expect * 1.03, "Hello@13 = %d px8, expected ~%.1f", w, expect);
    int32_t wb = font_measure("Hello", 5, 13, true);
    CHECK(wb > w, "bold %d not wider than regular %d", wb, w);
    // Outline metrics scale linearly with size and roughly agree with the bitmap face.
    int32_t w26 = font_measure("Hello", 5, 26, false), w52 = font_measure("Hello", 5, 52, false), w104 = font_measure("Hello", 5, 104, false);
    CHECK(abs(w52 - 2 * w26) <= 8 * 5, "26→52: %d vs %d", w26, w52);
    CHECK(abs(w104 - 2 * w52) <= 8, "52→104: %d vs %d", w52, w104);
    CHECK(font_measure("Hello", 5, 0, false) == 0, "size 0");
    CHECK(font_measure("", 0, 13, false) == 0, "empty");
    // Missing glyphs advance by the space width (emoji, U+25CE).
    int32_t sp = font_measure(" ", 1, 13, false), emo = font_measure("\xF0\x9F\x98\x94", 4, 13, false);
    CHECK(sp > 0 && emo == sp, "missing glyph advance %d vs space %d", emo, sp);
    CHECK(font_measure("\xE2\x97\x8E", 3, 48, false) == font_measure(" ", 1, 48, false), "missing outline glyph advance");
    // Non-ASCII glyphs the fixtures use are real glyphs (wider than nothing, not the space fallback).
    CHECK(font_measure("\xE2\x80\x94", 3, 13, false) > sp, "em dash is a glyph");
    CHECK(font_measure("\xE2\x88\x92", 3, 13, false) > 0, "minus sign");
    // Large faces: the middle dot the server emits at 22 px is kept; a dropped Latin-1 symbol
    // measures as a space, and the same text through the outline path is unaffected.
    int32_t sp22 = font_measure(" ", 1, 22, false);
    CHECK(font_measure("\xC2\xB7", 2, 22, false) != sp22 || sp22 == 0, "U+00B7 at 22 px should be a real glyph");
    CHECK(font_measure("\xC2\xBF", 2, 24, false) == font_measure(" ", 1, 24, false), "U+00BF at 24 px should fall back to the space");
    CHECK(font_measure("\xC2\xBF", 2, 12, false) != font_measure(" ", 1, 12, false), "U+00BF at 12 px is a glyph");
    CHECK(font_measure("\xC2\xBF", 2, 40, false) != font_measure(" ", 1, 40, false), "U+00BF outline is a glyph");
    int32_t a, d;
    font_extent(13, false, &a, &d);
    CHECK(a > 13 * 8 * 0.8 && a < 13 * 8 * 1.1 && d > 0 && d < 13 * 8 * 0.4, "extent@13 a=%d d=%d", a, d);
    font_extent(64, true, &a, &d);
    CHECK(a > 64 * 8 * 0.8 && a < 64 * 8 * 1.1 && d > 0 && d < 64 * 8 * 0.4, "extent@64 a=%d d=%d", a, d);
    // The extent of 26 (bitmap) and 27 (outline) agree closely: both paths share a baseline model.
    int32_t a26, d26, a27, d27;
    font_extent(26, false, &a26, &d26);
    font_extent(27, false, &a27, &d27);
    CHECK(abs(a27 - a26) <= 16 && abs(d27 - d26) <= 8, "26/27 extent mismatch a %d/%d d %d/%d", a26, a27, d26, d27);
}

static void test_size_selection(void) {
    CHECK(font_bitmap_size_for(27) == -1, "27 -> outline, got %d", font_bitmap_size_for(27));
    CHECK(font_bitmap_size_for(26) == 26, "26 -> %d", font_bitmap_size_for(26));
    CHECK(font_bitmap_size_for(18) == 19, "18 -> %d (ties up)", font_bitmap_size_for(18));
    CHECK(font_bitmap_size_for(20) == 19, "20 -> %d", font_bitmap_size_for(20));
    CHECK(font_bitmap_size_for(21) == 22, "21 -> %d", font_bitmap_size_for(21));
    CHECK(font_bitmap_size_for(23) == 24, "23 -> %d (ties up)", font_bitmap_size_for(23));
    CHECK(font_bitmap_size_for(25) == 26, "25 -> %d (ties up)", font_bitmap_size_for(25));
    CHECK(font_bitmap_size_for(11) == 11, "11 -> %d (exact since the 720x480 sizes)", font_bitmap_size_for(11));
    CHECK(font_bitmap_size_for(15) == 16, "15 -> %d (ties up)", font_bitmap_size_for(15));
    CHECK(font_bitmap_size_for(17) == 16, "17 -> %d (nearest)", font_bitmap_size_for(17));
    CHECK(font_bitmap_size_for(9) == 9, "9 -> %d", font_bitmap_size_for(9));
    CHECK(font_bitmap_size_for(1) == 7, "1 -> %d (smallest face)", font_bitmap_size_for(1));
    for (int i = 0; i < NSIZES; i++)
        CHECK(font_bitmap_size_for(SIZES[i]) == SIZES[i], "exact %d -> %d", SIZES[i], font_bitmap_size_for(SIZES[i]));
    // Snapping affects metrics: 17 px measures with the 16 px face, 18 px with the 19 px face.
    CHECK(font_measure("Hello", 5, 17, false) == font_measure("Hello", 5, 16, false), "17 measures as 16");
    CHECK(font_measure("Hello", 5, 18, false) == font_measure("Hello", 5, 19, false), "18 measures as 19");
}

static void test_utf8(void) {
    const char *s, *end;
    // valid
    s = "a\xC3\xA9\xE2\x80\x94\xF0\x9F\x98\x94"; end = s + 10;
    CHECK(utf8_next(&s, end) == 'a', "ascii");
    CHECK(utf8_next(&s, end) == 0xE9, "2-byte");
    CHECK(utf8_next(&s, end) == 0x2014, "3-byte");
    CHECK(utf8_next(&s, end) == 0x1F614, "4-byte");
    CHECK(s == end, "consumed all");
    CHECK(utf8_next(&s, end) == 0 && s == end, "at end returns 0 without advancing");
    // truncated 3-byte at end of buffer: FFFD, consumed what was there, never past end
    s = "\xE2\x80"; end = s + 2;
    CHECK(utf8_next(&s, end) == 0xFFFD && s == end, "truncated");
    // broken continuation: FFFD then the following ASCII byte survives
    s = "\xE2" "b"; end = s + 2;
    CHECK(utf8_next(&s, end) == 0xFFFD && s == end - 1, "broken continuation");
    CHECK(utf8_next(&s, end) == 'b', "byte after broken sequence");
    // stray continuation byte, 0xFF lead
    s = "\x80\xFF"; end = s + 2;
    CHECK(utf8_next(&s, end) == 0xFFFD && utf8_next(&s, end) == 0xFFFD && s == end, "stray bytes");
    // overlong encoding of '/', surrogate, > U+10FFFF
    s = "\xC0\xAF"; end = s + 2;
    CHECK(utf8_next(&s, end) == 0xFFFD && s == end, "overlong");
    s = "\xED\xA0\x80"; end = s + 3;
    CHECK(utf8_next(&s, end) == 0xFFFD && s == end, "surrogate");
    s = "\xF4\x90\x80\x80"; end = s + 4;
    CHECK(utf8_next(&s, end) == 0xFFFD && s == end, "> U+10FFFF");
    // a lead byte as the very last byte
    s = "x\xF0"; end = s + 2;
    utf8_next(&s, end);
    CHECK(utf8_next(&s, end) == 0xFFFD && s == end, "lead at end");
    // NUL is a codepoint, not a terminator (len-delimited)
    s = "\0"; end = s + 1;
    CHECK(utf8_next(&s, end) == 0 && s == end, "NUL advances");
    // A length shorter than the string never reads the bytes beyond it.
    s = "\xC3\xA9" "zz"; end = s + 1;
    CHECK(utf8_next(&s, end) == 0xFFFD && s == end, "len cuts a sequence");
}

// Counts closed contours in a GEOM_BREAK-delimited stream and checks its shape.
static void test_flatten(void) {
    static int16_t out[KIOSK_GLYPH_MAX_VERTS * 2];
    const int sizes[] = { 64, 132 };
    for (int i = 0; i < 2; i++) {
        uint32_t ncont = 0;
        int n = font_test_flatten_glyph('g', sizes[i], false, out, KIOSK_GLYPH_MAX_VERTS, &ncont);
        CHECK(n > 3 && n <= KIOSK_GLYPH_MAX_VERTS, "g@%d: %d pairs (contours %u)", sizes[i], n, ncont);
        CHECK(ncont == 2, "g has %u contours", ncont);   // Roboto 'g': outer + counter
        if (n <= 0) continue;
        int contours = 1, verts_in = 0, min_in = 1 << 30;
        int16_t minx = 32767, maxx = -32768, miny = 32767, maxy = -32768;
        for (int k = 0; k < n; k++) {
            if (out[k * 2] == GEOM_BREAK) {
                CHECK(k > 0 && k < n - 1, "break at stream edge");
                if (verts_in < min_in) min_in = verts_in;
                verts_in = 0; contours++;
                continue;
            }
            verts_in++;
            if (out[k * 2] < minx) minx = out[k * 2];
            if (out[k * 2] > maxx) maxx = out[k * 2];
            if (out[k * 2 + 1] < miny) miny = out[k * 2 + 1];
            if (out[k * 2 + 1] > maxy) maxy = out[k * 2 + 1];
        }
        if (verts_in < min_in) min_in = verts_in;
        CHECK(contours == (int)ncont, "stream has %d contours, glyph %u", contours, ncont);
        CHECK(min_in >= 3, "smallest contour has %d vertices", min_in);
        // bbox sanity at pen (0,0): 'g' spans ~0.57 em wide, descends below the baseline (y > 0)
        // and rises to x-height above it (y < 0).
        int em8 = sizes[i] * 8;
        CHECK(maxx - minx > em8 * 0.4 && maxx - minx < em8 * 0.7, "g@%d width %d px8", sizes[i], maxx - minx);
        CHECK(miny < -em8 * 0.4 && maxy > em8 * 0.15, "g@%d y range %d..%d", sizes[i], miny, maxy);
        // Curve subdivision produced more vertices at the larger size.
        if (i == 1) {
            uint32_t c2;
            int n64 = font_test_flatten_glyph('g', 64, false, out, KIOSK_GLYPH_MAX_VERTS, &c2);
            CHECK(n > n64, "132 px (%d) should flatten finer than 64 px (%d)", n, n64);
        }
    }
    // Every charset glyph fits the budget at the largest size the fixtures use, both weights.
    for (int w = 0; w < 2; w++)
        for (int cp = charset_next(0); cp > 0; cp = charset_next(cp)) {
            uint32_t c;
            int n = font_test_flatten_glyph((uint32_t)cp, 132, w, out, KIOSK_GLYPH_MAX_VERTS, &c);
            CHECK(n >= 0 || n == -1, "U+%04X %s@132 does not fit (%d)", cp, w ? "bold" : "regular", n);
        }
    // Space: present, no contours.
    uint32_t c;
    CHECK(font_test_flatten_glyph(' ', 64, false, out, KIOSK_GLYPH_MAX_VERTS, &c) == 0 && c == 0, "space outline");
    // A tiny budget forces the coarsening retry, then a clean failure rather than an overrun.
    memset(out, 0x7f, sizeof out);
    int n = font_test_flatten_glyph('@', 132, true, out, 8, &c);
    CHECK(n == -2, "@ in 8 pairs should not fit (%d)", n);
    CHECK(out[16] == 0x7f7f, "flattener wrote past its budget");
    CHECK(font_test_flatten_glyph(0x1F614, 64, false, out, KIOSK_GLYPH_MAX_VERTS, &c) == -1, "missing glyph");
    // Flattened chords stay short: no edge longer than ~1/6 em at 132 px on the round 'o', which
    // is the visible symptom of under-subdivision.
    n = font_test_flatten_glyph('o', 132, false, out, KIOSK_GLYPH_MAX_VERTS, &c);
    int longest = 0;
    for (int k = 1; k < n; k++) {
        if (out[k * 2] == GEOM_BREAK || out[(k - 1) * 2] == GEOM_BREAK) continue;
        int dx = abs(out[k * 2] - out[(k - 1) * 2]), dy = abs(out[k * 2 + 1] - out[(k - 1) * 2 + 1]);
        if (dx + dy > longest) longest = dx + dy;
    }
    CHECK(n > 0 && longest < 132 * 8 / 6, "o@132 longest chord %d px8 (%d verts)", longest, n);
}

static void test_init_rejects_garbage(void) {
    static uint8_t bad[64];
    CHECK(!font_init(bad, sizeof bad), "zeros accepted");
    memcpy(bad, font_blob, 64);
    CHECK(!font_init(bad, 64), "truncated blob accepted");
    CHECK(!font_init(font_blob, 8), "8-byte blob accepted");
    CHECK(!font_init(NULL, 0), "NULL accepted");
    CHECK(font_measure("x", 1, 13, false) == 0, "measure after failed init");
    CHECK(font_bitmap_size_for(13) == -1, "size lookup after failed init");
    // A blob cut anywhere inside the outline section must be rejected, never read past its end.
    CHECK(!font_init(font_blob, font_blob_size - 1), "blob short by one byte accepted");
    CHECK(!font_init(font_blob, font_blob_size / 2), "half blob accepted");
    // Corrupt the outline glyph count so a point offset lands outside the blob.
    static uint8_t *copy;
    copy = malloc(font_blob_size);
    memcpy(copy, font_blob, font_blob_size);
    uint32_t outlines_off = rd32(copy, 8);
    copy[outlines_off + 6] = 0xff; copy[outlines_off + 7] = 0xff;
    CHECK(!font_init(copy, font_blob_size), "corrupt outline glyph count accepted");
    memcpy(copy, font_blob, font_blob_size);
    uint32_t faces_off = rd32(copy, 12), g0 = rd32(copy, faces_off + 8);
    copy[g0 + 12 + 8] = 0xff; copy[g0 + 12 + 9] = 0xff; copy[g0 + 12 + 10] = 0xff; copy[g0 + 12 + 11] = 0x7f;   // glyph 1 bitmap offset
    CHECK(!font_init(copy, font_blob_size), "corrupt bitmap offset accepted");
    free(copy);
    CHECK(font_init(font_blob, font_blob_size), "real blob rejected");
}

int main(void) {
    if (!font_init(font_blob, font_blob_size)) { printf("FAIL font_init\n"); return 1; }
    test_blob_layout();
    test_measure();
    test_size_selection();
    test_utf8();
    test_flatten();
    test_init_rejects_garbage();
    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("test_font: ok (blob %u bytes)\n", (unsigned)font_blob_size);
    return 0;
}
