// geom_flash.c on the host: the flash_store.h primitives are emulated over a 2 MB RAM image that
// enforces erase-before-program (bits only go 1→0) and counts erases. Build with
//   -DSTANDALONE_TEST -Isrc/common -Isrc/pico host/tests/test_geom_flash.c src/common/geom_flash.c src/common/geom_common.c
// and do NOT link geom_ram.c (both define the geom_store_* backend functions).
#ifndef STANDALONE_TEST
#include <stdio.h>
int main(void) { printf("test_geom_flash: skipped (needs -DSTANDALONE_TEST, geom_flash.c, no geom_ram.c)\n"); return 0; }
#else
#include "geom.h"
#include "geom_internal.h"
#include "flash_store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_MSG(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---- fake flash -----------------------------------------------------------------------------
#define FLASH_BYTES (2u * 1024 * 1024)
#define PAGE 256u
#define SECTOR 4096u
static uint8_t flash_img[FLASH_BYTES];
static bool page_erased[FLASH_BYTES / PAGE];
static int erase_calls, program_calls, violations;
static uint32_t erased_bytes, programmed_bytes;

static void flash_reset_chip(void) {
    memset(flash_img, 0xFF, sizeof flash_img);
    memset(page_erased, 1, sizeof page_erased);
    erase_calls = program_calls = violations = 0;
    erased_bytes = programmed_bytes = 0;
}

void flash_store_erase(uint32_t offset, size_t len) {
    erase_calls++;
    if (offset % SECTOR || len % SECTOR || len == 0 || offset > FLASH_BYTES || FLASH_BYTES - offset < len) { violations++; printf("  bad erase %x+%zx\n", offset, len); return; }
    memset(flash_img + offset, 0xFF, len);
    for (uint32_t p = offset / PAGE; p < (offset + len) / PAGE; p++) page_erased[p] = true;
    erased_bytes += (uint32_t)len;
}

void flash_store_program(uint32_t offset, const uint8_t *data, size_t len) {
    program_calls++;
    if (offset % PAGE || len % PAGE || len == 0 || offset > FLASH_BYTES || FLASH_BYTES - offset < len) { violations++; printf("  bad program %x+%zx\n", offset, len); return; }
    for (uint32_t p = offset / PAGE; p < (offset + len) / PAGE; p++) {
        if (!page_erased[p]) { violations++; printf("  program into unerased page %x\n", p * PAGE); }
        page_erased[p] = false;
    }
    for (size_t i = 0; i < len; i++) flash_img[offset + i] &= data[i];   // bits only clear
    programmed_bytes += (uint32_t)len;
}

const uint8_t *geom_flash_test_ptr(uint32_t offset) { return flash_img + offset; }
void geom_flash_test_reboot(void);

