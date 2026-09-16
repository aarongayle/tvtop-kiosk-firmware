// Core-1 video through the RP2350's HSTX peripheral (Pico 2 W).
//
// HSTX serialises and TMDS-encodes in hardware, so there is no software TMDS encoder and no palette
// of DC-balanced colours. Each scanline is a short command stream (hstx_line.h) that DMA streams into
// the HSTX FIFO. Core 0 packs rendered rows into runs that carry their colour; core 1 expands those
// into command streams a few lines ahead of the beam (two words a run, no lookups), and
// a DMA interrupt, also on core 1, hands them to the hardware in order. Everything core 1 runs after
// start-up is in RAM, so core 0 can erase and program flash while the picture keeps going.
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/vreg.h"
#include "hardware/regs/addressmap.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/timer.h"
#include "scanout.h"
#include "hstx_line.h"

#define HSTX_FIRST_GPIO 12u
#define HSTX_LAST_GPIO 19u
#define LINE_SLOTS 8                                  // lines built ahead of the beam
// Runs per line a slot can hold. The densest line seen on a real board is a few hundred runs; a
// line with more is sent black and counted as dropped rather than sizing every slot for 1920.
#define HSTX_MAX_RUNS 1024
#define LINE_WORDS (6u + 2u * HSTX_MAX_RUNS + 2u)      // prefix + two words a run + padding

static const video_mode_info_t *g_mode;
// Core 1 reads the timing for every line it builds, and the mode table is const data in flash,
// which is unreadable while core 0 erases or programs the geometry cache. So core 1 uses this copy.
static hstx_timing_t g_timing;
static linepool_t *g_pool;
static uint32_t g_sys_khz;

// Palette index -> 0x00RRGGBB, in two tables. Core 0 writes an entry before any published line uses
// it. Each line names the table it was packed against; a palette reset moves new lines to the other
// table, so the previous frame's lines still on screen keep their colours until they are replaced.
static uint32_t g_tables[2][256];
// Not const: core 1 reads this for every line, and a const array lands in flash, which stops
// answering while core 0 writes the config or the geometry cache. Everything core 1 touches is RAM.
static const uint32_t *g_table_ptrs[2] = { g_tables[0], g_tables[1] };
static uint8_t g_table;
static uint16_t g_generation;
#define g_rgb (g_tables[g_table])

static uint32_t g_blank_vsync[6], g_blank[6], g_prefix[6];
static uint32_t g_black[8];
static uint32_t g_black_len;

// Line slots: FREE (building or unused), READY (built for g_y), POSTED (handed to DMA).
enum { SLOT_FREE = 0, SLOT_READY = 1, SLOT_POSTED = 2 };
static uint32_t g_words[LINE_SLOTS][LINE_WORDS];
static uint32_t g_len[LINE_SLOTS];
static uint16_t g_y[LINE_SLOTS];
static volatile uint8_t g_state[LINE_SLOTS];

static uint32_t g_v_total, g_v_blank, g_vsync_first, g_vsync_end;
static int g_ch[2];
static int8_t g_posted[2] = { -1, -1 };
static bool g_pong;
static volatile uint32_t g_next = 2;   // scanline the next interrupt posts (two are preloaded)

static volatile uint32_t stat_frames, stat_missed, stat_dropped, stat_worst_us, stat_worst_y, stat_worst_len;
static volatile bool core1_ready;

// ---------------- Flash timing ----------------
//
// The flash interface runs at a divisor chosen for the stock 150 MHz clock. At 372 MHz that clocks
// the flash near 190 MHz and execute-in-place reads corrupt: PicoHDMI's 720p builds hard-fault the
// same way and run from RAM instead, which this firmware is too large to do. So the divisor goes up
// before the system clock does, keeping the flash clock at or below 100 MHz with the receive delay
// at half the divisor (CLKDIV 4, RXDELAY 2 at 372 MHz, the setting RP2350 overclockers use).
static uint32_t g_flash_timing;

static void __no_inline_not_in_flash_func(apply_flash_timing)(uint32_t timing) {
    qmi_hw->m[0].timing = timing;
    // A divisor change takes effect from the next transfer: make one now.
    volatile uint8_t probe = *(const uint8_t *)XIP_BASE;
    (void)probe;
}

static void slow_flash_for(uint32_t sys_khz) {
    uint32_t clkdiv = (sys_khz + 99999u) / 100000u;
    if (clkdiv < 2u) clkdiv = 2u;
    uint32_t current = qmi_hw->m[0].timing;
    uint32_t have = (current & QMI_M0_TIMING_CLKDIV_BITS) >> QMI_M0_TIMING_CLKDIV_LSB;
    if (have >= clkdiv) { g_flash_timing = current; return; }
    uint32_t timing = (current & ~(QMI_M0_TIMING_CLKDIV_BITS | QMI_M0_TIMING_RXDELAY_BITS)) |
                      (clkdiv << QMI_M0_TIMING_CLKDIV_LSB) | ((clkdiv / 2u) << QMI_M0_TIMING_RXDELAY_LSB);
    uint32_t irq = save_and_disable_interrupts();
    apply_flash_timing(timing);
    restore_interrupts(irq);
    g_flash_timing = timing;
}

