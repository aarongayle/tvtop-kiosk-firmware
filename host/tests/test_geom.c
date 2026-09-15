// geom (RAM backend) tests: 1500 mixed defs round trip, bboxes, clamping, oversize records,
// out-of-range ids, abort/commit/open semantics, capacity exhaustion, misuse.
#include "geom.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_MSG(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint8_t *scratch_alloc(void) {
    uint8_t *s = malloc(GEOM_SCRATCH_MIN);
    memset(s, 0xAA, GEOM_SCRATCH_MIN);
    return s;
}

// Deterministic test geometry per id.
static bool is_circle(uint16_t id) { return id % 3 == 0; }
static int ncontours(uint16_t id) { return 1 + id % 4; }
static int nverts(uint16_t id, int c) { return 3 + (id + c) % 5; }
static void vert(uint16_t id, int c, int k, int32_t *x, int32_t *y) {
    *x = (int32_t)id * 7 + c * 100 + k * 13 - 900;
    *y = (int32_t)id * 3 - c * 50 + k * 17 - 400;
}

static void write_def(geom_store_t *g, uint16_t id) {
    if (is_circle(id)) { CHECK(geom_store_add_circle(g, id, id * 8, -(int32_t)id * 4, id + 1)); return; }
    CHECK(geom_store_begin_poly(g, id));
    for (int c = 0; c < ncontours(id); c++)
        for (int k = 0; k < nverts(id, c); k++) {
            int32_t x, y; vert(id, c, k, &x, &y);
            CHECK(geom_store_add_vertex(g, x, y, k == 0));
        }
    CHECK(geom_store_end_poly(g));
}

static int32_t floor8(int32_t v) { return v >> 3; }
static int32_t ceil8(int32_t v) { return (v + 7) >> 3; }

static void check_def(const geom_store_t *g, uint16_t id) {
    const geom_rec_t *r = geom_store_get(g, id);
    CHECK_MSG(r != NULL, "id %u", id);
    if (!r) return;
    CHECK(r->id == id);
    CHECK(((uintptr_t)r & 3) == 0);
    if (is_circle(id)) {
        const int32_t *c = geom_rec_circle(r);
        CHECK(r->kind == GEOM_CIRCLE && r->count == 3);
        CHECK(c[0] == id * 8 && c[1] == -(int32_t)id * 4 && c[2] == id + 1);
        CHECK(r->bx0 == floor8(c[0] - c[2]) && r->bx1 == ceil8(c[0] + c[2]));
        CHECK(r->by0 == floor8(c[1] - c[2]) && r->by1 == ceil8(c[1] + c[2]));
        return;
    }
    CHECK(r->kind == GEOM_POLY);
    const int16_t *v = geom_rec_verts(r);
    uint32_t i = 0;
    int32_t minx = INT32_MAX, maxx = INT32_MIN, miny = INT32_MAX, maxy = INT32_MIN;
    for (int c = 0; c < ncontours(id); c++) {
        CHECK_MSG(i + 1 < r->count * 2 && v[i] == GEOM_BREAK && v[i + 1] == 0, "id %u contour %d marker", id, c);
        i += 2;
        for (int k = 0; k < nverts(id, c); k++) {
            int32_t x, y; vert(id, c, k, &x, &y);
            CHECK_MSG(i + 1 < r->count * 2 && v[i] == x && v[i + 1] == y, "id %u vertex %d/%d", id, c, k);
            i += 2;
            if (x < minx) minx = x; if (x > maxx) maxx = x; if (y < miny) miny = y; if (y > maxy) maxy = y;
        }
    }
    CHECK_MSG(i == r->count * 2, "id %u count %u vs %u", id, r->count, i / 2);
    CHECK_MSG(r->bx0 == floor8(minx) && r->bx1 == ceil8(maxx) && r->by0 == floor8(miny) && r->by1 == ceil8(maxy),
              "id %u bbox [%d,%d)x[%d,%d) expected [%d,%d)x[%d,%d)", id, r->bx0, r->bx1, r->by0, r->by1,
              floor8(minx), ceil8(maxx), floor8(miny), ceil8(maxy));
}

