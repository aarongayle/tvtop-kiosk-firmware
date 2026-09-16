// Device backend: the set lives in the flash region [KIOSK_GEOM_FLASH_OFFSET, +KIOSK_GEOM_FLASH_SIZE)
// and is read in place through XIP.
//
// Layout: geom_flash_hdr_t in the first GEOM_FLASH_HDR_BYTES (two 4 KB sectors), records from
// GEOM_FLASH_HDR_BYTES on. Writing: the header sectors are erased in geom_store_begin, so from
// that moment until commit there is no valid set on the device (a power cut mid-transfer leaves
// nothing to trust, which is exactly what the kiosk loop expects). Records stream through a
// 256-byte page buffer; each 64 KB block is erased lazily right before the first page in it is
// programmed, so a small set costs a couple of erases rather than a whole-region wipe. The header
// is programmed last, its first page (magic) last of all.
//
// Only flash_store.h is used from the device side; the host test provides those functions over a
// RAM image and defines STANDALONE_TEST to get geom_flash_test_reboot().
#include "geom_internal.h"
#include "flash_store.h"
#include <stddef.h>
#include <string.h>

#define REGION KIOSK_GEOM_FLASH_OFFSET
#define REGION_SIZE KIOSK_GEOM_FLASH_SIZE
#define DATA_MAX (REGION_SIZE - GEOM_FLASH_HDR_BYTES)

#ifdef STANDALONE_TEST
// The host test maps flash offsets onto its RAM image (flash_store_ptr is a fixed XIP address).
const uint8_t *geom_flash_test_ptr(uint32_t offset);
#define FLASH_PTR(off) geom_flash_test_ptr(off)
#else
#define FLASH_PTR(off) flash_store_ptr(off)
#endif

typedef struct {
    uint8_t page[GEOM_FLASH_PAGE];
    uint32_t page_off;        // region offset of the page being filled (page-aligned)
    uint32_t page_fill;       // bytes in page[]
    uint32_t erased_upto;     // region offset: everything below is erased and unwritten-or-ours
    uint32_t crc;             // running crc32 of the data bytes appended so far
    uint32_t data_len;
    uint8_t crc_state;        // 0 = not checked since boot, 1 = verified good, 2 = verified bad
} flash_impl_t;

static geom_store_t store;
static flash_impl_t impl;

static const geom_flash_hdr_t *header(void) {
    return (const geom_flash_hdr_t *)(const void *)FLASH_PTR(REGION);
}

// Erases whole 64 KB blocks (the last one clipped to the region) up to and including the block
// holding `end_off - 1`.
static void ensure_erased(uint32_t end_off) {
    while (impl.erased_upto < end_off) {
        uint32_t block_end = (impl.erased_upto / GEOM_FLASH_BLOCK + 1) * GEOM_FLASH_BLOCK;
        if (block_end > REGION_SIZE) block_end = REGION_SIZE;
        flash_store_erase(REGION + impl.erased_upto, block_end - impl.erased_upto);
        impl.erased_upto = block_end;
    }
}

static bool flush_page(void) {
    if (impl.page_off + GEOM_FLASH_PAGE > REGION_SIZE) return false;
    ensure_erased(impl.page_off + GEOM_FLASH_PAGE);
    flash_store_program(REGION + impl.page_off, impl.page, GEOM_FLASH_PAGE);
    impl.page_off += GEOM_FLASH_PAGE;
    impl.page_fill = 0;
    return true;
}

static bool flash_append(geom_store_t *g, uint32_t off, const uint8_t *data, size_t len) {
    (void)g;
    if (off != impl.page_off + impl.page_fill) return false;             // cursor desync: refuse
    if (len > DATA_MAX || off - GEOM_FLASH_HDR_BYTES > DATA_MAX - len) return false;   // whole record must fit
    while (len) {
        size_t n = GEOM_FLASH_PAGE - impl.page_fill;
        if (n > len) n = len;
        memcpy(impl.page + impl.page_fill, data, n);
        impl.page_fill += (uint32_t)n;
        impl.crc = crc32_update(impl.crc, data, n);
        impl.data_len += (uint32_t)n;
        data += n; len -= n;
        if (impl.page_fill == GEOM_FLASH_PAGE && !flush_page()) return false;
    }
    return true;
}

static const geom_backend_t flash_backend = { flash_append };

geom_store_t *geom_flash_get(void) {
    if (!store.be) { store.be = &flash_backend; store.impl = &impl; }
    return &store;
}

bool geom_store_begin(geom_store_t *g, const char *static_id, uint8_t *scratch, size_t scratch_len) {
    if (g->writing) return false;
    if (!geom_common_begin(g, static_id, scratch, scratch_len, GEOM_FLASH_HDR_BYTES)) return false;
    // The old set is gone from here on; make that true on the device before anything else.
    g->present = false; g->open = false; g->rd_index = NULL;
    flash_store_erase(REGION, GEOM_FLASH_HDR_BYTES);
    impl.page_off = GEOM_FLASH_HDR_BYTES;
    impl.page_fill = 0;
    impl.erased_upto = GEOM_FLASH_HDR_BYTES;
    impl.crc = 0;
    impl.data_len = 0;
    return true;
}

void geom_store_abort(geom_store_t *g) {
    if (g->writing) geom_common_end_write(g);
}

