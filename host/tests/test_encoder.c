// tmds_rle_encode_line against an independent per-pixel reference built from tmds.h.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tmds.h"
#include "palette.h"
// The encoder prototype (scanout.h drags in the device-only libdvi headers).
void tmds_rle_encode_line(const uint8_t *spans, uint32_t nbytes, uint32_t *out, uint32_t width, const uint32_t *pair_lut, const uint16_t *sym_lut);

static int failures;
#define CHECK(cond) do { if (!(cond)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static uint32_t pair_lut[3][256];
static uint16_t sym_lut[3][256];
static uint32_t rgb[256];

static uint32_t lcg_state = 12345;
static uint32_t lcg(void) { lcg_state = lcg_state * 1103515245u + 12345u; return lcg_state >> 8; }

static void setup_luts(void) {
    for (int i = 0; i < 256; i++) {
        uint8_t r = tmds_nearest_balanced((uint8_t)(lcg() & 255)), g = tmds_nearest_balanced((uint8_t)(lcg() & 255)), b = tmds_nearest_balanced((uint8_t)(lcg() & 255));
        rgb[i] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        sym_lut[0][i] = tmds_symbol_balanced(b);
        sym_lut[1][i] = tmds_symbol_balanced(g);
        sym_lut[2][i] = tmds_symbol_balanced(r);
        for (int l = 0; l < 3; l++) pair_lut[l][i] = tmds_pack2(sym_lut[l][i], sym_lut[l][i]);
    }
}

// Reference: expand spans to pixels, then pack symbols pairwise per lane.
static void reference(const uint8_t *spans, uint32_t nbytes, uint32_t *out, uint32_t width) {
    static uint8_t px[4096];
    uint32_t x = 0;
    for (uint32_t i = 0; i + 1 < nbytes && x < width; i += 2) {
        uint32_t run = (uint32_t)spans[i + 1] + 1;
        if (run > width - x) run = width - x;
        memset(px + x, spans[i], run);
        x += run;
    }
    memset(px + x, 0, width - x);
    for (int l = 0; l < 3; l++)
        for (uint32_t i = 0; i < width / 2; i++)
            out[l * (width / 2) + i] = tmds_pack2(sym_lut[l][px[2 * i]], sym_lut[l][px[2 * i + 1]]);
}

static int disparity_of(uint16_t sym) { int d = 0; for (int i = 0; i < 10; i++) d += (sym >> i) & 1 ? 1 : -1; return d; }

static void run_case(const char *name, const uint8_t *spans, uint32_t nbytes, uint32_t width) {
    static uint32_t got[3 * 640], want[3 * 640];
    memset(got, 0xA5, sizeof got);
    reference(spans, nbytes, want, width);
    tmds_rle_encode_line(spans, nbytes, got, width, &pair_lut[0][0], &sym_lut[0][0]);
    uint32_t words = 3 * width / 2;
    uint32_t bad = 0;
    for (uint32_t i = 0; i < words; i++) if (got[i] != want[i]) { if (!bad) printf("  %s: first mismatch at word %u lane %u: got %08x want %08x\n", name, i % (width / 2), i / (width / 2), got[i], want[i]); bad++; }
    CHECK(bad == 0);
    // Lane disparity over the line sums to zero (balanced symbols only).
    for (int l = 0; l < 3; l++) {
        int d = 0;
        for (uint32_t i = 0; i < width / 2; i++) { uint32_t w = got[l * (width / 2) + i]; d += disparity_of(w & 0x3ff) + disparity_of((w >> 10) & 0x3ff); }
        CHECK(d == 0);
    }
}

static uint32_t make_spans(uint8_t *spans, uint32_t width, uint32_t max_run, uint32_t min_run) {
    uint32_t x = 0, n = 0;
    while (x < width) {
        uint32_t run = min_run + (max_run > min_run ? lcg() % (max_run - min_run + 1) : 0);
        if (run > width - x) run = width - x;
        if (run > 256) run = 256;
        spans[n++] = (uint8_t)(lcg() & 255);
        spans[n++] = (uint8_t)(run - 1);
        x += run;
    }
    return n;
}

int main(void) {
    setup_luts();
    static uint8_t spans[4096];
    for (int wi = 0; wi < 2; wi++) {
        uint32_t width = wi ? 1280 : 640;
        // Solid line: runs of 256.
        uint32_t n = 0;
        for (uint32_t x = 0; x < width; x += 256) { spans[n++] = 7; spans[n++] = (uint8_t)((width - x >= 256 ? 256 : width - x) - 1); }
        run_case("solid", spans, n, width);
        // Alternating single pixels (worst case).
        n = 0;
        for (uint32_t x = 0; x < width; x++) { spans[n++] = (uint8_t)(x & 1 ? 3 : 200); spans[n++] = 0; }
        run_case("alternating", spans, n, width);
        // Runs of 3 (every span straddles pair boundaries differently).
        n = make_spans(spans, width, 3, 3);
        run_case("runs of 3", spans, n, width);
        // Random runs 1..40.
        for (int k = 0; k < 50; k++) { n = make_spans(spans, width, 40, 1); run_case("random", spans, n, width); }
        // Random long runs up to 256.
        for (int k = 0; k < 20; k++) { n = make_spans(spans, width, 256, 100); run_case("long", spans, n, width); }
        // Short span list: the rest of the line is black (index 0).
        spans[0] = 9; spans[1] = 4;
        run_case("short list", spans, 2, width);
        // Overlong runs are clamped to the line.
        spans[0] = 9; spans[1] = 255; spans[2] = 8; spans[3] = 255; spans[4] = 7; spans[5] = 255; spans[6] = 6; spans[7] = 255; spans[8] = 5; spans[9] = 255; spans[10] = 4; spans[11] = 255;
        run_case("overlong", spans, 12, width);
        // Empty list.
        run_case("empty", spans, 0, width);
    }
    printf(failures ? "test_encoder: %d failure(s)\n" : "test_encoder: OK\n", failures);
    return failures ? 1 : 0;
}
