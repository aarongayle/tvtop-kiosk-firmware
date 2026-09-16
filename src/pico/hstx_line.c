// See hstx_line.h.
#include "hstx_line.h"

// On the device these run on core 1 while core 0 may be writing flash, so they live in RAM.
#if defined(KIOSK_HOST)
#define HSTX_RAM
#else
#define HSTX_RAM __attribute__((section(".time_critical.hstx_line"), noinline))
#endif

uint32_t HSTX_RAM hstx_sync_word(const hstx_timing_t *t, bool vsync, bool hsync) {
    bool v = vsync ? t->v_positive : !t->v_positive;
    bool h = hsync ? t->h_positive : !t->h_positive;
    uint32_t lane0 = v ? (h ? HSTX_TMDS_CTRL_11 : HSTX_TMDS_CTRL_10) : (h ? HSTX_TMDS_CTRL_01 : HSTX_TMDS_CTRL_00);
    return lane0 | (HSTX_TMDS_CTRL_00 << 10) | (HSTX_TMDS_CTRL_00 << 20);
}

void hstx_line_prefix(const hstx_timing_t *t, bool vsync, uint32_t prefix[6]) {
    prefix[0] = HSTX_CMD_RAW_REPEAT | t->h_front;
    prefix[1] = hstx_sync_word(t, vsync, false);
    prefix[2] = HSTX_CMD_RAW_REPEAT | t->h_sync;
    prefix[3] = hstx_sync_word(t, vsync, true);
    prefix[4] = HSTX_CMD_RAW_REPEAT | t->h_back;
    prefix[5] = hstx_sync_word(t, vsync, false);
}

uint32_t hstx_blank_line(const hstx_timing_t *t, bool vsync, uint32_t *out) {
    hstx_line_prefix(t, vsync, out);
    out[4] = HSTX_CMD_RAW_REPEAT | (uint32_t)(t->h_back + t->h_active);
    return 6;
}

uint32_t HSTX_RAM hstx_active_line(const hstx_timing_t *t, const uint32_t prefix[6], const uint8_t *spans, uint32_t len,
                                   const uint32_t *rgb, uint32_t *out, uint32_t max) {
    uint32_t n = 0;
    const uint32_t width = t->h_active;
    if (max < 8) return 0;
    for (uint32_t k = 0; k < 6; k++) out[n++] = prefix[k];

    uint32_t x = 0, i = 0;
    while (i + 1 < len && x < width) {
        uint32_t run = (uint32_t)spans[i + 1] + 1u;
        if (run > width - x) run = width - x;
        if (run >= 2) {
            if (n + 2 > max) return 0;
            out[n++] = HSTX_CMD_TMDS_REPEAT | run;
            out[n++] = rgb[spans[i]];
            x += run;
            i += 2;
            continue;
        }
        // A stretch of single pixels (anti-aliased edges, text): one command, then each colour.
        uint32_t count = 0;
        while (i + 2 * count + 1 < len && spans[i + 2 * count + 1] == 0 && x + count < width && count < HSTX_MAX_COUNT) count++;
        if (n + 1 + count > max) return 0;
        out[n++] = HSTX_CMD_TMDS | count;
        for (uint32_t k = 0; k < count; k++) out[n++] = rgb[spans[i + 2 * k]];
        x += count;
        i += 2 * count;
    }
    if (x < width) {
        if (n + 2 > max) return 0;
        out[n++] = HSTX_CMD_TMDS_REPEAT | (width - x);
        out[n++] = 0;
    }
    return n;
}

uint16_t hstx_pack_line(const uint8_t *px, uint16_t width, const uint32_t *rgb, uint8_t *out, uint16_t max) {
    uint32_t n = 0, x = 0;
    while (x < width) {
        uint8_t idx = px[x];
        uint32_t run = 1;
        while (run < 256u && x + run < width && px[x + run] == idx) run++;
        if (n + 4u > max) return 0;
        uint32_t c = rgb[idx] & 0xffffffu;
        out[n++] = (uint8_t)c;
        out[n++] = (uint8_t)(c >> 8);
        out[n++] = (uint8_t)(c >> 16);
        out[n++] = (uint8_t)(run - 1u);
        x += run;
    }
    return (uint16_t)n;
}

uint32_t HSTX_RAM hstx_active_line_packed(const hstx_timing_t *t, const uint32_t prefix[6], const uint8_t *runs, uint32_t len,
                                          uint32_t *out, uint32_t max) {
    const uint32_t width = t->h_active;
    if (max < 8u) return 0;
    uint32_t n = 0;
    for (uint32_t k = 0; k < 6u; k++) out[n++] = prefix[k];
    uint32_t x = 0;
    const uint32_t limit = max - 2u;   // room for the padding pair
    for (uint32_t i = 0; i + 3u < len && x < width; i += 4u) {
        uint32_t run = (uint32_t)runs[i + 3] + 1u;
        if (run > width - x) run = width - x;
        if (n + 2u > limit) return 0;
        out[n++] = HSTX_CMD_TMDS_REPEAT | run;
        out[n++] = (uint32_t)runs[i] | (uint32_t)runs[i + 1] << 8 | (uint32_t)runs[i + 2] << 16;
        x += run;
    }
    if (x < width) {
        out[n++] = HSTX_CMD_TMDS_REPEAT | (width - x);
        out[n++] = 0;
    }
    return n;
}
