// Backend-independent half of the geometry store: streaming record building, bounding boxes, the
// index, and bounds-checked reads. See geom_internal.h for the split and the trailer layout.
#include "geom_internal.h"
#include <string.h>

void geom_copy_id(char dst[KIOSK_MAX_STATIC_ID], const char *src) {
    size_t n = 0;
    if (src) while (n < KIOSK_MAX_STATIC_ID - 1 && src[n]) { dst[n] = src[n]; n++; }
    dst[n] = 0;
}

bool geom_common_begin(geom_store_t *g, const char *static_id, uint8_t *scratch, size_t scratch_len, uint32_t first_off) {
    if (!scratch || scratch_len < GEOM_SCRATCH_MIN) return false;
    if (((uintptr_t)scratch & 3u) != 0) return false;   // the index is addressed as u32
    g->wr_index = (uint32_t *)(void *)scratch;
    for (uint32_t i = 0; i < KIOSK_MAX_DEFS; i++) g->wr_index[i] = GEOM_ABSENT;
    g->wbuf = scratch + KIOSK_MAX_DEFS * 4;
    g->wbuf_len = 0;
    g->rec_active = false;
    g->rec_failed = false;
    g->wr_off = first_off;
    g->wr_ndefs = 0;
    g->stats_dropped = 0;
    geom_copy_id(g->wr_id, static_id);
    g->writing = true;
    return true;
}

void geom_common_end_write(geom_store_t *g) {
    g->writing = false;
    g->rec_active = false;
    g->wbuf_len = 0;
    g->wr_index = NULL;
    g->wbuf = NULL;
}

void geom_store_set_id(geom_store_t *g, const char *static_id) {
    if (g->writing) geom_copy_id(g->wr_id, static_id);
}

bool geom_store_is_open(const geom_store_t *g) { return g->open; }
const char *geom_store_id(const geom_store_t *g) { return g->present ? g->id : ""; }
uint16_t geom_store_count(const geom_store_t *g) { return g->open ? g->ndefs : 0; }
uint32_t geom_store_dropped(const geom_store_t *g) { return g->stats_dropped; }

size_t geom_store_bytes_used(const geom_store_t *g) {
    if (g->writing) return g->wr_off + g->wbuf_len;
    return g->present ? g->rd_data_end : 0;
}

// ---- reading --------------------------------------------------------------------------------

const geom_rec_t *geom_store_get(const geom_store_t *g, uint16_t id) {
    if (!g->open || id >= KIOSK_MAX_DEFS || !g->rd_index) return NULL;
    uint32_t off = g->rd_index[id];
    if (off == GEOM_ABSENT || (off & 3u) != 0) return NULL;
    // Everything below defends against a corrupt index/record (flash bit rot, a foreign header):
    // the renderer must never read outside the set. The trailer sits at `off`; its data precedes it.
    if (off < g->rd_data_begin || off > g->rd_data_end || g->rd_data_end - off < sizeof(geom_rec_t)) return NULL;
    const geom_rec_t *r = (const geom_rec_t *)(const void *)(g->rd_base + off);
    uint32_t room = (off - g->rd_data_begin) / 4u;   // int16 pairs available before the trailer
    if (r->kind == GEOM_POLY) { if (r->count > room) return NULL; }
    else if (r->kind == GEOM_CIRCLE) { if (r->count != 3 || room < 3) return NULL; }
    else return NULL;
    if (r->id != id) return NULL;
    return r;
}

// ---- writing --------------------------------------------------------------------------------

static inline int32_t clamp_px8(int32_t v) {
    return v > PX8_MAX ? PX8_MAX : (v < -PX8_MAX ? -PX8_MAX : v);
}
static inline int16_t clamp_px64(int64_t px) {
    return (int16_t)(px > INT16_MAX ? INT16_MAX : (px < INT16_MIN ? INT16_MIN : px));
}

bool geom_common_flush(geom_store_t *g) {
    if (g->wbuf_len == 0) return true;
    if (!g->be->append(g, g->wr_off, g->wbuf, g->wbuf_len)) return false;
    g->wr_off += g->wbuf_len;
    g->wbuf_len = 0;
    return true;
}

// Queues `len` bytes (a multiple of 4) for the backend, flushing the buffer when it fills.
static bool put(geom_store_t *g, const void *data, uint32_t len) {
    const uint8_t *d = data;
    while (len) {
        uint32_t n = GEOM_WRITE_BUF_BYTES - g->wbuf_len;
        if (n > len) n = len;
        memcpy(g->wbuf + g->wbuf_len, d, n);
        g->wbuf_len += n;
        d += n; len -= n;
        if (g->wbuf_len == GEOM_WRITE_BUF_BYTES && !geom_common_flush(g)) { g->wbuf_len = 0; return false; }
    }
    return true;
}

