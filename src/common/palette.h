// Palette: colours actually used by frames, quantised to TMDS-balanced component levels.
//
// Append-only across frames so lines of the previous frame still on screen keep their colours
// during the top-to-bottom wipe. Blends (alpha fills, anti-aliased glyph edges) are palette
// entries too, allocated on demand through a small cache.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "kiosk_config.h"

#define PALETTE_SIZE 256
#define PALETTE_RESET_THRESHOLD 208   // frame decoder resets when count exceeds this at frame start
#define PALETTE_BLEND_CACHE 96

typedef struct {
    uint32_t rgb[PALETTE_SIZE];     // 0x00RRGGBB, components already balanced
    uint16_t count;
    uint16_t dirty_from;            // first index not yet uploaded to the scanout LUT
    struct { uint8_t bg, fg, level, idx; } blend[PALETTE_BLEND_CACHE];
    uint8_t blend_count, blend_next;
    uint32_t stats_overflow;        // colours mapped to a nearest neighbour because the palette was full
    uint32_t src[PALETTE_SIZE];     // the colour each entry was first added for, before quantising
    uint16_t generation;            // changes on every reset: indices from before now mean other colours
} palette_t;

void palette_init(palette_t *p);
void palette_reset(palette_t *p);                     // count=0, cache cleared, dirty_from=0

uint8_t palette_quantize_component(uint8_t v);       // = tmds_nearest_balanced
uint32_t palette_quantize_rgb(uint32_t rgb);

// Find-or-add. Never fails: when the palette is full the nearest existing colour is returned.
uint8_t palette_add(palette_t *p, uint32_t rgb);
// -1 if the exact (quantised) colour is absent.
int palette_find(const palette_t *p, uint32_t rgb);
// Mix `level`/255 of fg over bg (level is quantised to 1/8 steps internally: 0 → bg, 255 → fg).
uint8_t palette_blend(palette_t *p, uint8_t bg, uint8_t fg, uint8_t level);

static inline uint32_t palette_rgb(const palette_t *p, uint8_t idx) { return p->rgb[idx]; }
