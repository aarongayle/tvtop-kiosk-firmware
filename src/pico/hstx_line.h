// Scanlines as HSTX command streams (RP2350 Pico 2 W).
//
// The RP2350's HSTX peripheral has a command expander with a hardware TMDS encoder: a command word
// (type in bits 15..12, count in bits 11..0) is followed by the data it consumes. TMDS_REPEAT sends
// one 0x00RRGGBB pixel `count` times, TMDS sends `count` pixels, RAW_REPEAT sends one 30-bit word of
// control symbols `count` times. So a line from the line pool, (palette index, run-1) byte pairs,
// maps almost one to one onto commands: a run becomes two words, however long it is.
//
// Portable C with no tables, so core 1 can build lines from RAM on the device and the host tests
// can check every word.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define HSTX_CMD_RAW         (0x0u << 12)
#define HSTX_CMD_RAW_REPEAT  (0x1u << 12)
#define HSTX_CMD_TMDS        (0x2u << 12)
#define HSTX_CMD_TMDS_REPEAT (0x3u << 12)
#define HSTX_CMD_NOP         (0xfu << 12)
#define HSTX_MAX_COUNT       0xfffu

// TMDS control symbols for (vsync level, hsync level) on the wire.
#define HSTX_TMDS_CTRL_00 0x354u
#define HSTX_TMDS_CTRL_01 0x0abu
#define HSTX_TMDS_CTRL_10 0x154u
#define HSTX_TMDS_CTRL_11 0x2abu

typedef struct {
    uint16_t h_front, h_sync, h_back, h_active;
    uint16_t v_front, v_sync, v_back, v_active;
    bool h_positive, v_positive;   // sync pulses are high on the wire
} hstx_timing_t;

// The raw word for one blanking symbol: lane 0 carries the sync levels, lanes 1 and 2 CTRL_00.
// vsync/hsync say whether the pulse is asserted; polarity decides the wire level.
uint32_t hstx_sync_word(const hstx_timing_t *t, bool vsync, bool hsync);

// The six words that open every line: front porch, hsync, back porch.
void hstx_line_prefix(const hstx_timing_t *t, bool vsync, uint32_t prefix[6]);

// A whole blanking line (the prefix with the back porch extended over the active width). 6 words.
uint32_t hstx_blank_line(const hstx_timing_t *t, bool vsync, uint32_t *out);

// Packed runs: the line-pool format on the RP2350. Each run is four bytes, little-endian
// 0xLLRRGGBB: LL is the run length minus one (so 1..256 pixels) and RRGGBB its exact colour. Core 0
// packs them while rendering (the palette lookup happens there, once); core 1 then turns each run into
// two HSTX words with a shift and a mask, which keeps up with the densest 720p line.
uint16_t hstx_pack_line(const uint8_t *px, uint16_t width, const uint32_t *rgb, uint8_t *out, uint16_t max);
uint32_t hstx_active_line_packed(const hstx_timing_t *t, const uint32_t prefix[6], const uint8_t *runs, uint32_t len,
                                 uint32_t *out, uint32_t max);

// An active line: `prefix` (from hstx_line_prefix) then TMDS commands for (index, run-1) spans,
// colours looked up in rgb[256]. Runs of two or more pixels become TMDS_REPEAT; consecutive single
// pixels are grouped into one TMDS command. Spans past the active width are clipped and a short
// line is padded with black. Returns words written, or 0 if they would exceed max.
uint32_t hstx_active_line(const hstx_timing_t *t, const uint32_t prefix[6], const uint8_t *spans, uint32_t len,
                          const uint32_t *rgb, uint32_t *out, uint32_t max);