static void test_round_trip(void) {
    geom_store_t *g = geom_ram_create(1 << 20);
    uint8_t *scratch = scratch_alloc();
    CHECK(g && !geom_store_is_open(g) && geom_store_get(g, 0) == NULL && geom_store_count(g) == 0);
    CHECK(strcmp(geom_store_id(g), "") == 0);
    CHECK(!geom_store_open(g, "anything"));
    CHECK(geom_store_begin(g, "set-A", scratch, GEOM_SCRATCH_MIN));
    CHECK(!geom_store_begin(g, "set-B", scratch, GEOM_SCRATCH_MIN));   // already writing
    for (uint16_t id = 0; id < 1500; id++) write_def(g, id);
    CHECK(geom_store_dropped(g) == 0);
    CHECK(!geom_store_is_open(g));     // nothing open before the first commit
    CHECK(geom_store_commit(g));
    CHECK(geom_store_is_open(g) && geom_store_count(g) == 1500 && strcmp(geom_store_id(g), "set-A") == 0);
    CHECK(geom_store_bytes_used(g) > 1500 * 16);
    for (uint16_t id = 0; id < 1500; id++) check_def(g, id);
    for (uint16_t id = 1500; id < KIOSK_MAX_DEFS; id++) CHECK(geom_store_get(g, id) == NULL);
    CHECK(geom_store_get(g, KIOSK_MAX_DEFS) == NULL);
    CHECK(geom_store_get(g, 0xFFFF) == NULL);
    // The scratch is free after commit: clobbering it must not affect reads.
    memset(scratch, 0x55, GEOM_SCRATCH_MIN);
    for (uint16_t id = 0; id < 1500; id += 97) check_def(g, id);

    // open semantics
    CHECK(!geom_store_open(g, "set-B"));
    CHECK(!geom_store_is_open(g) && geom_store_get(g, 1) == NULL && geom_store_count(g) == 0);
    CHECK(strcmp(geom_store_id(g), "set-A") == 0);   // still the committed id
    CHECK(!geom_store_open(g, "set-A-longer"));
    CHECK(!geom_store_open(g, "set-"));
    CHECK(!geom_store_open(g, NULL));
    CHECK(geom_store_open(g, "set-A"));
    CHECK(geom_store_is_open(g) && geom_store_count(g) == 1500);
    check_def(g, 7);

    // abort leaves the previous set open and intact
    CHECK(geom_store_begin(g, "set-B", scratch, GEOM_SCRATCH_MIN));
    CHECK(geom_store_add_circle(g, 0, 1, 2, 3));
    CHECK(geom_store_begin_poly(g, 1));
    CHECK(geom_store_add_vertex(g, 5, 5, true));
    geom_store_abort(g);
    geom_store_abort(g);   // idempotent
    CHECK(geom_store_is_open(g) && strcmp(geom_store_id(g), "set-A") == 0 && geom_store_count(g) == 1500);
    for (uint16_t id = 0; id < 1500; id += 61) check_def(g, id);
    CHECK(!geom_store_commit(g));   // nothing being written

    // set_id after begin(NULL); commit without an id fails and keeps the old set
    CHECK(geom_store_begin(g, NULL, scratch, GEOM_SCRATCH_MIN));
    CHECK(geom_store_add_circle(g, 5, 80, 80, 40));
    CHECK(!geom_store_commit(g));
    CHECK(geom_store_is_open(g) && strcmp(geom_store_id(g), "set-A") == 0);
    check_def(g, 5);
    CHECK(geom_store_begin(g, NULL, scratch, GEOM_SCRATCH_MIN));
    CHECK(geom_store_add_circle(g, 5, 80, 80, 40));
    geom_store_set_id(g, "set-C");
    CHECK(geom_store_commit(g));
    CHECK(geom_store_is_open(g) && strcmp(geom_store_id(g), "set-C") == 0 && geom_store_count(g) == 1);
    CHECK(geom_store_get(g, 7) == NULL);
    const geom_rec_t *r = geom_store_get(g, 5);
    CHECK(r && r->kind == GEOM_CIRCLE && geom_rec_circle(r)[2] == 40 && r->bx0 == 5 && r->bx1 == 15);
    CHECK(!geom_store_open(g, "set-A"));
    CHECK(geom_store_open(g, "set-C"));
    // A long id is truncated consistently.
    char longid[64];
    memset(longid, 'x', sizeof longid); longid[63] = 0;
    CHECK(geom_store_begin(g, longid, scratch, GEOM_SCRATCH_MIN));
    CHECK(geom_store_commit(g));
    CHECK(strlen(geom_store_id(g)) == KIOSK_MAX_STATIC_ID - 1);
    CHECK(!geom_store_open(g, longid));   // an id that does not fit can never match
    longid[KIOSK_MAX_STATIC_ID - 1] = 0;
    CHECK(geom_store_open(g, longid));
    free(scratch);
}

