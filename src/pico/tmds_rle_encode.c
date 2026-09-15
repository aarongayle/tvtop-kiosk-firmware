// Run-length spans → TMDS lane buffers (portable C reference; see scanout.h for the contract).
//
// Every palette entry's components are DC-balanced TMDS values, so a symbol never depends on the
// running disparity and a run of one colour is just the same 32-bit word (two symbols) repeated.
// The only work per span is the word straddling the span boundary when a span ends on an odd
// pixel: its low symbol belongs to the previous colour and its high symbol to the next one.
//
// Two facts keep the hot loop free of extra lookups and mask registers on Cortex-M0+:
//  * pair_lut[idx] = sym | sym << 10, so the low symbol is pair << 22 >> 22 and the high half is
//    pair >> 10 << 10 — the boundary word never needs sym_lut (the parameter stays for the asm
//    variant and for callers that want it; the C version ignores it).
//  * The three lanes are processed one after another for each span, so at most one lane's pair
//    and pointer are live at a time; the three pending low symbols are the only per-lane state.
//
// Robustness against a torn or corrupt line: runs are clamped to the pixels left on the line,
// a short span list is padded with entry 0 (black), an odd trailing byte is ignored. Output
// writes are therefore bounded by width/2 words per lane whatever the input.
//
// This file is compiled for the device (the function lives in SRAM: core 1 must never fetch
// from XIP while core 0 programs flash) and for the host tests (KIOSK_HOST).
#include <stdint.h>

#ifdef KIOSK_HOST
#define KIOSK_RAM_FUNC(f) f
void tmds_rle_encode_line(const uint8_t *spans, uint32_t nbytes, uint32_t *out, uint32_t width,
                          const uint32_t *pair_lut, const uint16_t *sym_lut);
#else
#include "pico.h"
#include "scanout.h"
#define KIOSK_RAM_FUNC(f) __not_in_flash_func(f)
#endif

#ifdef KIOSK_ASM_ENCODER
// The assembly implementation (tmds_rle_encode_asm.S) provides tmds_rle_encode_line; the C
// version is kept under another name as the reference the asm is checked against.
#define ENCODER_NAME tmds_rle_encode_line_c
void tmds_rle_encode_line_c(const uint8_t *spans, uint32_t nbytes, uint32_t *out, uint32_t width,
                            const uint32_t *pair_lut, const uint16_t *sym_lut);
#else
#define ENCODER_NAME tmds_rle_encode_line
#endif

#define SYM_MASK 0x3ffu
#define HI_MASK  0xffc00u

// Writes n copies of p starting at o. Runs of 8+ pixels come through here with n >= 4; the
// 4-wide body is what GCC turns into four back-to-back stores per branch on M0+.
static inline void fill_words(uint32_t *o, uint32_t n, uint32_t p) {
    while (n >= 4) {
        o[0] = p; o[1] = p; o[2] = p; o[3] = p;
        o += 4;
        n -= 4;
    }
    while (n) {
        *o++ = p;
        n--;
    }
}

void KIOSK_RAM_FUNC(ENCODER_NAME)(const uint8_t *spans, uint32_t nbytes, uint32_t *out, uint32_t width,
                                  const uint32_t *pair_lut, const uint16_t *sym_lut) {
    (void)sym_lut;
    const uint32_t stride = width >> 1;              // words per lane
    const uint8_t *end = spans + (nbytes & ~1u);
    uint32_t *o = out;                               // lane-0 word being written; lanes 1/2 at +stride
    uint32_t remaining = width;
    uint32_t pend0 = 0, pend1 = 0, pend2 = 0;        // low symbols of a half-written word
    uint32_t phase = 0;                              // 1: o[] holds a pending low symbol

    for (;;) {
        uint32_t idx, run;
        if (spans < end) {
            idx = spans[0];
            run = (uint32_t)spans[1] + 1u;
            spans += 2;
            if (run > remaining) run = remaining;
        } else if (remaining) {
            idx = 0;                                 // pad a short line with black
            run = remaining;
        } else {
            break;
        }
        remaining -= run;
        if (run == 0) break;                         // only when the line was already full

        uint32_t p0 = pair_lut[idx];
        uint32_t p1 = pair_lut[256 + idx];
        uint32_t p2 = pair_lut[512 + idx];

        if (phase) {
            o[0] = pend0 | (p0 & HI_MASK);
            o[stride] = pend1 | (p1 & HI_MASK);
            o[2 * stride] = pend2 | (p2 & HI_MASK);
            o++;
            phase = 0;
            if (--run == 0) continue;
        }

        uint32_t nw = run >> 1;
        if (run & 1u) {
            pend0 = p0 & SYM_MASK;
            pend1 = p1 & SYM_MASK;
            pend2 = p2 & SYM_MASK;
            phase = 1;
        }
        if (nw == 0) continue;                       // 1-pixel run at an even pixel: nothing to store

        if (nw < 4) {
            // Short runs (2..7 px): straight-line stores, no loop set-up.
            uint32_t *o1 = o + stride, *o2 = o1 + stride;
            o[0] = p0; o1[0] = p1; o2[0] = p2;
            if (nw > 1) { o[1] = p0; o1[1] = p1; o2[1] = p2; }
            if (nw > 2) { o[2] = p0; o1[2] = p1; o2[2] = p2; }
        } else {
            fill_words(o, nw, p0);
            fill_words(o + stride, nw, p1);
            fill_words(o + 2 * stride, nw, p2);
        }
        o += nw;
    }

    // A line whose span bytes summed to an odd count short of the width was padded above, so
    // phase is always 0 here when width is even; the guard only matters for a bad width.
    if (phase) {
        o[0] = pend0;
        o[stride] = pend1;
        o[2 * stride] = pend2;
    }
}
