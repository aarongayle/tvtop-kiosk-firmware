// Core-1 video output on top of PicoDVI's libdvi.
//
// libdvi owns the DMA control blocks, the PIO serialisers and the blanking; we feed it one encoded
// scanline at a time from the line pool. Core 1 runs entirely from SRAM: every function it calls
// after launch is either __not_in_flash_func/__scratch_x here, an inline queue helper from libdvi,
// or a __dvi_func in libdvi itself. That is what allows core 0 to erase and program flash (the
// config sector, the geometry cache) while the picture keeps going.
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/structs/systick.h"
#include "hardware/structs/padsbank0.h"
#include "dvi.h"
#include "dvi_serialiser.h"
#include "common_dvi_pin_configs.h"
#include "scanout.h"
#include "tmds.h"
#include "tmds_wr.h"

struct dvi_inst dvi0;

static const video_mode_info_t *g_mode;
static linepool_t *g_pool;

// One pool of TMDS words, split into as many scanline buffers as the active mode's width allows
// (see scanout.h). More buffers than libdvi's minimum of three give the encoder lines of slack:
// with three at 720x480p60 it had exactly one line period per line and ~20% of lines went out as
// libdvi's red "not ready" fallback.
#define TMDS_WORDS_PER_LANE (OUT_MAX_W / DVI_SYMBOLS_PER_WORD)
static uint32_t tmds_pool[TMDS_POOL_LINES_AT_MAX_WIDTH * 3 * TMDS_WORDS_PER_LANE] __attribute__((aligned(4)));
static uint32_t tmds_nbuf;

// Palette and colour-pair words for the word-run scanline format (tmds_wr.h). Core 0 writes an
// entry before any published line refers to it; core 1 only reads the table.
static wr_ctx_t g_wr;
static uint16_t g_uploaded;   // palette entries already in g_wr
static palette_t *g_pal;      // the palette last uploaded; lines are encoded against it

typedef void (*expand_fn_t)(const uint8_t *, uint32_t, uint32_t *, uint32_t, const uint32_t *);
// Chosen in scanout_init, before core 1 starts, and never changed after.
static expand_fn_t g_expand = wr_expand_line;
static const char *g_expand_name = KIOSK_ASM_EXPANDER ? "assembly" : "C";
static uint8_t g_tok_scratch[LINE_MAX_BYTES];   // core 0 only: self-test and bench

static volatile uint32_t stat_frames;
static volatile uint32_t stat_max_cycles;
static volatile bool core1_ready;

const video_mode_info_t *scanout_setup_clocks(video_mode_t m, uint32_t sys_clk_khz) {
    g_mode = video_mode_info(m);
    vreg_set_voltage((enum vreg_voltage)g_mode->vreg);
    sleep_ms(10);
    // Every clock video_mode_clock_khz returns is exact for the PLL; fall back to the mode's own.
    if (!set_sys_clock_khz(sys_clk_khz, false)) set_sys_clock_khz(g_mode->sys_clk_khz, true);
    return g_mode;
}

void scanout_set_entry(uint8_t idx, uint32_t rgb) {
    wr_set_colour(&g_wr, idx, rgb);
}

// Mixed-pair ids are handed out as new two-colour words appear and were only ever freed when the
// palette shrank. Over hours of boards and game switches the 256 ids filled up, and every new
// anti-aliased edge after that fell back to a wrong word (text on coloured backgrounds turned to
// noise). One frame needs well under 256 (Frontier Island ~100, a World map ~230), so start a
// frame with a fresh table once it is over half full. Lines of the previous frame still on screen
// can show a wrong edge colour until the new frame redraws them, a fraction of a second.
void scanout_frame_begin(void) {
    if ((uint32_t)(g_wr.next_id - WR_PURE_IDS) > (uint32_t)(WR_IDS - WR_PURE_IDS) / 2u) wr_reset_pairs(&g_wr);
}

