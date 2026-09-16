// See tmds_wr.h for the format. Compiled for the device (wr_expand_line lives in SRAM so core 1
// never touches flash) and for the host tests (KIOSK_HOST).
#include <string.h>
#include "tmds.h"
#include "tmds_wr.h"

#ifdef KIOSK_HOST
#define KIOSK_RAM_FUNC(f) f
#else
#include "pico.h"
#define KIOSK_RAM_FUNC(f) __not_in_flash_func(f)
#endif

static void set_word(wr_ctx_t *c, uint16_t id, uint8_t a, uint8_t b) {
    uint32_t *e = c->table + (uint32_t)id * 4u;
    for (int lane = 0; lane < 3; lane++) e[lane] = (uint32_t)c->sym[lane][a] | (uint32_t)c->sym[lane][b] << 10;
    e[3] = 0;
}

void wr_init(wr_ctx_t *c) {
    memset(c, 0, sizeof *c);
    uint16_t black = tmds_symbol_balanced(tmds_nearest_balanced(0));
    for (int lane = 0; lane < 3; lane++)
        for (int i = 0; i < 256; i++) c->sym[lane][i] = black;
    for (uint16_t id = 0; id < WR_IDS; id++) set_word(c, id, 0, 0);
    c->next_id = WR_PURE_IDS;
}

void wr_reset_pairs(wr_ctx_t *c) {
    memset(c->slot, 0, sizeof c->slot);
    c->next_id = WR_PURE_IDS;
}

void wr_set_colour(wr_ctx_t *c, uint8_t idx, uint32_t rgb) {
    uint8_t comp[3] = { (uint8_t)rgb, (uint8_t)(rgb >> 8), (uint8_t)(rgb >> 16) };   // lane order b, g, r
    for (int lane = 0; lane < 3; lane++) c->sym[lane][idx] = tmds_symbol_balanced(tmds_nearest_balanced(comp[lane]));
    set_word(c, idx, idx, idx);
}

static uint16_t pair_id(wr_ctx_t *c, uint8_t a, uint8_t b) {
    if (a == b) return a;
    uint16_t k = (uint16_t)(a << 8 | b);
    uint32_t h = ((uint32_t)k * 40503u >> 6) & (WR_SLOTS - 1u);
    for (;;) {   // at most 256 mixed ids in 1024 slots, so an empty slot is always reached
        uint16_t s = c->slot[h];
        if (!s) break;
        if (c->key[s - 1] == k) return (uint16_t)(s - 1);
        h = (h + 1u) & (WR_SLOTS - 1u);
    }
    if (c->next_id >= WR_IDS) { c->stats_table_full++; return a; }
    uint16_t id = c->next_id++;
    c->key[id] = k;
    set_word(c, id, a, b);   // complete before the id can appear in any published line
    c->slot[h] = (uint16_t)(id + 1);
    return id;
}

uint16_t wr_line_from_pixels(wr_ctx_t *c, const uint8_t *px, uint16_t width, uint8_t *out, uint16_t max) {
    const uint32_t words = width / 2u;
    uint32_t n = 0, i = 0;
#define PUT16(v) do { uint32_t v_ = (v); if (n + 2u > max) return 0; out[n] = (uint8_t)v_; out[n + 1] = (uint8_t)(v_ >> 8); n += 2u; } while (0)
    while (i < words) {
        uint8_t a = px[2 * i], b = px[2 * i + 1];
        if (a == b) {
            uint32_t j = i + 1;
            while (j < words && px[2 * j] == a && px[2 * j + 1] == a) j++;
            if (j - i >= WR_RUN_MIN) {
                for (uint32_t len = j - i; len;) {
                    uint32_t take = len > WR_RUN_MAX ? WR_RUN_MAX : len;
                    PUT16((take - 1u) << 9 | a);
                    len -= take;
                }
                i = j;
                continue;
            }
        }
        // Literal block: everything up to the next run of at least WR_RUN_MIN identical plain words.
        uint32_t start = i;
        while (i < words) {
            uint8_t x = px[2 * i];
            if (x == px[2 * i + 1]) {
                uint32_t j = i + 1;
                while (j < words && j - i < WR_RUN_MIN && px[2 * j] == x && px[2 * j + 1] == x) j++;
                if (j - i >= WR_RUN_MIN) break;
            }
            i++;
        }
        uint32_t cnt = i - start;
        if (cnt == 1) {
            PUT16(pair_id(c, px[2 * start], px[2 * start + 1]));   // a RUN of one word is one token
        } else {
            PUT16(0x8000u | cnt);
            for (uint32_t k = start; k < i; k++) PUT16((uint32_t)pair_id(c, px[2 * k], px[2 * k + 1]) << 4);
        }
    }
#undef PUT16
    return (uint16_t)n;
}

#if !defined(KIOSK_HOST) && defined(KIOSK_ASM_EXPANDER) && KIOSK_ASM_EXPANDER
#define WR_EXPAND_C wr_expand_line_c   // tmds_wr_expand_m0.S provides wr_expand_line
#else
#define WR_EXPAND_C wr_expand_line
#endif

void KIOSK_RAM_FUNC(WR_EXPAND_C)(const uint8_t *tok, uint32_t nbytes, uint32_t *out, uint32_t width, const uint32_t *table) {
    const uint32_t stride = width >> 1;
    uint32_t *o0 = out, *o1 = out + stride, *o2 = out + 2 * stride;
    const uint32_t *const end0 = out + stride;
    const uint8_t *p = tok, *const pend = tok + (nbytes & ~1u);
    const uint8_t *const tbytes = (const uint8_t *)table;
    while (p < pend && o0 < end0) {
        uint32_t t = (uint32_t)p[0] | (uint32_t)p[1] << 8;
        p += 2;
        uint32_t room = (uint32_t)(end0 - o0);
        if (t & 0x8000u) {
            uint32_t cnt = t & 0x7fffu;
            uint32_t avail = (uint32_t)(pend - p) >> 1;
            if (cnt > avail) cnt = avail;
            if (cnt > room) cnt = room;
            const uint32_t *stop = o0 + cnt;
            while (o0 < stop) {
                const uint32_t *e = (const uint32_t *)(void *)(tbytes + (((uint32_t)p[0] | (uint32_t)p[1] << 8) & 0x1ff0u));
                p += 2;
                *o0++ = e[0];
                *o1++ = e[1];
                *o2++ = e[2];
            }
        } else {
            const uint32_t *e = table + ((t & 0x1ffu) << 2);
            uint32_t cnt = ((t >> 9) & 63u) + 1u;
            if (cnt > room) cnt = room;
            uint32_t w0 = e[0], w1 = e[1], w2 = e[2];
            const uint32_t *stop = o0 + cnt;
            while (o0 + 4 <= stop) { o0[0] = w0; o0[1] = w0; o0[2] = w0; o0[3] = w0; o0 += 4; }
            while (o0 < stop) *o0++ = w0;
            stop = o1 + cnt;
            while (o1 + 4 <= stop) { o1[0] = w1; o1[1] = w1; o1[2] = w1; o1[3] = w1; o1 += 4; }
            while (o1 < stop) *o1++ = w1;
            stop = o2 + cnt;
            while (o2 + 4 <= stop) { o2[0] = w2; o2[1] = w2; o2[2] = w2; o2[3] = w2; o2 += 4; }
            while (o2 < stop) *o2++ = w2;
        }
    }
    while (o0 < end0) { *o0++ = table[0]; *o1++ = table[1]; *o2++ = table[2]; }   // short line: pad with id 0
}
