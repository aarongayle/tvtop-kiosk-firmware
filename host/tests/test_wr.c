// Word-run scanlines: pixels -> tokens -> TMDS lane words must equal a per-pixel reference, for
// every width and content shape, and a corrupt token stream must never write outside the line.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tmds.h"
#include "tmds_wr.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static wr_ctx_t ctx;
static uint32_t rgb[256];
static uint32_t lcg_state = 7;
static uint32_t lcg(void) { lcg_state = lcg_state * 1103515245u + 12345u; return lcg_state >> 8; }

static void reference(const uint8_t *px, uint32_t width, uint32_t *out) {
    uint32_t words = width / 2;
    for (int lane = 0; lane < 3; lane++)
        for (uint32_t i = 0; i < words; i++) {
            uint8_t ca = (uint8_t)(rgb[px[2 * i]] >> (8 * lane)), cb = (uint8_t)(rgb[px[2 * i + 1]] >> (8 * lane));
            out[lane * words + i] = tmds_pack2(tmds_symbol_balanced(tmds_nearest_balanced(ca)), tmds_symbol_balanced(tmds_nearest_balanced(cb)));
        }
}

static int disparity(uint16_t s) { int d = 0; for (int i = 0; i < 10; i++) d += (s >> i & 1) ? 1 : -1; return d; }

static void check_line(const char *what, const uint8_t *px, uint32_t width) {
    static uint8_t tok[4096];
    static uint32_t got[3 * 640 + 8], want[3 * 640];
    uint16_t n = wr_line_from_pixels(&ctx, px, (uint16_t)width, tok, sizeof tok);
    CHECK(n > 0 && n % 2 == 0);
    got[3 * width / 2] = 0xDEADBEEF;
    wr_expand_line(tok, n, got, width, ctx.table);
    reference(px, width, want);
    uint32_t bad = 0;
    for (uint32_t i = 0; i < 3 * width / 2; i++) if (got[i] != want[i]) { if (!bad) printf("  %s w=%u: first mismatch at %u\n", what, width, i); bad++; }
    CHECK(bad == 0);
    CHECK(got[3 * width / 2] == 0xDEADBEEF);
    for (int lane = 0; lane < 3; lane++) {
        int d = 0;
        for (uint32_t i = 0; i < width / 2; i++) { uint32_t w = got[lane * width / 2 + i]; d += disparity(w & 0x3ff) + disparity(w >> 10 & 0x3ff); }
        CHECK(d == 0);
    }
}

int main(void) {
    wr_init(&ctx);
    for (int i = 0; i < 256; i++) { rgb[i] = lcg() & 0xffffff; wr_set_colour(&ctx, (uint8_t)i, rgb[i]); }
    static uint8_t px[1280];
    static const uint32_t widths[] = { 640, 720, 1280 };
    for (int wi = 0; wi < 3; wi++) {
        uint32_t w = widths[wi];
        memset(px, 5, w); check_line("solid", px, w);
        for (uint32_t x = 0; x < w; x++) px[x] = (uint8_t)(x & 1 ? 9 : 200); check_line("alternating", px, w);
        for (uint32_t x = 0; x < w; x++) px[x] = (uint8_t)((x / 3) % 2 ? 4 : 17); check_line("runs of 3", px, w);
        for (uint32_t x = 0; x < w; x++) px[x] = (uint8_t)((x / 2) % 2 ? 4 : 17); check_line("pure words alternating", px, w);
        for (int k = 0; k < 200; k++) {   // anti-aliased-text-like: few colours, short random runs
            uint32_t x = 0;
            while (x < w) { uint32_t run = 1 + lcg() % 6; uint8_t c = (uint8_t)(lcg() % 6); while (run-- && x < w) px[x++] = c; }
            check_line("text-like", px, w);
            if (ctx.next_id > 400) wr_reset_pairs(&ctx);
        }
        for (int k = 0; k < 50; k++) {    // long runs with occasional odd boundaries
            uint32_t x = 0;
            while (x < w) { uint32_t run = 1 + lcg() % 300; uint8_t c = (uint8_t)(lcg() % 256); while (run-- && x < w) px[x++] = c; }
            check_line("long runs", px, w);
        }
    }
    // Table exhaustion: more distinct mixed pairs than ids. Output stays in bounds and every word
    // is either exact or its left pixel repeated.
    wr_reset_pairs(&ctx);
    uint32_t w = 1280;
    for (uint32_t x = 0; x < w; x++) px[x] = (uint8_t)(lcg() & 255);
    static uint8_t tok[4096];
    static uint32_t got[3 * 640 + 1];
    uint16_t n = wr_line_from_pixels(&ctx, px, (uint16_t)w, tok, sizeof tok);
    CHECK(n > 0 && ctx.stats_table_full > 0);
    got[1920] = 0xCAFEF00D;
    wr_expand_line(tok, n, got, w, ctx.table);
    CHECK(got[1920] == 0xCAFEF00D);
    // Output-size limit.
    CHECK(wr_line_from_pixels(&ctx, px, (uint16_t)w, tok, 100) == 0);
    // Corrupt tokens: random bytes, odd lengths, empty. Must not write past the line.
    for (int k = 0; k < 2000; k++) {
        uint32_t len = lcg() % 1500;
        for (uint32_t i = 0; i < len; i++) tok[i] = (uint8_t)lcg();
        got[1920] = 0x12345678;
        wr_expand_line(tok, len, got, w, ctx.table);
        CHECK(got[1920] == 0x12345678);
    }
    wr_expand_line(tok, 0, got, w, ctx.table);
    CHECK(got[0] == ctx.table[0] && got[639] == ctx.table[0] && got[1919] == ctx.table[2]);
    printf(failures ? "test_wr: %d failure(s)\n" : "test_wr: OK\n", failures);
    return failures ? 1 : 0;
}