static void test_clamp_and_bbox(void) {
    geom_store_t *g = geom_ram_create(1 << 16);
    uint8_t *scratch = scratch_alloc();
    CHECK(geom_store_begin(g, "s", scratch, GEOM_SCRATCH_MIN));
    CHECK(geom_store_begin_poly(g, 0));
    CHECK(geom_store_add_vertex(g, 1000000, -1000000, true));
    CHECK(geom_store_add_vertex(g, INT32_MAX, INT32_MIN, false));
    CHECK(geom_store_add_vertex(g, -9, 9, false));
    CHECK(geom_store_end_poly(g));
    // Negative extents floor, positive ceil: [-2,2)
    CHECK(geom_store_begin_poly(g, 1));
    CHECK(geom_store_add_vertex(g, -9, -9, true));
    CHECK(geom_store_add_vertex(g, 9, 9, false));
    CHECK(geom_store_end_poly(g));
    // Exact pixel edges: vertices at 8 and 16 px8 → [1,2)
    CHECK(geom_store_begin_poly(g, 2));
    CHECK(geom_store_add_vertex(g, 8, 8, true));
    CHECK(geom_store_add_vertex(g, 16, 16, false));
    CHECK(geom_store_end_poly(g));
    // First vertex without contour_start still gets a marker.
    CHECK(geom_store_begin_poly(g, 3));
    CHECK(geom_store_add_vertex(g, 1, 1, false));
    CHECK(geom_store_add_vertex(g, 2, 2, false));
    CHECK(geom_store_end_poly(g));
    // Empty polygon.
    CHECK(geom_store_begin_poly(g, 4));
    CHECK(geom_store_end_poly(g));
    // Circle with a negative radius and a huge centre.
    CHECK(geom_store_add_circle(g, 5, 0, 0, -16));
    CHECK(geom_store_add_circle(g, 6, INT32_MAX, INT32_MIN, INT32_MAX));
    CHECK(geom_store_commit(g));
    const geom_rec_t *r = geom_store_get(g, 0);
    CHECK(r && r->count == 4);
    const int16_t *v = geom_rec_verts(r);
    CHECK(v[0] == GEOM_BREAK && v[1] == 0 && v[2] == PX8_MAX && v[3] == -PX8_MAX && v[4] == PX8_MAX && v[5] == -PX8_MAX && v[6] == -9 && v[7] == 9);
    CHECK(r->bx0 == -2 && r->bx1 == 4095 && r->by0 == -4095 && r->by1 == 2);
    r = geom_store_get(g, 1);
    CHECK(r && r->bx0 == -2 && r->bx1 == 2 && r->by0 == -2 && r->by1 == 2);
    r = geom_store_get(g, 2);
    CHECK(r && r->bx0 == 1 && r->bx1 == 2 && r->by0 == 1 && r->by1 == 2);
    r = geom_store_get(g, 3);
    CHECK(r && r->count == 3 && geom_rec_verts(r)[0] == GEOM_BREAK && geom_rec_verts(r)[2] == 1 && geom_rec_verts(r)[4] == 2);
    r = geom_store_get(g, 4);
    CHECK(r && r->kind == GEOM_POLY && r->count == 0 && r->bx0 == 0 && r->bx1 == 0);
    r = geom_store_get(g, 5);
    CHECK(r && geom_rec_circle(r)[2] == 16 && r->bx0 == -2 && r->bx1 == 2);
    r = geom_store_get(g, 6);
    CHECK(r && r->bx1 == INT16_MAX && r->by0 == INT16_MIN && r->bx0 == 0 && r->by1 == 0);
    CHECK(geom_store_count(g) == 7);
    free(scratch);
}

