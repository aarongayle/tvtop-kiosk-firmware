// Host backend: the set lives in malloc'd memory. Two regions are kept so that a set being
// written never disturbs the committed one — commit swaps them, abort simply forgets the new
// region's contents. This is what lets the host decoder retry a failed static transfer while the
// previous board stays drawable.
#include "geom_internal.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t *rd, *wr;
    size_t cap;
    uint32_t rd_index[KIOSK_MAX_DEFS];
} ram_impl_t;

static bool ram_append(geom_store_t *g, uint32_t off, const uint8_t *data, size_t len) {
    ram_impl_t *im = (ram_impl_t *)g->impl;
    if (off > im->cap || im->cap - off < len) return false;
    memcpy(im->wr + off, data, len);
    return true;
}

static const geom_backend_t ram_backend = { ram_append };

geom_store_t *geom_ram_create(size_t capacity) {
    geom_store_t *g = (geom_store_t *)calloc(1, sizeof *g);
    ram_impl_t *im = (ram_impl_t *)calloc(1, sizeof *im);
    uint8_t *a = (uint8_t *)malloc(capacity ? capacity : 1);
    uint8_t *b = (uint8_t *)malloc(capacity ? capacity : 1);
    if (!g || !im || !a || !b) { free(g); free(im); free(a); free(b); return NULL; }
    im->rd = a; im->wr = b; im->cap = capacity;
    g->be = &ram_backend;
    g->impl = im;
    return g;
}

bool geom_store_begin(geom_store_t *g, const char *static_id, uint8_t *scratch, size_t scratch_len) {
    if (g->writing) return false;
    return geom_common_begin(g, static_id, scratch, scratch_len, 0);
}

bool geom_store_commit(geom_store_t *g) {
    if (!g->writing) return false;
    if (g->wr_id[0] == 0 || g->rec_active || !geom_common_flush(g)) { geom_common_end_write(g); return false; }   // a set without an id can never be opened
    ram_impl_t *im = (ram_impl_t *)g->impl;
    memcpy(im->rd_index, g->wr_index, sizeof im->rd_index);
    uint8_t *t = im->rd; im->rd = im->wr; im->wr = t;
    g->rd_base = im->rd;
    g->rd_index = im->rd_index;
    g->rd_data_begin = 0;
    g->rd_data_end = g->wr_off;
    g->ndefs = g->wr_ndefs;
    memcpy(g->id, g->wr_id, sizeof g->id);
    g->present = true;
    g->open = true;
    geom_common_end_write(g);
    return true;
}

void geom_store_abort(geom_store_t *g) {
    if (g->writing) geom_common_end_write(g);
}

bool geom_store_open(geom_store_t *g, const char *static_id) {
    g->open = g->present && static_id && strncmp(g->id, static_id, KIOSK_MAX_STATIC_ID) == 0
              && strlen(static_id) < KIOSK_MAX_STATIC_ID;
    return g->open;
}
