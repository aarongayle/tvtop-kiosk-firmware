// Private contract between geom_common.c (record building, reading) and the two backends
// (geom_ram.c, geom_flash.c). Not for other modules.
//
// A set is built sequentially: geom_store_begin hands the common code a scratch area that holds the
// id→offset index (KIOSK_MAX_DEFS × u32) followed by a GEOM_WRITE_BUF_BYTES write-combining buffer.
// Record data streams through the backend's `append` hook as it arrives; the 16-byte geom_rec_t
// header is appended LAST (a trailer) and the index points at it, so a record's size need not be
// known up front and a 3,000-vertex coastline costs no staging RAM. Reading goes through
// `rd_base` + `rd_index[id]`, which the backend points at RAM (geom_ram) or at the XIP-mapped
// flash header (geom_flash) when it opens a set.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "geom.h"

#define GEOM_ABSENT 0xFFFFFFFFu

// Flash header (also the on-disk layout the host test checks). 'KGEO' as little-endian bytes.
#define GEOM_HDR_MAGIC 0x4F45474Bu
#define GEOM_HDR_VERSION 4u   // 3: header carries the output resolution the set was decoded for
                              // 4: GROUP records (a set cached by older firmware may have dropped its groups)
#define GEOM_FLASH_HDR_BYTES 8192u
#define GEOM_FLASH_PAGE 256u
#define GEOM_FLASH_BLOCK 65536u
#define GEOM_FLASH_SECTOR 4096u

typedef struct {
    uint32_t magic;
    uint32_t version;
    char static_id[KIOSK_MAX_STATIC_ID];
    uint32_t ndefs;
    uint32_t data_len;                 // bytes of records after the header
    uint32_t data_crc32;               // crc32_update(0, region + GEOM_FLASH_HDR_BYTES, data_len)
    uint32_t out_wh;                   // width << 16 | height the records were scaled to (0 = unknown)
    uint32_t index[KIOSK_MAX_DEFS];    // byte offset of each record's trailer from the region start, GEOM_ABSENT if none
} geom_flash_hdr_t;

typedef struct geom_backend {
    // Appends `len` bytes (a multiple of 4) at region offset `off`, which always equals the
    // backend's own write cursor. All-or-nothing: false = out of space or device error, and
    // nothing was written.
    bool (*append)(geom_store_t *g, uint32_t off, const uint8_t *data, size_t len);
} geom_backend_t;

struct geom_store {
    const geom_backend_t *be;
    void *impl;                       // backend private state

    // ---- committed set (readable) ----
    bool present;                     // a complete set exists
    bool open;                        // geom_store_open matched the set's id and resolution
    char id[KIOSK_MAX_STATIC_ID];
    uint32_t set_wh;                  // resolution the committed set was decoded for
    uint32_t res_wh;                  // resolution the device renders at (geom_store_set_resolution), 0 = any
    uint16_t ndefs;
    const uint8_t *rd_base;           // record trailer at rd_base + rd_index[id]
    const uint32_t *rd_index;
    uint32_t rd_data_begin;           // offset (from rd_base) of the first record byte
    uint32_t rd_data_end;             // offset (from rd_base) one past the last record byte

    // ---- set being written ----
    bool writing;
    char wr_id[KIOSK_MAX_STATIC_ID];
    uint32_t *wr_index;               // scratch[0 .. KIOSK_MAX_DEFS*4)
    uint8_t *wbuf;                    // scratch after the index, GEOM_WRITE_BUF_BYTES
    uint32_t wbuf_len;
    geom_rec_t rec;                   // the polygon being built (bbox in px8 until end_poly)
    uint32_t rec_data_off;            // offset of its first data byte
    bool rec_active;                  // between begin_poly and end_poly
    bool rec_failed;                  // an append was refused: the record will be dropped
    uint32_t wr_off;                  // backend write cursor (region-relative)
    uint16_t wr_ndefs;
    uint32_t stats_dropped;
};

// Shared write-side setup: validates the scratch, lays out the index and buffer, resets counters.
// The backend calls it from geom_store_begin after its own preparation; `first_off` is where the
// first record goes (0 for RAM, GEOM_FLASH_HDR_BYTES for flash).
bool geom_common_begin(geom_store_t *g, const char *static_id, uint8_t *scratch, size_t scratch_len, uint32_t first_off);
// Flushes the write-combining buffer (call before commit). false if the backend refused.
bool geom_common_flush(geom_store_t *g);
// Resets write state after commit/abort (does not touch the committed-set fields).
void geom_common_end_write(geom_store_t *g);
// Copies at most KIOSK_MAX_STATIC_ID-1 chars, always NUL-terminates.
void geom_copy_id(char dst[KIOSK_MAX_STATIC_ID], const char *src);