static void test_oversize_and_misuse(void) {
    geom_store_t *g = geom_ram_create(1 << 20);
    uint8_t *scratch = scratch_alloc();
    // Out of range ids and calls outside a write.
    CHECK(!geom_store_begin_poly(g, 0));
    CHECK(!geom_store_add_vertex(g, 0, 0, true));
    CHECK(!geom_store_end_poly(g));
    CHECK(!geom_store_add_circle(g, 0, 0, 0, 1));
    geom_store_set_id(g, "ignored");
    CHECK(!geom_store_begin(g, "s", scratch, GEOM_SCRATCH_MIN - 1));   // scratch too small
    CHECK(!geom_store_begin(g, "s", NULL, GEOM_SCRATCH_MIN));
    CHECK(!geom_store_begin(g, "s", scratch + 1, GEOM_SCRATCH_MIN));   // misaligned
    CHECK(geom_store_begin(g, "s", scratch, GEOM_SCRATCH_MIN));
    CHECK(!geom_store_begin_poly(g, KIOSK_MAX_DEFS));
    CHECK(!geom_store_add_circle(g, KIOSK_MAX_DEFS, 0, 0, 1));
    CHECK(!geom_store_add_circle(g, 0xFFFF, 0, 0, 1));
    CHECK(!geom_store_add_vertex(g, 0, 0, true));   // no poly open
    CHECK(!geom_store_end_poly(g));
    CHECK(geom_store_begin_poly(g, 10));
    CHECK(!geom_store_begin_poly(g, 11));           // nested
    CHECK(!geom_store_add_circle(g, 12, 0, 0, 1));  // circle while a poly is open
    CHECK(geom_store_add_vertex(g, 1, 1, true));
    CHECK(geom_store_end_poly(g));
    CHECK(geom_store_dropped(g) == 0);
    // Records stream into the store, so size is limited only by the region: a record larger than
    // a tiny store is dropped (its partial bytes are wasted) and the store stays usable.
    {
        geom_store_t *tiny = geom_ram_create(2048);
        uint8_t *ts = scratch_alloc();
        CHECK(geom_store_begin(tiny, "tiny", ts, GEOM_SCRATCH_MIN));
        CHECK(geom_store_begin_poly(tiny, 20));
        bool all_ok = true;
        for (uint32_t i = 0; i < 1000; i++) all_ok = geom_store_add_vertex(tiny, (int32_t)i, (int32_t)i, i == 0) && all_ok;
        CHECK(!all_ok);
        CHECK(!geom_store_add_vertex(tiny, 1, 1, true));   // stays refused after the region filled
        CHECK(!geom_store_end_poly(tiny));
        CHECK(geom_store_dropped(tiny) == 1);
        CHECK(!geom_store_end_poly(tiny));                 // no longer open
        CHECK(geom_store_commit(tiny));
        CHECK(geom_store_count(tiny) == 0 && geom_store_get(tiny, 20) == NULL);
    }
    uint32_t max_pairs = 3072;   // a 12 KB record: bigger than any staging area we used to have
    CHECK(geom_store_begin_poly(g, 21));
    for (uint32_t i = 0; i < max_pairs - 1; i++) CHECK(geom_store_add_vertex(g, (int32_t)(i & 1023), 3, i == 0));
    CHECK(geom_store_end_poly(g));
    CHECK(geom_store_add_circle(g, 22, 1, 2, 3));
    // Duplicate id replaces.
    CHECK(geom_store_add_circle(g, 22, 4, 5, 6));
    CHECK(geom_store_commit(g));
    CHECK(geom_store_count(g) == 3 && geom_store_dropped(g) == 0);   // 10, 21, 22 (replaced)
    CHECK(geom_store_get(g, 20) == NULL);
    const geom_rec_t *r = geom_store_get(g, 21);
    CHECK(r && r->count == max_pairs && geom_rec_verts(r)[(max_pairs - 1) * 2] == (int16_t)((max_pairs - 2) & 1023));
    r = geom_store_get(g, 22);
    CHECK(r && geom_rec_circle(r)[0] == 4 && geom_rec_circle(r)[2] == 6);
    CHECK(geom_store_get(g, 10) != NULL && geom_store_get(g, 11) == NULL && geom_store_get(g, 12) == NULL);

    // Region capacity exhaustion: records that do not fit are dropped, the rest survive commit.
    geom_store_t *small = geom_ram_create(1000);
    CHECK(geom_store_begin(small, "small", scratch, GEOM_SCRATCH_MIN));
    int stored = 0;
    for (uint16_t id = 0; id < 100; id++) if (geom_store_add_circle(small, id, id, id, 1)) stored++;
    CHECK(stored == 1000 / 28);
    CHECK(geom_store_dropped(small) == 100 - stored);
    CHECK(geom_store_begin_poly(small, 200));
    CHECK(geom_store_add_vertex(small, 1, 1, true));
    CHECK(!geom_store_end_poly(small));
    CHECK(geom_store_commit(small));
    CHECK(geom_store_count(small) == (uint16_t)stored && geom_store_bytes_used(small) == (size_t)stored * 28);
    for (uint16_t id = 0; id < 100; id++) CHECK((geom_store_get(small, id) != NULL) == (id < stored));
    CHECK(geom_store_get(small, 200) == NULL);
    free(scratch);
}

int main(void) {
    test_round_trip();
    test_clamp_and_bbox();
    test_oversize_and_misuse();
    if (failures) { printf("test_geom: %d failure(s)\n", failures); return 1; }
    printf("test_geom: all passed\n");
    return 0;
}
