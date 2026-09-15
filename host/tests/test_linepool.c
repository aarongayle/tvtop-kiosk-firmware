// Line pool ring: allocation order, wrap-around, refusal to overrun live data, dup lines, RLE.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "linepool.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static uint32_t lcg_state = 99;
static uint32_t lcg(void) { lcg_state = lcg_state * 1103515245u + 12345u; return lcg_state >> 8; }

static void test_rle(void) {
    static uint8_t px[OUT_MAX_W], back[OUT_MAX_W], enc[LINE_MAX_BYTES];
    // A run of 1280 → 5 spans (256 × 5).
    memset(px, 3, 1280);
    uint16_t n = rle_encode_line(px, 1280, enc, sizeof enc);
    CHECK(n == 10);
    rle_decode_line(enc, n, back, 1280);
    CHECK(memcmp(px, back, 1280) == 0);
    // Worst case: alternating pixels → 2 bytes per pixel, exactly LINE_MAX_BYTES.
    for (int i = 0; i < 1280; i++) px[i] = (uint8_t)(i & 1);
    n = rle_encode_line(px, 1280, enc, sizeof enc);
    CHECK(n == 2560);
    rle_decode_line(enc, n, back, 1280);
    CHECK(memcmp(px, back, 1280) == 0);
    CHECK(rle_encode_line(px, 1280, enc, 2559) == 0);   // does not fit → 0
    // Random lines round-trip.
    for (int k = 0; k < 200; k++) {
        uint32_t x = 0;
        while (x < 1280) { uint32_t run = 1 + lcg() % 300; if (run > 1280 - x) run = 1280 - x; memset(px + x, (int)(lcg() & 255), run); x += run; }
        n = rle_encode_line(px, 1280, enc, sizeof enc);
        CHECK(n > 0);
        rle_decode_line(enc, n, back, 1280);
        CHECK(memcmp(px, back, 1280) == 0);
    }
    // Width 640.
    memset(px, 5, 640);
    n = rle_encode_line(px, 640, enc, sizeof enc);
    CHECK(n == 6);
}

static void test_ring(void) {
    enum { POOL = 8192, LINES = 60, WIDTH = 100 };
    static uint8_t mem[POOL];
    linepool_t lp;
    linepool_init(&lp, mem, POOL, LINES, WIDTH);
    // Two frames of 60 lines × 100 bytes = 6000 bytes each: the second frame overwrites the first
    // as it goes and must never fail.
    for (int frame = 0; frame < 5; frame++) {
        linepool_frame_begin(&lp);
        for (uint16_t y = 0; y < LINES; y++) {
            uint8_t *p = linepool_alloc(&lp, y, 100);
            CHECK(p != NULL);
            if (!p) continue;
            CHECK(p >= mem && p + 100 <= mem + POOL);
            memset(p, (int)(frame * 7 + y), 100);
            linepool_commit(&lp, y, p, 100);
            line_ref_t r = linepool_reader_begin(&lp, y);
            CHECK(r.len == 100 && mem[r.off] == (uint8_t)(frame * 7 + y));
            linepool_reader_done(&lp);
        }
        CHECK(lp.stats_bytes_frame == LINES * 100);
        // Every published line still holds its own bytes after the frame.
        for (uint16_t y = 0; y < LINES; y++) {
            line_ref_t r = lp.line[y];
            CHECK(r.len == 100 && mem[r.off] == (uint8_t)(frame * 7 + y) && mem[r.off + 99] == (uint8_t)(frame * 7 + y));
        }
    }
    // Lines that grow: a frame whose lines are 200 bytes (12000 > pool) must eventually be
    // refused, and the pool must still be consistent (no line's bytes overlap another's).
    linepool_frame_begin(&lp);
    int refused = 0;
    for (uint16_t y = 0; y < LINES; y++) {
        uint8_t *p = linepool_alloc(&lp, y, 200);
        if (!p) { refused++; linepool_commit_dup(&lp, y); continue; }
        memset(p, 0xEE, 200);
        linepool_commit(&lp, y, p, 200);
    }
    CHECK(refused > 0);
    CHECK(lp.stats_alloc_fail == (uint32_t)refused);
    for (uint16_t a = 0; a < LINES; a++) {
        if (lp.line[a].flags & LINE_FLAG_DUP_PREV) { CHECK(lp.line[a].len == 0); continue; }
        for (uint16_t b = 0; b < LINES; b++) {
            if (a == b || (lp.line[b].flags & LINE_FLAG_DUP_PREV)) continue;
            uint32_t a0 = lp.line[a].off, a1 = a0 + lp.line[a].len, b0 = lp.line[b].off, b1 = b0 + lp.line[b].len;
            CHECK(a1 <= b0 || b1 <= a0);
        }
    }
    // Back to small lines: recovers.
    for (int frame = 0; frame < 3; frame++) {
        linepool_frame_begin(&lp);
        for (uint16_t y = 0; y < LINES; y++) {
            uint8_t *p = linepool_alloc(&lp, y, 50);
            CHECK(p != NULL);
            if (p) { memset(p, 1, 50); linepool_commit(&lp, y, p, 50); }
        }
    }
    // Max-size allocation on a 1280-wide pool.
    static uint8_t big[LINE_MAX_BYTES * 3];
    linepool_init(&lp, big, sizeof big, 2, OUT_MAX_W);
    uint8_t *p = linepool_alloc(&lp, 0, LINE_MAX_BYTES);
    CHECK(p != NULL);
    if (p) linepool_commit(&lp, 0, p, LINE_MAX_BYTES);
}

int main(void) {
    test_rle();
    test_ring();
    printf(failures ? "test_linepool: %d failure(s)\n" : "test_linepool: OK\n", failures);
    return failures ? 1 : 0;
}
