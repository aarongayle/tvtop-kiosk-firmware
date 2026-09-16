// Word-run (WR) scanlines: the line format core 1 turns into TMDS words.
//
// libdvi sends two 10-bit symbols per 32-bit word, so a scanline of width W is W/2 words per
// lane. Every palette component is a DC-balanced TMDS value, so a word depends only on the two
// palette indices it covers. Core 0, while rendering, converts each 8 bpp line into tokens over
// those words; core 1 only copies precomputed words, which is what makes dense text affordable
// inside a 31.8 us scanline on a Cortex-M0+.
//
// Token stream (little-endian u16):
//   RUN      bit15=0, bits14..9 = count-1 (1..64 words), bits8..0 = pair id
//   LITERAL  bit15=1, bits14..0 = n; followed by n u16 values, each (pair id << 4)
// Pair ids 0..255 are "both pixels palette index i"; 256..511 are mixed pairs allocated on demand.
// The table holds, per id, the word for lanes 0 (blue), 1 (green), 2 (red) and one pad word, so a
// literal's value is directly its byte offset into the table.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define WR_IDS 512
#define WR_PURE_IDS 256
#define WR_RUN_MAX 64
// A RUN token costs about as much as three literal words to decode, so shorter runs go in literals.
// Shortest run of identical words sent as a RUN token. A RUN costs 2 bytes and usually splits a
// literal block (another 2-byte header), so 3 words is where it starts saving memory. A detailed
// World map at 960x540 needs 83.7 KB of line pool at 6, 65.0 KB at 3.
#ifndef WR_RUN_MIN
#define WR_RUN_MIN 3
#endif
#define WR_SLOTS 1024

typedef struct {
    uint32_t table[WR_IDS * 4];  // read by core 1; an id's words are written before any line uses it
    uint16_t sym[3][256];        // per lane and palette index: the 10-bit balanced symbol
    uint16_t slot[WR_SLOTS];     // open-addressed hash of (a << 8 | b) -> id + 1; 0 = empty
    uint16_t key[WR_IDS];
    uint16_t next_id;            // next free mixed-pair id
    uint32_t stats_table_full;   // mixed words drawn as their left pixel because the table was full
} wr_ctx_t;

void wr_init(wr_ctx_t *c);                                  // every id black
void wr_set_colour(wr_ctx_t *c, uint8_t idx, uint32_t rgb); // palette entry idx (pure id idx)
void wr_reset_pairs(wr_ctx_t *c);                           // forget mixed pairs (after a palette reset)
// Converts `width` palette indices into tokens. Returns bytes written, or 0 if they exceed max.
uint16_t wr_line_from_pixels(wr_ctx_t *c, const uint8_t *px, uint16_t width, uint8_t *out, uint16_t max);
// Core 1: tokens -> three lane buffers laid out lane0 | lane1 | lane2, width/2 words each. Any
// token stream, however corrupt, writes exactly width/2 words per lane and reads only the table.
void wr_expand_line(const uint8_t *tok, uint32_t nbytes, uint32_t *out, uint32_t width, const uint32_t *table);
// Device builds with KIOSK_ASM_EXPANDER: wr_expand_line is the Thumb assembly version and this is
// the C reference it is checked against at boot (and the fallback if the check fails).
void wr_expand_line_c(const uint8_t *tok, uint32_t nbytes, uint32_t *out, uint32_t width, const uint32_t *table);