// Fills page `p` (256 bytes) of the header image: the fixed fields, then slices of the index.
static void header_page(const geom_store_t *g, uint32_t p, uint8_t *out) {
    // Only the fixed fields are assembled here (the index is copied straight from the scratch),
    // so this needs a few dozen bytes of stack rather than a whole 6 KB header.
    struct { uint32_t magic, version; char static_id[KIOSK_MAX_STATIC_ID]; uint32_t ndefs, data_len, data_crc32, out_wh; } fixed;
    _Static_assert(sizeof fixed == offsetof(geom_flash_hdr_t, index), "fixed header fields must match geom_flash_hdr_t");
    memset(&fixed, 0, sizeof fixed);
    fixed.magic = GEOM_HDR_MAGIC;
    fixed.version = GEOM_HDR_VERSION;
    memcpy(fixed.static_id, g->wr_id, sizeof fixed.static_id);
    fixed.ndefs = g->wr_ndefs;
    fixed.data_len = impl.data_len;
    fixed.data_crc32 = impl.crc;
    fixed.out_wh = g->res_wh;
    const size_t index_off = offsetof(geom_flash_hdr_t, index);
    memset(out, 0xFF, GEOM_FLASH_PAGE);
    for (uint32_t i = 0; i < GEOM_FLASH_PAGE; i++) {
        uint32_t o = p * GEOM_FLASH_PAGE + i;
        if (o < index_off) out[i] = ((const uint8_t *)&fixed)[o];
        else if (o < sizeof(geom_flash_hdr_t)) out[i] = ((const uint8_t *)g->wr_index)[o - index_off];
    }
}

bool geom_store_commit(geom_store_t *g) {
    if (!g->writing) return false;
    if (g->wr_id[0] == 0 || g->rec_active || !geom_common_flush(g)) { geom_common_end_write(g); return false; }
    if (impl.page_fill) {
        memset(impl.page + impl.page_fill, 0xFF, GEOM_FLASH_PAGE - impl.page_fill);
        if (!flush_page()) { geom_common_end_write(g); return false; }
    }
    // The header is programmed page by page from the page buffer, tail pages first, so a crash
    // during the write cannot leave a header whose magic is valid but whose index is not.
    _Static_assert(sizeof(geom_flash_hdr_t) <= GEOM_FLASH_HDR_BYTES, "header must fit its sectors");
    uint32_t npages = ((uint32_t)sizeof(geom_flash_hdr_t) + GEOM_FLASH_PAGE - 1) / GEOM_FLASH_PAGE;
    for (uint32_t p = npages; p-- > 0;) {
        header_page(g, p, impl.page);
        flash_store_program(REGION + p * GEOM_FLASH_PAGE, impl.page, GEOM_FLASH_PAGE);
    }

    g->rd_base = FLASH_PTR(REGION);
    g->rd_index = header()->index;
    g->rd_data_begin = GEOM_FLASH_HDR_BYTES;
    g->rd_data_end = GEOM_FLASH_HDR_BYTES + impl.data_len;
    g->ndefs = g->wr_ndefs;
    g->set_wh = g->res_wh;
    memcpy(g->id, g->wr_id, sizeof g->id);
    g->present = true;
    g->open = true;
    impl.crc_state = 1;   // we just wrote it; no need to re-read 900 KB to prove it
    geom_common_end_write(g);
    return true;
}

// Validates the header in flash and, once per boot, the data CRC. Fills the read-side fields.
static bool load_header(geom_store_t *g) {
    const geom_flash_hdr_t *h = header();
    if (h->magic != GEOM_HDR_MAGIC || h->version != GEOM_HDR_VERSION) return false;
    if (h->ndefs > KIOSK_MAX_DEFS || h->data_len > DATA_MAX) return false;
    if (memchr(h->static_id, 0, sizeof h->static_id) == NULL || h->static_id[0] == 0) return false;
    if (impl.crc_state == 0) {
        uint32_t c = crc32_update(0, FLASH_PTR(REGION + GEOM_FLASH_HDR_BYTES), h->data_len);
        impl.crc_state = c == h->data_crc32 ? 1 : 2;
    }
    if (impl.crc_state != 1) return false;
    g->rd_base = FLASH_PTR(REGION);
    g->rd_index = h->index;
    g->rd_data_begin = GEOM_FLASH_HDR_BYTES;
    g->rd_data_end = GEOM_FLASH_HDR_BYTES + h->data_len;
    g->ndefs = (uint16_t)h->ndefs;
    g->set_wh = h->out_wh;
    memcpy(g->id, h->static_id, sizeof g->id);
    g->id[KIOSK_MAX_STATIC_ID - 1] = 0;
    return true;
}

bool geom_store_open(geom_store_t *g, const char *static_id) {
    if (g->writing) { g->open = false; return false; }
    if (!g->present) g->present = load_header(g);
    g->open = g->present && static_id && strncmp(g->id, static_id, KIOSK_MAX_STATIC_ID) == 0
              && strlen(static_id) < KIOSK_MAX_STATIC_ID && (g->res_wh == 0 || g->set_wh == g->res_wh);
    return g->open;
}

#ifdef STANDALONE_TEST
// Forgets everything cached in RAM, as a reset would; the flash image is untouched.
void geom_flash_test_reboot(void) {
    memset(&store, 0, sizeof store);
    memset(&impl, 0, sizeof impl);
}
#endif
