// Palette (see palette.h). Integer-only: it runs on core 0 at decode/render time but is called for
// every glyph edge blend, so it must stay cheap on an M0+.
#include "palette.h"
#include "tmds.h"
#include <string.h>

void palette_reset(palette_t *p) { memset(p, 0, sizeof *p); }
void palette_init(palette_t *p) { palette_reset(p); }

uint8_t palette_quantize_component(uint8_t v) { return tmds_nearest_balanced(v); }

uint32_t palette_quantize_rgb(uint32_t rgb) {
    return ((uint32_t)tmds_nearest_balanced((uint8_t)(rgb >> 16)) << 16) |
           ((uint32_t)tmds_nearest_balanced((uint8_t)(rgb >> 8)) << 8) |
           tmds_nearest_balanced((uint8_t)rgb);
}

static int find_exact(const palette_t *p, uint32_t q) {
    for (uint16_t i = 0; i < p->count; i++)
        if (p->rgb[i] == q) return i;
    return -1;
}

int palette_find(const palette_t *p, uint32_t rgb) {
    return find_exact(p, palette_quantize_rgb(rgb & 0xffffffu));
}

uint8_t palette_add(palette_t *p, uint32_t rgb) {
    uint32_t q = palette_quantize_rgb(rgb & 0xffffffu);
    int i = find_exact(p, q);
    if (i >= 0) return (uint8_t)i;
    if (p->count < PALETTE_SIZE) {
        p->rgb[p->count] = q;
        return (uint8_t)p->count++;
    }
    // Full: nearest existing colour by squared RGB distance. Rare (the decoder resets the palette
    // at a frame start when it is nearly full), so a linear scan is fine.
    p->stats_overflow++;
    int r = (int)(q >> 16) & 0xff, g = (int)(q >> 8) & 0xff, b = (int)q & 0xff;
    uint32_t best_d = UINT32_MAX;
    uint8_t best = 0;
    for (uint16_t k = 0; k < PALETTE_SIZE; k++) {
        uint32_t c = p->rgb[k];
        int dr = r - ((int)(c >> 16) & 0xff), dg = g - ((int)(c >> 8) & 0xff), db = b - ((int)c & 0xff);
        uint32_t d = (uint32_t)(dr * dr + dg * dg + db * db);
        if (d < best_d) { best_d = d; best = (uint8_t)k; }
    }
    return best;
}

uint8_t palette_blend(palette_t *p, uint8_t bg, uint8_t fg, uint8_t level) {
    // 9 steps: 0, 32, ..., 224, 255 (nearest). The step index doubles as the mix weight /256.
    unsigned step = ((unsigned)level + 16) >> 5;
    if (step == 0 || bg == fg) return bg;
    if (step >= 8) return fg;
    uint8_t ql = (uint8_t)(step << 5);

    for (unsigned i = 0; i < p->blend_count; i++) {
        if (p->blend[i].bg == bg && p->blend[i].fg == fg && p->blend[i].level == ql) return p->blend[i].idx;
    }

    unsigned w = step << 5;                // fg weight out of 256
    uint32_t cb = p->rgb[bg], cf = p->rgb[fg];
    uint32_t mixed = 0;
    for (int shift = 16; shift >= 0; shift -= 8) {
        unsigned a = (cb >> shift) & 0xff, b = (cf >> shift) & 0xff;
        unsigned m = (a * (256 - w) + b * w + 128) >> 8;
        mixed |= (uint32_t)m << shift;
    }
    uint8_t idx = palette_add(p, mixed);

    // Ring replacement: cheap and good enough, since a frame's blends are dominated by a few
    // (text colour, background) pairs.
    p->blend[p->blend_next].bg = bg;
    p->blend[p->blend_next].fg = fg;
    p->blend[p->blend_next].level = ql;
    p->blend[p->blend_next].idx = idx;
    if (++p->blend_next >= PALETTE_BLEND_CACHE) p->blend_next = 0;
    if (p->blend_count < PALETTE_BLEND_CACHE) p->blend_count++;
    return idx;
}