void scanout_upload_palette(palette_t *pal) {
    g_pal = pal;
    // A palette that shrank was reset at the start of a frame: its indices now mean different
    // colours, so every mixed-pair word built from the old ones is stale.
    // A reset palette can refill past its old size before the next upload, so compare generations,
    // not only counts. Otherwise mixed words keep colours of indices that now mean something else.
    static uint16_t g_generation;
    if (pal->count < g_uploaded || pal->generation != g_generation) { wr_reset_pairs(&g_wr); g_generation = pal->generation; }
    for (uint16_t i = pal->dirty_from; i < pal->count; i++) wr_set_colour(&g_wr, (uint8_t)i, pal->rgb[i]);
    pal->dirty_from = pal->count;
    g_uploaded = pal->count;
}

uint16_t scanout_encode_line(const uint8_t *px, uint16_t width, uint8_t *out, uint16_t max) {
    // Rendering adds palette entries as it goes (anti-aliased edges, alpha blends). A mixed-pair
    // word is computed from the symbols of both indices at the moment it is first used, so any
    // entry added since the last upload must reach the table first, or its words are baked with a
    // stale colour and edges come out dark and broken.
    if (g_pal && g_pal->dirty_from < g_pal->count) scanout_upload_palette(g_pal);
    return wr_line_from_pixels(&g_wr, px, width, out, max);
}

#if KIOSK_ASM_EXPANDER
static bool expander_selftest(uint32_t *a, uint32_t *b, uint32_t w) {
    static const uint32_t colours[6] = { 0x000000, 0xffffff, 0xff2040, 0x20c060, 0x3050ff, 0x808080 };
    static uint8_t px[OUT_MAX_W];
    const uint32_t words = 3u * w / DVI_SYMBOLS_PER_WORD;
    bool ok = true;
    for (uint8_t i = 0; i < 6; i++) wr_set_colour(&g_wr, (uint8_t)(250 + i), colours[i]);
    uint32_t seed = 12345;
    for (int pattern = 0; pattern < 24 && ok; pattern++) {
        uint32_t x = 0;
        while (x < w) {   // run lengths from 1 px to over 64 words, so RUN, LITERAL and clamping all run
            seed = seed * 1103515245u + 12345u;
            uint32_t len = pattern < 8 ? 1u + (seed >> 16) % 6u : 1u + (seed >> 16) % (pattern * 20u);
            uint8_t c = (uint8_t)(250 + (seed >> 24) % 6);
            while (len-- && x < w) px[x++] = c;
        }
        uint16_t n = wr_line_from_pixels(&g_wr, px, (uint16_t)w, g_tok_scratch, sizeof g_tok_scratch);
        if (pattern == 23) n = (uint16_t)(n / 3 & ~1u);   // truncated stream: exercises the padding
        memset(a, 0x55, words * 4); memset(b, 0xAA, words * 4);
        wr_expand_line(g_tok_scratch, n, a, w, g_wr.table);
        wr_expand_line_c(g_tok_scratch, n, b, w, g_wr.table);
        ok = memcmp(a, b, words * 4) == 0;
    }
    for (uint8_t i = 0; i < 6; i++) wr_set_colour(&g_wr, (uint8_t)(250 + i), 0);
    wr_reset_pairs(&g_wr);
    return ok;
}
#endif

const char *scanout_expander_name(void) { return g_expand_name; }

void scanout_init(const video_mode_info_t *mode, linepool_t *pool) {
    g_mode = mode;
    g_pool = pool;
    wr_init(&g_wr);
    dvi0.timing = mode->timing;
    dvi0.ser_cfg = KIOSK_DVI_CFG;
    dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());
    uint32_t words_per_buf = 3u * (uint32_t)mode->w / DVI_SYMBOLS_PER_WORD;
    tmds_nbuf = (uint32_t)(sizeof tmds_pool / sizeof tmds_pool[0]) / words_per_buf;
    if (tmds_nbuf > TMDS_MAX_BUFFERS) tmds_nbuf = TMDS_MAX_BUFFERS;