uint32_t crc32_update(uint32_t crc, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

// ---- helpers -----------------------------------------------------------------------------------
#define REGION KIOSK_GEOM_FLASH_OFFSET
#define REGION_SIZE KIOSK_GEOM_FLASH_SIZE
static uint8_t scratch[GEOM_SCRATCH_MIN] __attribute__((aligned(8)));

static const geom_flash_hdr_t *hdr(void) { return (const geom_flash_hdr_t *)(const void *)(flash_img + REGION); }

static bool is_circle(uint16_t id) { return id % 5 == 0; }
static int ncontours(uint16_t id) { return 1 + id % 3; }
static int nverts(uint16_t id, int c) { return 20 + (id + c) % 30; }
static void vert(uint16_t id, int c, int k, int32_t *x, int32_t *y) { *x = id * 5 + c * 40 + k * 3 - 700; *y = id * 2 - c * 30 + k * 5 - 300; }

static void write_def(geom_store_t *g, uint16_t id) {
    if (is_circle(id)) { CHECK(geom_store_add_circle(g, id, id * 8, id * 4, id + 1)); return; }
    CHECK(geom_store_begin_poly(g, id));
    for (int c = 0; c < ncontours(id); c++)
        for (int k = 0; k < nverts(id, c); k++) { int32_t x, y; vert(id, c, k, &x, &y); CHECK(geom_store_add_vertex(g, x, y, k == 0)); }
    CHECK(geom_store_end_poly(g));
}

static void check_def(const geom_store_t *g, uint16_t id) {
    const geom_rec_t *r = geom_store_get(g, id);
    CHECK_MSG(r != NULL, "id %u", id);
    if (!r) return;
    const uint8_t *p = (const uint8_t *)r;
    CHECK(p >= flash_img + REGION + GEOM_FLASH_HDR_BYTES && p < flash_img + REGION + REGION_SIZE);   // reads are in place
    CHECK(r->id == id);
    if (is_circle(id)) { CHECK(r->kind == GEOM_CIRCLE && geom_rec_circle(r)[0] == id * 8 && geom_rec_circle(r)[2] == id + 1); return; }
    const int16_t *v = geom_rec_verts(r);
    uint32_t i = 0;
    for (int c = 0; c < ncontours(id); c++) {
        CHECK(v[i] == GEOM_BREAK && v[i + 1] == 0); i += 2;
        for (int k = 0; k < nverts(id, c); k++) { int32_t x, y; vert(id, c, k, &x, &y); CHECK_MSG(v[i] == x && v[i + 1] == y, "id %u %d/%d", id, c, k); i += 2; }
    }
    CHECK(i == r->count * 2);
}

static uint32_t blocks_for(uint32_t data_end) {   // expected erase calls: header + 64 KB blocks touched by [8192, data_end)
    uint32_t n = 1, upto = GEOM_FLASH_HDR_BYTES;
    while (upto < data_end) { n++; upto = (upto / GEOM_FLASH_BLOCK + 1) * GEOM_FLASH_BLOCK; }
    return n;
}

// ---- tests -----------------------------------------------------------------------------------

static void test_blank_and_write(void) {
    flash_reset_chip();
    geom_flash_test_reboot();
    geom_store_t *g = geom_flash_get();
    CHECK(g == geom_flash_get());
    CHECK(!geom_store_open(g, "board-1"));
    CHECK(!geom_store_is_open(g) && geom_store_get(g, 0) == NULL && strcmp(geom_store_id(g), "") == 0);

    CHECK(geom_store_begin(g, "board-1", scratch, sizeof scratch));
    CHECK(erase_calls == 1 && erased_bytes == GEOM_FLASH_HDR_BYTES);   // header sectors only, so far
    CHECK(!geom_store_open(g, "board-1"));                              // nothing valid while writing
    for (uint16_t id = 0; id < 600; id++) write_def(g, id);
    CHECK(geom_store_dropped(g) == 0);
    uint32_t used = (uint32_t)geom_store_bytes_used(g);
    CHECK(used > GEOM_FLASH_HDR_BYTES + 100000);   // > 64 KB of data: several blocks
    CHECK(program_calls > 0 && programmed_bytes < used);   // the last partial page is still buffered
    CHECK(geom_store_commit(g));
    CHECK(violations == 0);
    CHECK_MSG(erase_calls == (int)blocks_for(used), "erases %d expected %u (used %u)", erase_calls, blocks_for(used), used);
    CHECK(geom_store_is_open(g) && geom_store_count(g) == 600 && strcmp(geom_store_id(g), "board-1") == 0);
    // Header on "disk".
    const geom_flash_hdr_t *h = hdr();
    CHECK(h->magic == 0x4F45474Bu && memcmp(&h->magic, "KGEO", 4) == 0 && h->version == 4);
    CHECK(strcmp(h->static_id, "board-1") == 0 && h->ndefs == 600);
    CHECK(h->data_len == used - GEOM_FLASH_HDR_BYTES);
    CHECK(h->data_crc32 == crc32_update(0, flash_img + REGION + GEOM_FLASH_HDR_BYTES, h->data_len));
    // index[0] names the first record's trailer, which follows its data.
    CHECK(h->index[0] >= GEOM_FLASH_HDR_BYTES && h->index[0] < used && h->index[600] == 0xFFFFFFFFu && h->index[KIOSK_MAX_DEFS - 1] == 0xFFFFFFFFu);
    // Padding after the data is 0xFF up to the page end; nothing beyond was touched.
    uint32_t page_end = (used + PAGE - 1) / PAGE * PAGE;
    for (uint32_t i = used; i < page_end; i++) CHECK(flash_img[REGION + i] == 0xFF);
    CHECK(programmed_bytes == page_end - GEOM_FLASH_HDR_BYTES + 6400);   // data pages + 25 header pages
    for (uint32_t i = 0; i < 600; i++) check_def(g, (uint16_t)i);
    CHECK(geom_store_get(g, 600) == NULL);
    // Only the blocks we wrote were erased: the block after the data is untouched (still "fresh").
    CHECK(erased_bytes == GEOM_FLASH_HDR_BYTES + ((used - 1) / GEOM_FLASH_BLOCK + 1) * GEOM_FLASH_BLOCK - GEOM_FLASH_HDR_BYTES);
}

static void test_reboot_open(void) {
    geom_flash_test_reboot();
    geom_store_t *g = geom_flash_get();
    CHECK(!geom_store_is_open(g));
    CHECK(!geom_store_open(g, "board-2"));
    CHECK(strcmp(geom_store_id(g), "board-1") == 0);   // header was read
    CHECK(geom_store_open(g, "board-1"));
    CHECK(geom_store_open(g, "board-1"));               // cached
    CHECK(geom_store_count(g) == 600);
    for (uint32_t i = 0; i < 600; i += 7) check_def(g, (uint16_t)i);
    CHECK(!geom_store_open(g, "board-10"));
    CHECK(!geom_store_is_open(g));
    CHECK(geom_store_open(g, "board-1"));
    // Records are device pixels: a set decoded for one resolution must not open at another.
    geom_store_set_resolution(g, 1920, 1080);
    CHECK(!geom_store_open(g, "board-1"));
    geom_store_set_resolution(g, 0, 0);
    CHECK(geom_store_open(g, "board-1"));
    CHECK(geom_store_bytes_used(g) == GEOM_FLASH_HDR_BYTES + hdr()->data_len);
}

static void test_corruption(void) {
    // A flipped data bit fails the boot-time CRC.
    uint32_t off = REGION + GEOM_FLASH_HDR_BYTES + 5000;
    uint8_t save = flash_img[off];
    flash_img[off] ^= 0x10;
    geom_flash_test_reboot();
    CHECK(!geom_store_open(geom_flash_get(), "board-1"));
    flash_img[off] = save;
    geom_flash_test_reboot();
    CHECK(geom_store_open(geom_flash_get(), "board-1"));
    // Wrong version / magic / data_len out of range / unterminated id.
    geom_flash_hdr_t *h = (geom_flash_hdr_t *)(void *)(flash_img + REGION);
    uint32_t v = h->version; h->version = 3; geom_flash_test_reboot(); CHECK(!geom_store_open(geom_flash_get(), "board-1")); h->version = v;
    uint32_t m = h->magic; h->magic = 0; geom_flash_test_reboot(); CHECK(!geom_store_open(geom_flash_get(), "board-1")); h->magic = m;
    uint32_t dl = h->data_len; h->data_len = REGION_SIZE; geom_flash_test_reboot(); CHECK(!geom_store_open(geom_flash_get(), "board-1")); h->data_len = dl;
    uint32_t nd = h->ndefs; h->ndefs = KIOSK_MAX_DEFS + 1; geom_flash_test_reboot(); CHECK(!geom_store_open(geom_flash_get(), "board-1")); h->ndefs = nd;
    char idsave[KIOSK_MAX_STATIC_ID]; memcpy(idsave, h->static_id, sizeof idsave);
    memset(h->static_id, 'a', sizeof h->static_id); geom_flash_test_reboot(); CHECK(!geom_store_open(geom_flash_get(), "board-1"));
    memcpy(h->static_id, idsave, sizeof idsave);
    // A corrupt index entry (past the data, misaligned, or naming the wrong record) yields NULL, not a wild read.
    geom_flash_test_reboot();
    geom_store_t *g = geom_flash_get();
    CHECK(geom_store_open(g, "board-1"));
    uint32_t i3 = h->index[3];
    h->index[3] = GEOM_FLASH_HDR_BYTES + h->data_len - 8; CHECK(geom_store_get(g, 3) == NULL);
    h->index[3] = REGION_SIZE; CHECK(geom_store_get(g, 3) == NULL);
    h->index[3] = 0xFFFFFFF0u; CHECK(geom_store_get(g, 3) == NULL);
    h->index[3] = i3 + 2; CHECK(geom_store_get(g, 3) == NULL);
    h->index[3] = h->index[4]; CHECK(geom_store_get(g, 3) == NULL);   // record says id 4
    h->index[3] = i3; check_def(g, 3);
}

static void test_abort_and_crash(void) {
    geom_flash_test_reboot();
    geom_store_t *g = geom_flash_get();
    CHECK(geom_store_open(g, "board-1"));
    // begin erases the header: the old set is gone even if we abort.
    CHECK(geom_store_begin(g, "board-2", scratch, sizeof scratch));
    CHECK(!geom_store_is_open(g));
    write_def(g, 1);
    geom_store_abort(g);
    CHECK(!geom_store_open(g, "board-1"));
    CHECK(!geom_store_open(g, "board-2"));
    geom_flash_test_reboot();
    CHECK(!geom_store_open(geom_flash_get(), "board-1"));
    CHECK(hdr()->magic == 0xFFFFFFFFu);
    // Crash mid-write (reboot without commit): still nothing.
    g = geom_flash_get();
    CHECK(geom_store_begin(g, "board-3", scratch, sizeof scratch));
    for (uint16_t id = 0; id < 50; id++) write_def(g, id);
    geom_flash_test_reboot();
    CHECK(!geom_store_open(geom_flash_get(), "board-3"));
    // Commit without an id fails and leaves no set.
    g = geom_flash_get();
    CHECK(geom_store_begin(g, NULL, scratch, sizeof scratch));
    write_def(g, 2);
    CHECK(!geom_store_commit(g));
    CHECK(!geom_store_open(g, ""));
    geom_flash_test_reboot();
    CHECK(!geom_store_open(geom_flash_get(), ""));
    // set_id after begin, then a proper commit; open after reboot.
    g = geom_flash_get();
    CHECK(geom_store_begin(g, NULL, scratch, sizeof scratch));
    write_def(g, 2);
    write_def(g, 2);   // duplicate id: replaced, counted once
    geom_store_set_id(g, "board-4");
    CHECK(geom_store_commit(g));
    CHECK(geom_store_count(g) == 1);
    geom_flash_test_reboot();
    g = geom_flash_get();
    CHECK(geom_store_open(g, "board-4") && geom_store_count(g) == 1);
    check_def(g, 2);
    CHECK(geom_store_get(g, 1) == NULL);
    CHECK(violations == 0);
}

static void test_region_full(void) {
    geom_flash_test_reboot();
    flash_reset_chip();
    geom_store_t *g = geom_flash_get();
    CHECK(geom_store_begin(g, "huge", scratch, sizeof scratch));
    const uint32_t rec_bytes = 12288;   // one marker + max_pairs-1 vertices + the 16-byte trailer
    uint32_t max_pairs = (rec_bytes - sizeof(geom_rec_t)) / 4;
    int stored = 0;
    for (uint16_t id = 0; id < 200; id++) {
        CHECK(geom_store_begin_poly(g, id));
        for (uint32_t i = 0; i < max_pairs - 1; i++) geom_store_add_vertex(g, (int32_t)(i & 4095), id, i == 0);   // refused once the region is full
        if (geom_store_end_poly(g)) stored++;
    }
    uint32_t data_max = REGION_SIZE - GEOM_FLASH_HDR_BYTES;
    CHECK_MSG(stored == (int)(data_max / rec_bytes), "stored %d", stored);
    CHECK(geom_store_dropped(g) == 200 - (uint32_t)stored);
    // A circle record is 28 bytes; it fits only if the max-size records left a tail. (A dropped
    // record's partial bytes stay in the region, so once one record fails nothing else fits.)
    uint32_t tail = data_max - (uint32_t)stored * rec_bytes;
    bool tail_fits = tail >= 28 && geom_store_dropped(g) == 0;
    CHECK(geom_store_add_circle(g, 300, 1, 1, 1) == tail_fits);
    CHECK(geom_store_commit(g));
    CHECK(violations == 0);
    CHECK(geom_store_bytes_used(g) <= REGION_SIZE);
    CHECK(erase_calls == (int)blocks_for((uint32_t)geom_store_bytes_used(g)));
    for (uint32_t i = REGION + REGION_SIZE; i < REGION + REGION_SIZE + 4096; i++) CHECK(flash_img[i] == 0xFF);   // config sector untouched
    geom_flash_test_reboot();
    g = geom_flash_get();
    CHECK(geom_store_open(g, "huge"));
    CHECK(geom_store_count(g) == (uint16_t)stored + (tail_fits ? 1 : 0));
    for (uint16_t id = 0; id < 200; id++) {
        const geom_rec_t *r = geom_store_get(g, id);
        CHECK((r != NULL) == (id < stored));
        if (r) CHECK(r->count == max_pairs && geom_rec_verts(r)[3] == id && geom_rec_verts(r)[(max_pairs - 1) * 2] == (int16_t)((max_pairs - 2) & 4095));
    }
    CHECK((geom_store_get(g, 300) != NULL) == tail_fits);
}

// A group record survives the flash round trip and a reboot like any other.
static void test_group_flash(void) {
    flash_reset_chip();
    geom_flash_test_reboot();
    geom_store_t *g = geom_flash_get();
    const uint16_t m[6] = { 7, 0, 9, 2, 8, 1 };
    CHECK(geom_store_begin(g, "groups", scratch, sizeof scratch));
    CHECK(geom_store_add_group(g, 3, m, 3));
    CHECK(geom_store_add_circle(g, 7, 80, 80, 40));
    CHECK(geom_store_commit(g));
    geom_flash_test_reboot();
    g = geom_flash_get();
    CHECK(geom_store_open(g, "groups") && geom_store_count(g) == 2);
    const geom_rec_t *r = geom_store_get(g, 3);
    CHECK(r && r->kind == GEOM_GROUP && r->count == 3 && memcmp(geom_rec_members(r), m, sizeof m) == 0);
}

int main(void) {
    test_blank_and_write();
    test_reboot_open();
    test_corruption();
    test_abort_and_crash();
    test_region_full();
    test_group_flash();
    CHECK(violations == 0);
    if (failures) { printf("test_geom_flash: %d failure(s)\n", failures); return 1; }
    printf("test_geom_flash: all passed\n");
    return 0;
}
#endif
