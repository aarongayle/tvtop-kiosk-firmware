// Core-1 video: PicoDVI (libdvi) DMA/PIO plumbing plus our run-length → TMDS encoder.
//
// Everything core 1 executes or touches lives in SRAM so core 0 may erase/program flash at any
// time without stopping video. The palette LUTs are updated from core 0 between frames; a
// half-updated entry is only ever an entry no line on screen uses yet.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "board.h"
#include "linepool.h"
#include "palette.h"

#define DVI_N_TMDS_BUFFERS_OURS 3   // libdvi queue depth; the buffers live in scanout.c (DVI_N_TMDS_BUFFERS=0)

// Sets clocks/voltage for the mode (call FIRST in main, before stdio/wifi), then initialises
// libdvi with KIOSK_DVI_CFG. Returns the active mode info.
const video_mode_info_t *scanout_setup_clocks(video_mode_t m);
void scanout_init(const video_mode_info_t *mode, linepool_t *pool);
// Launches core 1: it registers the DMA IRQs, starts DVI and loops over the line pool forever.
void scanout_start(void);

// Palette → TMDS LUT. Safe from core 0 at any time. Uploads entries [pal->dirty_from, count).
void scanout_upload_palette(palette_t *pal);
void scanout_set_entry(uint8_t idx, uint32_t rgb);

// Stats.
uint32_t scanout_late_lines(void);    // lines the encoder failed to deliver in time (should stay 0)
uint32_t scanout_frames(void);
uint32_t scanout_max_line_cycles(void);   // worst encode time seen, in sysclk cycles (debug builds)

// The encoder itself (also compiled on the host for tests, see host/tests/test_encoder.c):
// spans: (idx, run-1) pairs summing to `width`; out: 3 lane buffers of width/2 words each, laid
// out lane0 | lane1 | lane2 (stride = width/2 words); pair_lut[3][256] = tmds_pack2(sym, sym);
// sym_lut[3][256] = 10-bit symbols. `width` is even.
void tmds_rle_encode_line(const uint8_t *spans, uint32_t nbytes, uint32_t *out, uint32_t width,
                          const uint32_t *pair_lut, const uint16_t *sym_lut);