// The ROM flash routines behind erase and program re-enter XIP with the boot-time timing. Called by
// flash_store.c, from RAM with interrupts off, before anything runs from flash again.
void __no_inline_not_in_flash_func(scanout_flash_timing_restore)(void) {
    if (g_flash_timing) apply_flash_timing(g_flash_timing);
}

// ---------------- Clocks and HSTX ----------------

const video_mode_info_t *scanout_setup_clocks(video_mode_t m, uint32_t sys_clk_khz) {
    g_mode = video_mode_info(m);
    vreg_set_voltage((enum vreg_voltage)g_mode->vreg);
    sleep_ms(10);
    uint vco, postdiv1, postdiv2;
    if (!check_sys_clock_khz(sys_clk_khz, &vco, &postdiv1, &postdiv2)) sys_clk_khz = g_mode->sys_clk_khz;
    slow_flash_for(sys_clk_khz);
    set_sys_clock_khz(sys_clk_khz, true);
    clock_configure_int_divider(clk_hstx, 0, CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLK_SYS, sys_clk_khz * 1000u, g_mode->hstx_clk_div);
    g_sys_khz = sys_clk_khz;
    return g_mode;
}

void scanout_set_pads(uint8_t drive, bool slew_fast) {
    for (uint g = HSTX_FIRST_GPIO; g <= HSTX_LAST_GPIO; g++) {
        gpio_set_drive_strength(g, (enum gpio_drive_strength)(drive & 3u));
        gpio_set_slew_rate(g, slew_fast ? GPIO_SLEW_RATE_FAST : GPIO_SLEW_RATE_SLOW);
    }
}

void scanout_set_tmds_enabled(bool on) {
    for (uint g = HSTX_FIRST_GPIO; g <= HSTX_LAST_GPIO; g++) {
        if (on) {
            gpio_set_function(g, GPIO_FUNC_HSTX);
        } else {
            gpio_put(g, 0);
            gpio_set_dir(g, true);
            gpio_set_function(g, GPIO_FUNC_SIO);
        }
    }
}

static void hstx_configure(void) {
    // One pixel per FIFO word, 0x00RRGGBB: lane 2 (red) takes bits 23..16, lane 1 (green) 15..8,
    // lane 0 (blue) 7..0, all eight bits. Raw words are one 30-bit control symbol each.
    hstx_ctrl_hw->expand_tmds =
        7u << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB | 16u << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB |
        7u << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB | 8u << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB |
        7u << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB | 0u << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB;
    hstx_ctrl_hw->expand_shift =
        1u << HSTX_CTRL_EXPAND_SHIFT_ENC_N_SHIFTS_LSB | 0u << HSTX_CTRL_EXPAND_SHIFT_ENC_SHIFT_LSB |
        1u << HSTX_CTRL_EXPAND_SHIFT_RAW_N_SHIFTS_LSB | 0u << HSTX_CTRL_EXPAND_SHIFT_RAW_SHIFT_LSB;
    // Ten TMDS bits per pixel, two per HSTX clock cycle: five cycles a pixel, and the clock lane
    // repeats every five cycles.
    hstx_ctrl_hw->csr = 0;
    hstx_ctrl_hw->csr = HSTX_CTRL_CSR_EXPAND_EN_BITS | 5u << HSTX_CTRL_CSR_CLKDIV_LSB |
                        5u << HSTX_CTRL_CSR_N_SHIFTS_LSB | 2u << HSTX_CTRL_CSR_SHIFT_LSB | HSTX_CTRL_CSR_EN_BITS;

    // PiCowBell pairs: D0 GPIO12/13 (bits 0/1), CK 14/15 (2/3), D2 16/17 (4/5), D1 18/19 (6/7).
    hstx_ctrl_hw->bit[2] = HSTX_CTRL_BIT0_CLK_BITS;
    hstx_ctrl_hw->bit[3] = HSTX_CTRL_BIT0_CLK_BITS | HSTX_CTRL_BIT0_INV_BITS;
    static const uint8_t lane_bit[3] = { 0, 6, 4 };
    for (uint lane = 0; lane < 3; lane++) {
        uint32_t sel = (lane * 10u) << HSTX_CTRL_BIT0_SEL_P_LSB | (lane * 10u + 1u) << HSTX_CTRL_BIT0_SEL_N_LSB;
        hstx_ctrl_hw->bit[lane_bit[lane]] = sel;
        hstx_ctrl_hw->bit[lane_bit[lane] + 1] = sel | HSTX_CTRL_BIT0_INV_BITS;
    }
    scanout_set_tmds_enabled(true);
    // 744 Mbit/s through the PiCowBell's series resistors: full drive and fast edges, as PicoHDMI
    // uses for 720p.
    scanout_set_pads(3, true);
}

