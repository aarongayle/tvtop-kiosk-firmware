// Palette (see palette.h). Integer-only: it runs on core 0 at decode/render time but is called for
// every glyph edge blend, so it must stay cheap on an M0+.
#include "palette.h"
#include "tmds.h"
#include <string.h>

static uint16_t g_generation;

void palette_reset(palette_t *p) {
    memset(p, 0, sizeof *p);
    p->generation = ++g_generation;
}
void palette_init(palette_t *p) { palette_reset(p); }

uint8_t palette_quantize_component(uint8_t v) { return KIOSK_FULL_COLOUR ? v : tmds_nearest_balanced(v); }

uint32_t palette_quantize_rgb(uint32_t rgb) {
    if (KIOSK_FULL_COLOUR) return rgb & 0xffffffu;   // HSTX TMDS-encodes any value: nothing to round
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

// Two sources count as the same colour when no channel differs by more than this.
#define SAME_COLOUR 10

static bool close_colours(uint32_t a, uint32_t b) {
    for (int shift = 0; shift <= 16; shift += 8) {
        int d = (int)((a >> shift) & 0xff) - (int)((b >> shift) & 0xff);
        if (d > SAME_COLOUR || d < -SAME_COLOUR) return false;
    }
    return true;
}

// The balanced level next to `level`, one step towards `target`; `level` itself at either end.
static uint8_t step_level(uint8_t level, uint8_t target) {
    for (int k = 0; k < TMDS_BALANCED_COUNT; k++) {
        if (tmds_balanced_levels[k] != level) continue;
        if (target < level && k > 0) return tmds_balanced_levels[k - 1];
        if (target > level && k + 1 < TMDS_BALANCED_COUNT) return tmds_balanced_levels[k + 1];
        return level;
    }
    return level;
}

// The balanced levels are sparse near white (EE, EF, then nothing up to FF), so distinct light
// colours such as white #FFFFFF and a cream panel #F8F4EC quantise to the same entry and the panel
// vanishes. When a clearly different colour lands on an existing entry, move its most different
// channel one level towards its own value, so it stays visibly distinct (cream becomes #EFEFDD).
// Above 0xDD the only balanced levels are 0xEE and 0xEF, so every light colour lands on what looks
// like white: a cream panel (#F8F4EC) on a white page disappears. Whatever order colours arrive
// in, a colour whose quantised value is white-looking but whose source is clearly not white steps
// down one level: its lowest channel, or all three for a neutral grey so it gains no tint.
// Cream becomes #EFEFDD and #F2F2F2 becomes #DDDDDD; white itself is unchanged.
#define NEAR_WHITE_SOURCE 0xF5   // every channel at least this counts as white
static uint32_t separate_from_white(uint32_t rgb, uint32_t q) {
    int lo = 255, hi = 0, lo_shift = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
        int qc = (int)((q >> shift) & 0xff), sc = (int)((rgb >> shift) & 0xff);
        if (qc < 0xEE) return q;                       // not white-looking
        if (sc < lo) { lo = sc; lo_shift = shift; }
        if (sc > hi) hi = sc;
    }
    if (lo >= NEAR_WHITE_SOURCE) return q;               // it is white
    bool neutral = hi - lo <= 6;
    for (int shift = 0; shift <= 16; shift += 8) {
        if (!neutral && shift != lo_shift) continue;
        uint8_t level = (uint8_t)(q >> shift);
        uint8_t moved = step_level(level, 0);
        while (moved >= 0xEE && moved != step_level(moved, 0)) moved = step_level(moved, 0);   // EF -> EE -> DD
        q = (q & ~(0xffu << shift)) | ((uint32_t)moved << shift);
    }
    return q;
}

static uint32_t distinct_quantised(const palette_t *p, uint32_t rgb, uint32_t q, int *found) {
    if (KIOSK_FULL_COLOUR) { *found = find_exact(p, q); return q; }   // exact colours never collide
    q = separate_from_white(rgb, q);
    *found = find_exact(p, q);
    if (*found >= 0 && close_colours(p->src[*found], rgb)) return q;
    // A conflict is any entry that looks the same on screen (every quantised channel within 2) but
    // was added for a clearly different colour. Exact matches are only the obvious case: white is
    // #EFEFEF and cream #EFEFEE, which no one can tell apart.
    int conflict = -1;
    for (uint16_t k = 0; k < p->count; k++) {
        uint32_t e = p->rgb[k];
        bool looks_same = true;
        for (int shift = 0; shift <= 16 && looks_same; shift += 8) {
            int d = (int)((e >> shift) & 0xff) - (int)((q >> shift) & 0xff);
            if (d > 2 || d < -2) looks_same = false;
        }
        if (looks_same && !close_colours(p->src[k], rgb)) { conflict = k; break; }
    }
    if (conflict < 0) return q;
    uint32_t other = p->src[conflict];
    int best_shift = 0, best_d = -1;
    for (int shift = 0; shift <= 16; shift += 8) {
        int d = (int)((rgb >> shift) & 0xff) - (int)((other >> shift) & 0xff);
        if (d < 0) d = -d;
        if (d > best_d) { best_d = d; best_shift = shift; }
    }
    uint8_t level = (uint8_t)(q >> best_shift);
    uint8_t mine = (uint8_t)(rgb >> best_shift), theirs = (uint8_t)(other >> best_shift);
    // Step away from the other colour's side: a colour darker than its neighbour goes down a level.
    uint8_t moved = step_level(level, mine < theirs ? 0 : 255);
    if (moved == level) return q;
    uint32_t q2 = (q & ~(0xffu << best_shift)) | ((uint32_t)moved << best_shift);
    *found = find_exact(p, q2);
    return q2;
}

uint8_t palette_add(palette_t *p, uint32_t rgb) {
    rgb &= 0xffffffu;
    int i;
    uint32_t q = distinct_quantised(p, rgb, palette_quantize_rgb(rgb), &i);
    if (i >= 0) return (uint8_t)i;
    if (p->count < PALETTE_SIZE) {
        p->rgb[p->count] = q;
        p->src[p->count] = rgb;
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
