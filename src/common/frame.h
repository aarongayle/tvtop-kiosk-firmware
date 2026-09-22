// The dynamic frame in RAM, its streaming decoder, and the band renderer.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "kiosk_config.h"
#include "json.h"
#include "palette.h"
#include "geom.h"
#include "raster.h"

enum { OP_RECT = 'r', OP_LINE = 'l', OP_CIRCLE = 'c', OP_TEXT = 't', OP_BITMAP = 'b', OP_ICON = 'i', OP_CLIP = 'k', OP_USE = 'u', OP_NONE = 0 };

// 16 bytes. v[] meaning by kind (all device px8 unless noted):
//  rect:   x, y, w, h, radius            cidx/alpha = fill
//  line:   x1, y1, x2, y2, width         cidx/alpha = stroke
//  circle: cx, cy, r                     cidx/alpha = fill
//  text:   x, baseline, size_px(int), -, -   aux = align | (bold<<2); str = arena offset of [u16 len][utf8]
//  bitmap: x, y, size, modules, -        str = arena offset of [u16 len][row bytes]; cidx = colour
//  icon:   x, y, size                    aux = icon index
//  clip:   x0, y0, x1, y1 in whole px (exclusive), aux = 1 if clip set, 0 if cleared
//  use:    v[0] = def id, v[1] = paint index; placed (aux = 1): v[2], v[3] = x, y, v[4] = scale in
//          thousandths (1..RASTER_XF_S_MAX, a plain integer)
typedef struct { uint8_t kind, aux, cidx, alpha; int16_t v[5]; uint16_t str; } op_t;

typedef struct { uint8_t fill_idx, fill_alpha, stroke_idx, stroke_alpha; int16_t width8; } paint_t;   // alpha 0 = none

typedef struct {
    uint8_t version;
    uint16_t w, h;               // output size (device px)
    int32_t ox8, oy8;            // device px8 of the canvas origin (letterbox offset), for placed 'u' ops
    uint8_t bg_idx;
    uint16_t nops;
    op_t ops[KIOSK_MAX_OPS];
    uint16_t arena_len;
    uint8_t arena[KIOSK_ARENA_BYTES];
    uint16_t npaints;
    paint_t paints[KIOSK_MAX_PAINTS];
    char static_id[KIOSK_MAX_STATIC_ID];   // "" when the frame has no static set
    char next_url[KIOSK_MAX_URL];          // "" when null/absent
    uint32_t next_ms;
    uint32_t rev_lo;                       // low 32 bits of rev (diagnostics only)
    bool next_url_truncated;               // next_url did not fit KIOSK_MAX_URL (treat as unusable)
    uint32_t stats_dropped_ops, stats_dropped_bytes;
    uint32_t stats_dropped_defs;           // static defs that did not fit the recorder / store
} frame_t;

typedef enum {
    FD_OK = 0,
    FD_ERR_JSON,            // malformed JSON
    FD_ERR_VERSION,         // frame.v != KIOSK_PROTOCOL_VERSION
    FD_ERR_STATIC_MISSING,  // static.id given, no defs in this frame, and the store has a different set
    FD_ERR_STATIC_STORE,    // defs present but the store failed (too big / flash error)
    FD_ERR_TOO_MANY_OPS,    // hard failure only if the op table overflowed and ops were lost
} frame_status_t;

typedef struct {
    json_stream_t js;
    frame_t *frame;
    palette_t *pal;
    geom_store_t *geom;
    uint8_t *scratch; size_t scratch_len;   // path staging + geom index while decoding static
    uint16_t out_w, out_h;
    int32_t k_fx;             // scale 1280→out_w in 16.16
    int32_t ox8, oy8;         // letterbox offset, px8
    frame_status_t status;
    bool had_static_defs;     // this frame carried static.defs (store was (re)written)
    bool static_ready;        // geometry for frame->static_id is available
    uint32_t bytes_fed;
    // internal decoder state (op assembly, def staging) — private to frame.c, which checks at
    // compile time that its state fits. Aligned so the state struct can live here directly.
    _Alignas(8) uint8_t priv[768];
} frame_decoder_t;

// Prepares to decode one response body into `frame` for an out_w×out_h display. If the palette is
// nearly full it is reset here. `geom` is the store (may already hold a set; it is only rewritten
// when the body carries static.defs).
void frame_decoder_init(frame_decoder_t *d, frame_t *frame, palette_t *pal, geom_store_t *geom, uint8_t *scratch, size_t scratch_len, uint16_t out_w, uint16_t out_h);
bool frame_decoder_feed(frame_decoder_t *d, const char *data, size_t len);   // false = fatal, stop feeding
// Finishes decoding; returns the status. On FD_OK the frame is drawable (frame->static_id, if any,
// is open in `geom`).
frame_status_t frame_decoder_finish(frame_decoder_t *d);

// Rendering. sink receives each finished scanline as `width` palette indices in ascending y.
typedef void (*frame_line_sink_t)(void *ctx, uint16_t y, const uint8_t *pixels, uint16_t width);
typedef struct {
    uint32_t ms;               // filled by caller-provided clock if any (0 on host)
    uint32_t edge_visits, spans, ops_drawn, ops_skipped;
} render_stats_t;
// `extra`/nextra are overlay ops appended after the frame's ops (may be NULL/0).
void frame_render(const frame_t *f, palette_t *pal, const geom_store_t *geom, raster_t *r,
                  const op_t *extra, uint16_t nextra, frame_line_sink_t sink, void *ctx, render_stats_t *stats);

// Builds an op in device space from canvas-space integers using the same scaling as the decoder
// (for overlays built by the kiosk loop). Returns false if the op is not representable.
bool frame_make_overlay_icon(op_t *op, uint16_t out_w, uint16_t out_h, int x, int y, int size, int icon_index, uint8_t cidx);
bool frame_make_overlay_rect(op_t *op, uint16_t out_w, uint16_t out_h, int x, int y, int w, int h, int radius, uint8_t cidx, uint8_t alpha);
