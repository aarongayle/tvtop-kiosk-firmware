// Run-length frame store. Each line is a byte string of (palette index, run length - 1) pairs
// summing to the line width. Lines live in a ring; the renderer commits lines in ascending y and
// each commit atomically replaces that line, so an update sweeps down the screen while the
// scanout keeps reading whatever is published.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "kiosk_config.h"

#define LINE_FLAG_DUP_PREV 1u   // line has no bytes: scanout repeats the previous line

typedef struct {
    uint32_t off;      // offset into pool
    uint16_t len;
    uint16_t flags;
} line_ref_t;

typedef struct {
    uint8_t *pool;
    uint32_t size;
    uint16_t lines, width;
    volatile uint32_t head;      // next write offset
    volatile uint32_t tail;      // oldest live byte
    volatile uint32_t live;      // bytes between tail and head (mod size)
    line_ref_t line[OUT_MAX_H];  // published lines
    // Reader guard: the scanout publishes the range it is currently reading so the writer never
    // overwrites it (the writer spins for the few microseconds needed).
    volatile uint32_t reader_off;
    volatile uint32_t reader_len;
    uint32_t stats_alloc_fail;
    uint32_t stats_bytes_frame;  // bytes committed since linepool_frame_begin
} linepool_t;

void linepool_init(linepool_t *lp, uint8_t *mem, uint32_t size, uint16_t lines, uint16_t width);
// Called before rendering a frame (resets per-frame stats).
void linepool_frame_begin(linepool_t *lp);
// Reserves up to max_len contiguous bytes for line y. NULL if the ring would overrun live data
// (caller then commits a LINE_FLAG_DUP_PREV line). Only one allocation may be outstanding.
uint8_t *linepool_alloc(linepool_t *lp, uint16_t y, uint16_t max_len);
// Publishes line y (p from linepool_alloc, len ≤ max_len) and frees the previous line y.
void linepool_commit(linepool_t *lp, uint16_t y, const uint8_t *p, uint16_t len);
void linepool_commit_dup(linepool_t *lp, uint16_t y);
// Scanout side: fetch a line and mark it as being read; call linepool_reader_done afterwards.
static inline line_ref_t linepool_reader_begin(linepool_t *lp, uint16_t y) {
    line_ref_t r = lp->line[y];
    lp->reader_off = r.off; lp->reader_len = r.len;
    return r;
}
static inline void linepool_reader_done(linepool_t *lp) { lp->reader_len = 0; }

// RLE helpers (portable). Encodes width pixels into out; returns bytes written or 0 if > max.
uint16_t rle_encode_line(const uint8_t *px, uint16_t width, uint8_t *out, uint16_t max);
void rle_decode_line(const uint8_t *spans, uint16_t len, uint8_t *px, uint16_t width);