// ---------------- Palette and lines (core 0) ----------------

void scanout_frame_begin(void) {}

void scanout_set_entry(uint8_t idx, uint32_t rgb) { g_rgb[idx] = rgb & 0xffffffu; }

static palette_t *g_pal;

void scanout_upload_palette(palette_t *pal) {
    uint16_t from = pal->dirty_from;
    if (g_pal != NULL && pal->generation != g_generation) {
        // Reset: indices now mean other colours. Fill the other table from scratch and pack new lines
        // against it; the old table stays intact for the lines still showing the previous frame.
        g_table ^= 1u;
        from = 0;
    }
    g_pal = pal;
    g_generation = pal->generation;
    for (uint16_t i = from; i < pal->count; i++) g_rgb[i] = pal->rgb[i] & 0xffffffu;
    pal->dirty_from = pal->count;
}

uint16_t scanout_encode_line(const uint8_t *px, uint16_t width, uint8_t *out, uint16_t max) {
    // Colours added while rendering (blends, anti-aliased edges) must be in the table before a line
    // that uses them is published.
    if (g_pal && (g_pal->dirty_from < g_pal->count || g_pal->generation != g_generation)) scanout_upload_palette(g_pal);
    return hstx_pack_line(px, width, g_table, out, max);
}

// ---------------- Core 1 ----------------

static void __not_in_flash_func(dma_irq)(void) {
    int idx = g_pong ? 1 : 0;
    uint ch = (uint)g_ch[idx];
    dma_hw->intr = 1u << ch;
    g_pong = !g_pong;
    // This channel finished the line it was given last time.
    if (g_posted[idx] >= 0) {
        g_state[g_posted[idx]] = SLOT_FREE;
        g_posted[idx] = -1;
    }

    dma_channel_hw_t *c = &dma_hw->ch[ch];
    uint32_t v = g_next;
    if (v >= g_vsync_first && v < g_vsync_end) {
        c->read_addr = (uintptr_t)g_blank_vsync;
        c->transfer_count = 6;
    } else if (v < g_v_blank) {
        c->read_addr = (uintptr_t)g_blank;
        c->transfer_count = 6;
    } else {
        uint16_t y = (uint16_t)(v - g_v_blank);
        int s = -1;
        for (int k = 0; k < LINE_SLOTS; k++) {
            if (g_state[k] == SLOT_READY && g_y[k] == y) { s = k; break; }
        }
        if (s >= 0) {
            g_state[s] = SLOT_POSTED;
            g_posted[idx] = (int8_t)s;
            c->read_addr = (uintptr_t)g_words[s];
            c->transfer_count = g_len[s];
        } else {
            stat_missed++;   // the line was not built in time: send it black rather than stall
            c->read_addr = (uintptr_t)g_black;
            c->transfer_count = g_black_len;
        }
    }
    if (++v >= g_v_total) {
        v = 0;
        stat_frames++;
    }
    g_next = v;
}

static void __not_in_flash_func(build_line)(int slot, uint16_t y) {
    // A DUP_PREV line repeats the nearest line above it that has spans.
    uint16_t src = y;
    line_ref_t ref = linepool_reader_begin(g_pool, src);
    while ((ref.flags & LINE_FLAG_DUP_PREV) && src > 0) {
        linepool_reader_done(g_pool);
        src--;
        ref = linepool_reader_begin(g_pool, src);
    }
    uint32_t t0 = timer_hw->timerawl;
    const uint8_t *spans = (ref.flags & LINE_FLAG_DUP_PREV) ? 0 : g_pool->pool + ref.off;
    uint32_t len = spans ? ref.len : 0u;
    uint32_t n = hstx_active_line_packed(&g_timing, g_prefix, spans, len, g_table_ptrs, g_words[slot], LINE_WORDS);
    linepool_reader_done(g_pool);
    uint32_t dt = timer_hw->timerawl - t0;
    if (dt > stat_worst_us) { stat_worst_us = dt; stat_worst_y = y; stat_worst_len = len; }
    if (!n) {
        if (len) stat_dropped++;   // more runs than a slot holds
        for (uint32_t k = 0; k < g_black_len; k++) g_words[slot][k] = g_black[k];
        n = g_black_len;
    }
    g_len[slot] = n;
    g_y[slot] = y;
    g_state[slot] = SLOT_READY;   // last: the interrupt may take it from here on
}

