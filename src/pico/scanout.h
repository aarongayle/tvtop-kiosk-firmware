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

// Scanline buffers are carved from one pool sized for three full-width (1280 px) lines. libdvi
// holds two of them at any moment (the line on the wire and the one awaiting release), so the
// encoder's lookahead is (buffers - 2) lines: one at 1280 px, three at 720 px, four at 640 px.
// libdvi's queues hold at most 8.
#define TMDS_POOL_LINES_AT_MAX_WIDTH 3
#define TMDS_MAX_BUFFERS 8

// Sets clocks/voltage for the mode (call FIRST in main, before stdio/wifi), then initialises
// libdvi with KIOSK_DVI_CFG. Returns the active mode info.
void scanout_frame_begin(void);   // call before rendering a frame: recycles colour-pair ids when needed
const video_mode_info_t *scanout_setup_clocks(video_mode_t m, uint32_t sys_clk_khz);
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

typedef struct {
    uint32_t missed_lines, dropped_lines;          // cumulative since boot (red lines / discarded late lines)
    uint32_t irq_max_cycles;                       // worst libdvi DMA IRQ since the last call
    uint32_t worst_line_cycles, worst_line_span_bytes, worst_line_y;   // worst encode since the last call (includes IRQ preemption)
    uint32_t line_budget_cycles;                   // system clock cycles in one scanline of this mode
    uint32_t frames;
    uint32_t tmds_buffers;                         // scanline buffers in use for this mode
    uint32_t over_budget_lines;                    // cumulative: lines whose encode took longer than a scanline
} scanout_profile_t;
void scanout_profile(scanout_profile_t *p);
void scanout_bench(void);
const char *scanout_expander_name(void);   // "assembly", or the C fallback and why
void scanout_set_tmds_enabled(bool on);
void scanout_set_pads(uint8_t drive, bool slew_fast);   // drive 0..3 = 2/4/8/12 mA, for GPIO12-19   // console `bench`: encoder cost per run length (core 0)

// Core 0: converts one rendered row of palette indices into the word-run tokens core 1 expands
// (tmds_wr.h), allocating colour-pair words as needed. Returns bytes, 0 if they exceed max.
uint16_t scanout_encode_line(const uint8_t *px, uint16_t width, uint8_t *out, uint16_t max);
