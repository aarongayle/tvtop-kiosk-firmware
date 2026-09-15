// TMDS (DVI) symbol generation for the stateless palette encoder.
//
// A data byte whose 9-bit intermediate word q_m has exactly four ones encodes to a symbol with
// zero disparity regardless of the encoder's running disparity. There are 52 such bytes. If every
// palette component is one of them, any pixel sequence is DC-balanced with no per-pixel state,
// which is what lets one Cortex-M0+ encode a full-resolution 1280-pixel line in time.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define TMDS_BALANCED_COUNT 52
extern const uint8_t tmds_balanced_levels[TMDS_BALANCED_COUNT];   // ascending

// Nearest balanced value to v (ties round up).
uint8_t tmds_nearest_balanced(uint8_t v);
bool tmds_is_balanced(uint8_t v);

// Encodes one data byte exactly as DVI 1.0 figure 3-5 with running disparity `*disparity`
// (updated). Bit 0 of the result is transmitted first. Used by tests and by tmds_symbol_balanced.
uint16_t tmds_encode_byte(uint8_t d, int *disparity);
// Symbol for a balanced value (asserts tmds_is_balanced(v)). Disparity-independent.
uint16_t tmds_symbol_balanced(uint8_t v);
// Decodes a 10-bit symbol back to its data byte (tests).
uint8_t tmds_decode_symbol(uint16_t sym);

// Word layout for DVI_SYMBOLS_PER_WORD == 2: the first pixel's symbol in bits 0..9, the second in
// bits 10..19 (PIO shifts right, LSB first).
static inline uint32_t tmds_pack2(uint16_t first, uint16_t second) { return (uint32_t)first | ((uint32_t)second << 10); }

// Control symbols (blanking) for reference; libdvi generates the blanking itself.
#define TMDS_CTRL_00 0x354u
#define TMDS_CTRL_01 0x0abu
#define TMDS_CTRL_10 0x154u
#define TMDS_CTRL_11 0x2abu
