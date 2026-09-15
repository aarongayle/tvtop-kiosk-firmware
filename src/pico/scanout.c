// Core-1 video output on top of PicoDVI's libdvi.
//
// libdvi owns the DMA control blocks, the PIO serialisers and the blanking; we feed it one encoded
// scanline at a time from the line pool. Core 1 runs entirely from SRAM: every function it calls
// after launch is either __not_in_flash_func/__scratch_x here, an inline queue helper from libdvi,
// or a __dvi_func in libdvi itself. That is what allows core 0 to erase and program flash (the
// config sector, the geometry cache) while the picture keeps going.
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/structs/systick.h"
#include "dvi.h"
#include "dvi_serialiser.h"
#include "common_dvi_pin_configs.h"
#include "scanout.h"
#include "tmds.h"

struct dvi_inst dvi0;

static const video_mode_info_t *g_mode;
static linepool_t *g_pool;

// Three scanline buffers (libdvi's queue depth) of 3 lanes × width/2 words each.
#define TMDS_WORDS_PER_LANE (OUT_MAX_W / DVI_SYMBOLS_PER_WORD)
static uint32_t tmds_buf[DVI_N_TMDS_BUFFERS_OURS][3 * TMDS_WORDS_PER_LANE] __attribute__((aligned(4)));

// Palette → symbol tables, indexed [lane][palette index]; lane 0 = blue, 1 = green, 2 = red.
static uint16_t sym_lut[3][256];
static uint32_t pair_lut[3][256];

static volatile uint32_t stat_frames;
static volatile uint32_t stat_max_cycles;
static volatile bool core1_ready;

const video_mode_info_t *scanout_setup_clocks(video_mode_t m) {
    g_mode = video_mode_info(m);
    vreg_set_voltage((enum vreg_voltage)g_mode->vreg);
    sleep_ms(10);
    set_sys_clock_khz(g_mode->sys_clk_khz, true);
    return g_mode;
}

void scanout_set_entry(uint8_t idx, uint32_t rgb) {
    uint8_t comp[3] = { (uint8_t)rgb, (uint8_t)(rgb >> 8), (uint8_t)(rgb >> 16) };   // b, g, r
    for (int lane = 0; lane < 3; lane++) {
        uint16_t s = tmds_symbol_balanced(tmds_nearest_balanced(comp[lane]));
        sym_lut[lane][idx] = s;
        pair_lut[lane][idx] = tmds_pack2(s, s);
    }
}

void scanout_upload_palette(palette_t *pal) {
    for (uint16_t i = pal->dirty_from; i < pal->count; i++) scanout_set_entry((uint8_t)i, pal->rgb[i]);
    pal->dirty_from = pal->count;
}

void scanout_init(const video_mode_info_t *mode, linepool_t *pool) {
    g_mode = mode;
    g_pool = pool;
    for (int i = 0; i < 256; i++) scanout_set_entry((uint8_t)i, 0);
    dvi0.timing = mode->timing;
    dvi0.ser_cfg = KIOSK_DVI_CFG;
    dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());
    for (int i = 0; i < DVI_N_TMDS_BUFFERS_OURS; i++) {
        uint32_t *buf = tmds_buf[i];
        // Start with valid black symbols so the first frames are clean even if core 1 is late.
        for (int w = 0; w < 3 * TMDS_WORDS_PER_LANE; w++) buf[w] = pair_lut[0][0];
        queue_add_blocking_u32(&dvi0.q_tmds_free, &buf);
    }
}

static void __scratch_x("core1_main") core1_main(void) {
    dvi_register_irqs_this_core(&dvi0, DMA_IRQ_0);
    dvi_start(&dvi0);
    core1_ready = true;
    // SysTick on this core for the per-line profile (counts down, wraps at 2^24).
    systick_hw->rvr = 0xFFFFFF;
    systick_hw->cvr = 0;
    systick_hw->csr = 5;   // enable, processor clock
    const uint16_t h = g_mode->h, w = g_mode->w;
    const uint8_t *prev_spans = NULL;
    uint16_t prev_len = 0;
    while (true) {
        for (uint16_t y = 0; y < h; y++) {
            uint32_t *buf;
            queue_remove_blocking_u32(&dvi0.q_tmds_free, &buf);
            line_ref_t ref = linepool_reader_begin(g_pool, y);
            const uint8_t *spans = g_pool->pool + ref.off;
            uint16_t len = ref.len;
            if ((ref.flags & LINE_FLAG_DUP_PREV) && prev_spans) { spans = prev_spans; len = prev_len; }
            uint32_t t0 = systick_hw->cvr;
            tmds_rle_encode_line(spans, len, buf, w, &pair_lut[0][0], &sym_lut[0][0]);
            uint32_t cycles = (t0 - systick_hw->cvr) & 0xFFFFFF;
            linepool_reader_done(g_pool);
            if (cycles > stat_max_cycles) stat_max_cycles = cycles;
            prev_spans = spans; prev_len = len;
            queue_add_blocking_u32(&dvi0.q_tmds_valid, &buf);
        }
        stat_frames++;
    }
}

void scanout_start(void) {
    multicore_launch_core1(core1_main);
    while (!core1_ready) tight_loop_contents();
}

uint32_t scanout_late_lines(void) { return dvi0.late_scanline_ctr; }
uint32_t scanout_frames(void) { return stat_frames; }
uint32_t scanout_max_line_cycles(void) { uint32_t c = stat_max_cycles; stat_max_cycles = 0; return c; }