// Writes the trailer and indexes the record. A repeated id replaces the earlier record (its bytes
// are simply left unreferenced).
static bool finish_record(geom_store_t *g, const geom_rec_t *rec) {
    if (!put(g, rec, sizeof *rec) || !geom_common_flush(g)) { g->wbuf_len = 0; g->stats_dropped++; return false; }
    uint32_t trailer_off = g->wr_off - (uint32_t)sizeof *rec;
    if (g->wr_index[rec->id] == GEOM_ABSENT) g->wr_ndefs++;
    g->wr_index[rec->id] = trailer_off;
    return true;
}

bool geom_store_add_circle(geom_store_t *g, uint16_t id, int32_t cx8, int32_t cy8, int32_t r8) {
    if (!g->writing || g->rec_active || id >= KIOSK_MAX_DEFS) return false;
    if (r8 < 0) r8 = -r8;
    geom_rec_t rec;
    memset(&rec, 0, sizeof rec);
    rec.id = id; rec.kind = GEOM_CIRCLE; rec.count = 3;
    // Bbox in whole pixels [floor(min), ceil(max)) — the centre-sampling rule makes ceil() the right
    // exclusive edge. int64 keeps cx8 ± r8 from overflowing for wire values near INT32_MAX; the
    // result is clamped into the int16 pixel range.
    rec.bx0 = clamp_px64(((int64_t)cx8 - r8) >> PX8_SHIFT);
    rec.by0 = clamp_px64(((int64_t)cy8 - r8) >> PX8_SHIFT);
    rec.bx1 = clamp_px64(((int64_t)cx8 + r8 + PX8_ONE - 1) >> PX8_SHIFT);
    rec.by1 = clamp_px64(((int64_t)cy8 + r8 + PX8_ONE - 1) >> PX8_SHIFT);
    int32_t c[3] = { cx8, cy8, r8 };
    if (!put(g, c, sizeof c)) { g->stats_dropped++; return false; }
    return finish_record(g, &rec);
}

bool geom_store_begin_poly(geom_store_t *g, uint16_t id) {
    if (!g->writing || g->rec_active || id >= KIOSK_MAX_DEFS) return false;
    memset(&g->rec, 0, sizeof g->rec);
    g->rec.id = id; g->rec.kind = GEOM_POLY;
    g->rec_data_off = g->wr_off + g->wbuf_len;
    g->rec_active = true;
    g->rec_failed = false;
    return true;
}

static bool put_pair(geom_store_t *g, int16_t x, int16_t y) {
    int16_t p[2] = { x, y };
    if (!put(g, p, 4)) { g->rec_failed = true; return false; }
    return true;
}

bool geom_store_add_vertex(geom_store_t *g, int32_t x8, int32_t y8, bool contour_start) {
    if (!g->rec_active || g->rec_failed) return false;
    // A marker precedes the first vertex of every contour, the first one included, so the reader
    // has one rule. A record's very first vertex gets one even without contour_start: the
    // flattener always sets it, but a caller that does not still yields a well-formed stream.
    if (contour_start || g->rec.count == 0) {
        if (!put_pair(g, GEOM_BREAK, 0)) return false;
        g->rec.count++;
    }
    int32_t x = clamp_px8(x8), y = clamp_px8(y8);
    // Clamping keeps x above GEOM_BREAK (INT16_MIN < -PX8_MAX), so no vertex can masquerade as a
    // marker.
    geom_rec_t *rec = &g->rec;
    if (rec->flags == 0) { rec->bx0 = rec->bx1 = (int16_t)x; rec->by0 = rec->by1 = (int16_t)y; rec->flags = 1; }   // px8 extents for now
    else {
        if (x < rec->bx0) rec->bx0 = (int16_t)x;
        if (x > rec->bx1) rec->bx1 = (int16_t)x;
        if (y < rec->by0) rec->by0 = (int16_t)y;
        if (y > rec->by1) rec->by1 = (int16_t)y;
    }
    if (!put_pair(g, (int16_t)x, (int16_t)y)) return false;
    rec->count++;
    return true;
}

bool geom_store_end_poly(geom_store_t *g) {
    if (!g->rec_active) return false;
    g->rec_active = false;
    if (g->rec_failed) { g->wbuf_len = 0; g->stats_dropped++; return false; }
    geom_rec_t *rec = &g->rec;
    if (rec->flags == 0) { rec->bx0 = rec->by0 = rec->bx1 = rec->by1 = 0; }
    else {
        // px8 extents → whole pixels [floor(min), ceil(max)). Arithmetic shifts floor negatives.
        rec->bx0 = (int16_t)(rec->bx0 >> PX8_SHIFT);
        rec->by0 = (int16_t)(rec->by0 >> PX8_SHIFT);
        rec->bx1 = (int16_t)((rec->bx1 + PX8_ONE - 1) >> PX8_SHIFT);
        rec->by1 = (int16_t)((rec->by1 + PX8_ONE - 1) >> PX8_SHIFT);
    }
    rec->flags = 0;
    return finish_record(g, rec);
}
