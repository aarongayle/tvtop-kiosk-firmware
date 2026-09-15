// Run-length frame store (see linepool.h).
//
// The pool is a ring of contiguous allocations, one per committed line. Because the renderer
// commits lines 0..lines-1 in ascending order every frame, allocations are made in ring order and
// the live allocations, oldest first, are always: old lines y+1..end, then new lines 0..y. The
// tail (oldest live byte) therefore advances when old line y is freed by the commit of new line
// y. A wrap leaves dead padding at the end of the ring; it is skipped when the next live line is
// found to start below the freed one.
//
// Publication ordering (for the scanout on the other core): linepool_commit writes off and len
// first, a barrier, then flags; linepool_commit_dup writes flags first, a barrier, then len = 0.
// The reader copies line[y] as two 32-bit words (off | len,flags) and, as the M0+ has no 64-bit
// load, may observe a mix of the old and the new record. The bytes of both records stay valid
// (the old ones are freed only *after* the new record is published and the reader guard covers a
// line being read), so a torn read draws at worst one wrong-but-decodable line for one frame,
// and a DUP flag is always seen before the len it belongs to.
#include "linepool.h"
#include <string.h>

#if defined(__GNUC__)
#define LP_BARRIER() __sync_synchronize()
#else
#define LP_BARRIER() ((void)0)
#endif

static inline uint32_t ring_dist(const linepool_t *lp, uint32_t from, uint32_t to) {
    return to >= from ? to - from : to + lp->size - from;
}

void linepool_init(linepool_t *lp, uint8_t *mem, uint32_t size, uint16_t lines, uint16_t width) {
    memset(lp, 0, sizeof *lp);
    lp->pool = mem;
    lp->size = size;
    lp->lines = lines > OUT_MAX_H ? OUT_MAX_H : lines;
    lp->width = width;
    // Nothing has been drawn yet: every line repeats its predecessor, so the scanout shows line 0
    // (whatever it substitutes for "previous of the first line", typically black) everywhere.
    for (uint32_t y = 0; y < lp->lines; y++) lp->line[y].flags = LINE_FLAG_DUP_PREV;
}

void linepool_frame_begin(linepool_t *lp) { lp->stats_bytes_frame = 0; }

// Waits while [off, off+len) overlaps the range the scanout is reading. The scanout reads one
// line in a few microseconds, so this spins briefly at most; on the host reader_len is always 0.
static void guard_wait(const linepool_t *lp, uint32_t off, uint32_t len) {
    for (;;) {
        uint32_t rl = lp->reader_len;
        if (rl == 0) return;
        uint32_t ro = lp->reader_off;
        if (off + len <= ro || ro + rl <= off) return;
    }
}

uint8_t *linepool_alloc(linepool_t *lp, uint16_t y, uint16_t max_len) {
    (void)y;
    if (max_len == 0) max_len = 1;
    if (lp->live == 0) { lp->head = 0; lp->tail = 0; }   // empty: start over for maximum contiguous room
    // Keep at least one byte unused so head == tail always means "empty".
    if ((uint32_t)max_len >= lp->size - lp->live) { lp->stats_alloc_fail++; return NULL; }
    uint32_t head = lp->head, tail = lp->tail;
    if (head >= tail) {
        if (head + max_len <= lp->size) {
            guard_wait(lp, head, max_len);
            return lp->pool + head;
        }
        // No room at the end: wrap. The bytes [head, size) become padding that stays "live"
        // until the tail passes them (see linepool_commit).
        if ((uint32_t)max_len >= tail) { lp->stats_alloc_fail++; return NULL; }
        lp->live += lp->size - head;
        lp->head = 0;
        guard_wait(lp, 0, max_len);
        return lp->pool;
    }
    if (head + max_len >= tail) { lp->stats_alloc_fail++; return NULL; }
    guard_wait(lp, head, max_len);
    return lp->pool + head;
}

// Finds the oldest live line by ring distance from head (the fallback when the freed line was
// not at the tail, which only happens if a frame was abandoned part-way).
static uint32_t oldest_live_off(const linepool_t *lp) {
    uint32_t best = lp->head, best_d = 0;
    for (uint32_t y = 0; y < lp->lines; y++) {
        const line_ref_t *l = &lp->line[y];
        if (l->len == 0) continue;
        uint32_t d = ring_dist(lp, l->off, lp->head);
        if (d > best_d) { best_d = d; best = l->off; }
    }
    return best;
}

// Start of the live line that follows line y in ring order: old lines y+1.. then new lines 0..y.
static uint32_t next_live_off(const linepool_t *lp, uint16_t y, bool *found) {
    for (uint32_t i = 1; i <= lp->lines; i++) {
        uint32_t k = (y + i) % lp->lines;
        if (lp->line[k].len) { *found = true; return lp->line[k].off; }
    }
    *found = false;
    return lp->head;
}

// Releases the bytes of the line that `old` described, given that `y` now holds the new record.
static void free_old(linepool_t *lp, uint16_t y, line_ref_t old) {
    if (old.len == 0) return;
    if (old.off == lp->tail) {
        uint32_t t = old.off + old.len;
        bool found;
        uint32_t next = next_live_off(lp, y, &found);
        // A next line starting below the freed one means the writer wrapped in between: the rest
        // of the ring after the freed line is padding.
        if (!found || next < t) t = found ? next : lp->head;
        lp->tail = t;
    } else {
        lp->tail = oldest_live_off(lp);
    }
    lp->live = ring_dist(lp, lp->tail, lp->head);
}

void linepool_commit(linepool_t *lp, uint16_t y, const uint8_t *p, uint16_t len) {
    if (y >= lp->lines) return;
    if (len == 0 || p == NULL) { linepool_commit_dup(lp, y); return; }
    uint32_t off = (uint32_t)(p - lp->pool);
    line_ref_t old = lp->line[y];
    volatile line_ref_t *l = (volatile line_ref_t *)&lp->line[y];
    l->off = off;
    l->len = len;
    LP_BARRIER();
    l->flags = 0;
    LP_BARRIER();
    lp->head = off + len;
    lp->live = ring_dist(lp, lp->tail, lp->head);
    lp->stats_bytes_frame += len;
    free_old(lp, y, old);
}

void linepool_commit_dup(linepool_t *lp, uint16_t y) {
    if (y >= lp->lines) return;
    line_ref_t old = lp->line[y];
    volatile line_ref_t *l = (volatile line_ref_t *)&lp->line[y];
    l->flags = LINE_FLAG_DUP_PREV;
    LP_BARRIER();
    l->len = 0;
    l->off = 0;
    LP_BARRIER();
    free_old(lp, y, old);
}

uint16_t rle_encode_line(const uint8_t *px, uint16_t width, uint8_t *out, uint16_t max) {
    uint32_t n = 0;
    uint32_t i = 0;
    while (i < width) {
        uint8_t v = px[i];
        uint32_t run = 1;
        while (run < 256 && i + run < width && px[i + run] == v) run++;
        if (n + 2 > max) return 0;
        out[n++] = v;
        out[n++] = (uint8_t)(run - 1);
        i += run;
    }
    return (uint16_t)n;
}

void rle_decode_line(const uint8_t *spans, uint16_t len, uint8_t *px, uint16_t width) {
    uint32_t x = 0;
    for (uint32_t i = 0; i + 1 < len && x < width; i += 2) {
        uint32_t run = (uint32_t)spans[i + 1] + 1;
        if (run > width - x) run = width - x;
        memset(px + x, spans[i], run);
        x += run;
    }
    if (x < width) memset(px + x, 0, width - x);   // short input: deterministic output
}