static void __not_in_flash_func(core1_main)(void) {
    // Start-up only (these SDK calls run from flash, and core 0 waits on core1_ready).
    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq);
    irq_set_enabled(DMA_IRQ_0, true);
    dma_irqn_set_channel_mask_enabled(0, (1u << g_ch[0]) | (1u << g_ch[1]), true);
    dma_channel_start((uint)g_ch[0]);
    core1_ready = true;

    for (;;) {
        uint32_t next = g_next;
        uint32_t irq = save_and_disable_interrupts();
        // Lines built for a scanline the beam has passed are stale: free them.
        for (int k = 0; k < LINE_SLOTS; k++) {
            if (g_state[k] != SLOT_READY) continue;
            uint32_t dist = (g_y[k] + g_v_blank + g_v_total - next) % g_v_total;
            if (dist >= LINE_SLOTS) g_state[k] = SLOT_FREE;
        }
        restore_interrupts(irq);

        int want = -1;
        for (uint32_t ahead = 0; ahead < LINE_SLOTS && want < 0; ahead++) {
            uint32_t v = (next + ahead) % g_v_total;
            if (v < g_v_blank) continue;
            uint16_t y = (uint16_t)(v - g_v_blank);
            bool have = false;
            for (int k = 0; k < LINE_SLOTS; k++) {
                if (g_state[k] != SLOT_FREE && g_y[k] == y) { have = true; break; }
            }
            if (!have) want = y;
        }
        if (want < 0) continue;   // everything ahead of the beam is built

        int slot = -1;
        for (int k = 0; k < LINE_SLOTS; k++) {
            if (g_state[k] == SLOT_FREE) { slot = k; break; }
        }
        if (slot >= 0) build_line(slot, (uint16_t)want);
    }
}

void scanout_init(const video_mode_info_t *mode, linepool_t *pool) {
    g_mode = mode;
    g_pool = pool;
    g_timing = mode->timing;
    const hstx_timing_t *t = &g_timing;
    g_vsync_first = t->v_front;
    g_vsync_end = t->v_front + t->v_sync;
    g_v_blank = t->v_front + t->v_sync + t->v_back;
    g_v_total = g_v_blank + t->v_active;
    hstx_blank_line(t, true, g_blank_vsync);
    hstx_blank_line(t, false, g_blank);
    hstx_line_prefix(t, false, g_prefix);
    g_black_len = hstx_active_line(t, g_prefix, 0, 0, g_rgb, g_black, sizeof g_black / sizeof g_black[0]);

    hstx_configure();

    g_ch[0] = dma_claim_unused_channel(true);
    g_ch[1] = dma_claim_unused_channel(true);
    for (int i = 0; i < 2; i++) {
        dma_channel_config c = dma_channel_get_default_config((uint)g_ch[i]);
        channel_config_set_chain_to(&c, (uint)g_ch[1 - i]);
        channel_config_set_dreq(&c, DREQ_HSTX);
        dma_channel_configure((uint)g_ch[i], &c, &hstx_fifo_hw->fifo, g_blank, 6, false);
    }
    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;
    (void)g_pal;
}

void scanout_start(void) {
    multicore_launch_core1(core1_main);
    while (!core1_ready) tight_loop_contents();
}

// ---------------- Stats ----------------

uint32_t scanout_late_lines(void) { return stat_missed; }
uint32_t scanout_frames(void) { return stat_frames; }
uint32_t scanout_max_line_cycles(void) { return stat_worst_us * (g_sys_khz / 1000u); }
const char *scanout_expander_name(void) { return "HSTX hardware TMDS"; }

void scanout_profile(scanout_profile_t *p) {
    memset(p, 0, sizeof *p);
    const hstx_timing_t *t = &g_mode->timing;
    p->missed_lines = stat_missed;
    p->dropped_lines = stat_dropped;
    p->frames = stat_frames;
    p->worst_line_cycles = stat_worst_us * (g_sys_khz / 1000u);
    p->worst_line_span_bytes = stat_worst_len;
    p->worst_line_y = stat_worst_y;
    p->line_budget_cycles = 5u * g_mode->hstx_clk_div * (uint32_t)(t->h_front + t->h_sync + t->h_back + t->h_active);
    p->tmds_buffers = LINE_SLOTS;
    stat_worst_us = 0;
}

void scanout_bench(void) {
    printf("HSTX backend: TMDS is encoded in hardware; core 1 builds %d lines ahead of the beam.\n"
           "  clk_sys %lu kHz, clk_hstx %lu Hz, %u x %u, worst line build %lu us, black fallback lines %lu\n",
           LINE_SLOTS, (unsigned long)g_sys_khz, (unsigned long)clock_get_hz(clk_hstx), g_mode->w, g_mode->h,
           (unsigned long)stat_worst_us, (unsigned long)stat_missed);
}
