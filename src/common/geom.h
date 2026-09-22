// Static geometry store: the decoded, device-space polygons and circles of one static set,
// addressed by the def id (base-36 ordinal from the wire, 0..KIOSK_MAX_DEFS-1).
//
// Two backends with the same interface: geom_ram.c (host, malloc) and geom_flash.c (device,
// flash region KIOSK_GEOM_FLASH_OFFSET). Writing is sequential; each record is staged whole in the
// scratch area handed to geom_store_begin, then appended. The header/index is written at commit,
// so an interrupted write leaves no valid set.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "kiosk_config.h"

enum { GEOM_POLY = 1, GEOM_CIRCLE = 2, GEOM_GROUP = 3 };
// GROUP records: `count` uint16 pairs {member def id, paint index}, drawn in order. Members are
// resolved when drawn (they may be stored before or after the group), and a member that is absent,
// is itself a group, or names a paint the frame lacks is skipped. The bbox of a group record is
// unused (all zero): the renderer takes the union of its members'.
#define GEOM_GROUP_MAX 32
// Vertex streams (POLY records): int16 px8 pairs. A marker pair {GEOM_BREAK, 0} precedes the first
// vertex of every contour, the first contour included, so a stored stream always begins with a
// marker; readers must tolerate a stream that starts without one (treat the first vertex as a
// contour start) and must treat x == GEOM_BREAK purely as "the next pair starts a contour" (the y
// of a marker is 0 and carries nothing). Real vertices are clamped to ±PX8_MAX, so no vertex x
// can equal GEOM_BREAK. `count` includes the marker pairs.
#define GEOM_BREAK INT16_MIN

typedef struct {
    uint16_t id;
    uint8_t kind;
    uint8_t flags;
    int16_t bx0, by0, bx1, by1;   // bounding box, whole pixels, [bx0,bx1) × [by0,by1)
    uint32_t count;               // POLY: number of int16 pairs PRECEDING this header (incl. GEOM_BREAK markers); CIRCLE: 3; GROUP: members
} geom_rec_t;                      // a trailer: the data (int16 pairs px8 / int32 cx8, cy8, r8, 4-byte aligned) comes first,
                                   // so a record of any size can stream into the store before its size is known

typedef struct geom_store geom_store_t;

// Backend construction. Host: geom_ram_create(); device: geom_flash_get() (singleton).
geom_store_t *geom_ram_create(size_t capacity);
geom_store_t *geom_flash_get(void);

// ---- reading ----
// Records are stored in device pixels, so a set only fits the resolution it was decoded at. Set the
// output size before opening or writing; a set decoded for another size refuses to open, and the
// kiosk fetches it again. 0 x 0 (the host tools) accepts any set.
void geom_store_set_resolution(geom_store_t *g, uint16_t w, uint16_t h);
bool geom_store_open(geom_store_t *g, const char *static_id);   // true if a complete set with this id, for this resolution, is present
bool geom_store_is_open(const geom_store_t *g);
const char *geom_store_id(const geom_store_t *g);
const geom_rec_t *geom_store_get(const geom_store_t *g, uint16_t id);   // NULL if absent
static inline const int16_t *geom_rec_verts(const geom_rec_t *r) { return (const int16_t *)((const uint8_t *)r - (size_t)r->count * 4); }
static inline const int32_t *geom_rec_circle(const geom_rec_t *r) { return (const int32_t *)((const uint8_t *)r - 12); }
static inline const uint16_t *geom_rec_members(const geom_rec_t *r) { return (const uint16_t *)((const uint8_t *)r - (size_t)r->count * 4); }
uint16_t geom_store_count(const geom_store_t *g);

// ---- writing ----  (scratch must be at least GEOM_SCRATCH_MIN bytes and stays owned until commit/abort:
// it holds the id→offset index being built plus a small write-combining buffer; vertices stream
// straight into the store, so a record is limited only by the region size)
#define GEOM_WRITE_BUF_BYTES 256
#define GEOM_SCRATCH_MIN (KIOSK_MAX_DEFS * 4 + GEOM_WRITE_BUF_BYTES)
// static_id may be NULL here and supplied later with geom_store_set_id (before commit).
bool geom_store_begin(geom_store_t *g, const char *static_id, uint8_t *scratch, size_t scratch_len);
void geom_store_set_id(geom_store_t *g, const char *static_id);
bool geom_store_add_circle(geom_store_t *g, uint16_t id, int32_t cx8, int32_t cy8, int32_t r8);
// `members` is n {id, paint} pairs; n is at most GEOM_GROUP_MAX.
bool geom_store_add_group(geom_store_t *g, uint16_t id, const uint16_t *members, uint32_t n);
bool geom_store_begin_poly(geom_store_t *g, uint16_t id);
bool geom_store_add_vertex(geom_store_t *g, int32_t x8, int32_t y8, bool contour_start);   // clamps to ±PX8_MAX
bool geom_store_end_poly(geom_store_t *g);        // false if the record did not fit (record dropped, store still usable)
bool geom_store_commit(geom_store_t *g);          // finalises; the set becomes open for reading
void geom_store_abort(geom_store_t *g);
size_t geom_store_bytes_used(const geom_store_t *g);   // record bytes written (this set) — flash: from the region start
// Records dropped since geom_store_begin because they did not fit (staging or region). A set that
// dropped anything is incomplete; the decoder should report FD_ERR_STATIC_STORE.
uint32_t geom_store_dropped(const geom_store_t *g);