#if KIOSK_ASM_EXPANDER
    // The pool's first two buffers are free until they are queued below: use them to check the
    // assembly expander against the C reference on lines that exercise every token path.
    if (!expander_selftest(tmds_pool, tmds_pool + words_per_buf, mode->w)) {
        g_expand = wr_expand_line_c;
        g_expand_name = "C (assembly failed its boot self-test)";
    }
#endif
    for (uint32_t i = 0; i < tmds_nbuf; i++) {
        uint32_t *buf = tmds_pool + i * words_per_buf;
        // Start with valid black symbols so the first frames are clean even if core 1 is late.
        for (int w = 0; w < words_per_buf; w++) buf[w] = g_wr.table[0];
        queue_add_blocking_u32(&dvi0.q_tmds_free, &buf);
    }
}

static volatile uint32_t stat_worst_len, stat_worst_y, stat_over_budget;

static void __scratch_x("core1_main") core1_main(void) {
    // SysTick on this core for the per-line and per-IRQ profile (counts down, wraps at 2^24).
    // Enabled before the DMA IRQ so libdvi's IRQ timing is valid from the first line.
    systick_hw->rvr = 0xFFFFFF;
    systick_hw->cvr = 0;
    systick_hw->csr = 5;   // enable, processor clock
    dvi_register_irqs_this_core(&dvi0, DMA_IRQ_0);
    dvi_start(&dvi0);
    core1_ready = true;
    const uint16_t h = g_mode->h, w = g_mode->w;
    const struct dvi_timing *tm = g_mode->timing;
    const uint32_t budget = 10u * (tm->h_active_pixels + tm->h_front_porch + tm->h_sync_width + tm->h_back_porch);
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
            g_expand(spans, len, buf, w, g_wr.table);
            uint32_t cycles = (t0 - systick_hw->cvr) & 0xFFFFFF;
            linepool_reader_done(g_pool);
            if (cycles > stat_max_cycles) { stat_max_cycles = cycles; stat_worst_len = len; stat_worst_y = y; }
            if (cycles > budget) stat_over_budget++;
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
uint32_t scanout_max_line_cycles(void) { return stat_max_cycles; }   // peek; scanout_profile() resets

void scanout_profile(scanout_profile_t *p) {
    const struct dvi_timing *t = g_mode->timing;
    p->missed_lines = dvi0.stat_missed_lines;
    p->dropped_lines = dvi0.stat_dropped_lines;
    p->irq_max_cycles = dvi0.stat_irq_max_cycles;
    dvi0.stat_irq_max_cycles = 0;
    p->worst_line_cycles = stat_max_cycles;
    p->worst_line_span_bytes = stat_worst_len;
    p->worst_line_y = stat_worst_y;
    stat_max_cycles = 0;
    // One TMDS bit per system clock cycle, ten bits per pixel.
    p->line_budget_cycles = 10u * (t->h_active_pixels + t->h_front_porch + t->h_sync_width + t->h_back_porch);
    p->frames = stat_frames;
    p->tmds_buffers = tmds_nbuf;
    p->over_budget_lines = stat_over_budget;
}

// Times wr_expand_line on core 0 for synthetic lines at the active mode's width: alternating colours
// with a fixed run length, and a text-like line of short random runs over a few colours. The
// minimum of several runs discards USB/Wi-Fi interrupts.
static uint32_t bench_expand(expand_fn_t f, const uint8_t *tok, uint16_t n, uint32_t *out, uint32_t w) {
    uint32_t best = 0xFFFFFF;
    for (int k = 0; k < 16; k++) {
        uint32_t t0 = systick_hw->cvr;
        f(tok, n, out, w, g_wr.table);
        uint32_t dt = (t0 - systick_hw->cvr) & 0xFFFFFF;
        if (dt < best) best = dt;
    }
    return best;
}

void scanout_bench(void) {
    static uint8_t px[OUT_MAX_W];
    static uint8_t tok[LINE_MAX_BYTES];
    static uint32_t out[3 * OUT_MAX_W / DVI_SYMBOLS_PER_WORD];
    static const uint16_t runs[] = { 1, 2, 3, 4, 8, 16, 64, 256, 0 };   // 0 = text-like
    const uint32_t w = g_mode->w;
    const struct dvi_timing *tm = g_mode->timing;
    uint32_t budget = 10u * (tm->h_active_pixels + tm->h_front_porch + tm->h_sync_width + tm->h_back_porch);
    systick_hw->rvr = 0xFFFFFF;
    systick_hw->cvr = 0;
    systick_hw->csr = 5;
    printf("expander bench on core 0 (%s): width %lu, line budget %lu cycles\n", g_expand_name, (unsigned long)w, (unsigned long)budget);
    uint32_t seed = 1;
    for (unsigned r = 0; r < sizeof runs / sizeof runs[0]; r++) {
        if (runs[r]) {
            for (uint32_t x = 0; x < w; x++) px[x] = (uint8_t)(1 + (x / runs[r]) % 2);
        } else {
            uint32_t x = 0;
            while (x < w) {
                seed = seed * 1103515245u + 12345u;
                uint32_t len = 1 + (seed >> 16) % 5;
                uint8_t c = (uint8_t)(1 + (seed >> 24) % 6);
                while (len-- && x < w) px[x++] = c;
            }
        }
        uint16_t n = scanout_encode_line(px, (uint16_t)w, tok, sizeof tok);
        uint32_t best = bench_expand(g_expand, tok, n, out, w);
        printf("  %s%3u px: %4u token bytes, %6lu cycles (%3lu%% of budget), %lu.%lu cycles/word",
               runs[r] ? "run " : "text", runs[r], n, (unsigned long)best, (unsigned long)(100u * best / budget),
               (unsigned long)(best / (w / 2)), (unsigned long)((best * 10u / (w / 2)) % 10u));
#if KIOSK_ASM_EXPANDER
        printf(" | C %lu cycles", (unsigned long)bench_expand(wr_expand_line_c, tok, n, out, w));
#endif
        printf("\n");
    }
    printf("  mixed-pair ids in use %u of %u, table-full fallbacks %lu\n", g_wr.next_id - WR_PURE_IDS, WR_IDS - WR_PURE_IDS, (unsigned long)g_wr.stats_table_full);
    systick_hw->csr = 0;
}

// Rewrites the pad settings of the eight TMDS pins (GPIO12-19) while video runs. libdvi's default
// is 2 mA with slew limiting; the PiCowBell's 220 ohm series resistors were chosen for the
// RP2350's HSTX drivers, so an RP2040 may need more drive for a clean eye at 270+ Mbit/s.
// drive: 0 = 2 mA, 1 = 4 mA, 2 = 8 mA, 3 = 12 mA. The input buffer stays disabled.
// Diagnostic: detach GPIO12-19 from the PIO serialiser and hold them low, so nothing radiates from
// the DVI connector, or hand them back. Video keeps running internally; the monitor loses signal.
void scanout_set_tmds_enabled(bool on) {
    for (uint g = 12; g <= 19; g++) {
        if (on) {
            gpio_set_function(g, GPIO_FUNC_PIO0);
        } else {
            gpio_put(g, 0);
            gpio_set_dir(g, true);
            gpio_set_function(g, GPIO_FUNC_SIO);
        }
    }
}

void scanout_set_pads(uint8_t drive, bool slew_fast) {
    for (uint g = 12; g <= 19; g++) {
        hw_write_masked(&padsbank0_hw->io[g],
                        ((uint32_t)(drive & 3u) << PADS_BANK0_GPIO0_DRIVE_LSB) | (slew_fast ? PADS_BANK0_GPIO0_SLEWFAST_BITS : 0u),
                        PADS_BANK0_GPIO0_DRIVE_BITS | PADS_BANK0_GPIO0_SLEWFAST_BITS);
    }
}
