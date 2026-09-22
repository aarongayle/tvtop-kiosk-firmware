// Frame decoder and band renderer (see frame.h for the data model, docs/RENDERING.md for the
// interpretation of every op).
//
// The decoder is a json.h event consumer. It navigates the document by (depth, key, index) and
// keeps a small explicit "section" state so unknown keys and stray containers are skipped without
// recursion. Each op array is staged element by element and validated once on its closing bracket;
// strings that go into the arena (text, decoded bitmaps) are appended as they stream and rolled
// back when the op is dropped. Static defs are recorded (path.h recorder, in the scratch area) and
// flattened into the geometry store on their closing bracket, because the transform arrives after
// the path text.
#include <stddef.h>
#include <string.h>
#include "frame.h"
#include "colour.h"
#include "font.h"
#include "icons.h"
#include "path.h"

// Arena offsets are uint16 in op_t, so the usable arena is capped below 64 KB on the host build.
#define ARENA_CAP ((KIOSK_ARENA_BYTES) > 65535u ? 65535u : (uint32_t)(KIOSK_ARENA_BYTES))
#define SMALL_CAP 40          // colour strings, icon names, def ids, def kinds ("url(#hatch-pattern-undefined)" is 29)
#define OP_MAX_ELEMS 8
#define BITMAP_MAX_MODULES 177   // QR version 40
#define ICON_TOL 0.15f
#define ICON_VERT_MAX 512        // int16 pairs; a size-132 dice is ~350

enum { SEC_TOP = 0, SEC_OPS, SEC_STATIC, SEC_DEFS, SEC_PAINTS };
enum { ET_NONE = 0, ET_NUM, ET_STR, ET_NULL, ET_BAD };
enum { DEF_NONE = 0, DEF_PATH, DEF_POLY, DEF_CIRCLE, DEF_GROUP, DEF_UNKNOWN };

typedef struct { char s[SMALL_CAP]; uint8_t len; bool over; int8_t idx; } small_t;

typedef struct {
    uint8_t sec;
    bool top_ok;                 // top-level value is an object
    bool have_v; int32_t v;
    uint16_t canvas_w, canvas_h; // from the frame's w/h; 0 = protocol default (1280×720)
    small_t sb;                 // bg (top level) and paint fill
    small_t sc;                  // op small string #2 and paint stroke
    small_t sa;                  // op small string #1
    small_t kind;                // def element 0
    uint8_t static_id_len;
    uint16_t next_url_len;
    // op assembly
    bool op_active, op_bad;
    uint8_t op_kind, op_kind_len;
    uint8_t etype[OP_MAX_ELEMS];
    int32_t e8[OP_MAX_ELEMS];    // canvas px8 of numeric elements
    uint16_t arena_start;        // arena_len when the op began (rollback point; also op.str)
    uint16_t big_len;            // payload bytes of the arena string of this op
    bool big_active, big_over;
    uint32_t b64_acc; uint8_t b64_bits; bool b64_end, b64_bad;
    uint32_t overflow_drops;     // ops lost to a full op table (→ FD_ERR_TOO_MANY_OPS)
    // static defs
    bool geom_begun, geom_ok;
    bool def_active, def_bad, def_id_ok;
    uint8_t def_kind;
    uint16_t def_id;
    float def_f[4]; bool def_has[4];
    path_recorder_t rec;
    // group defs: members are staged in the recorder scratch as {id, paint} uint16 pairs
    bool grp_list, grp_member, grp_mbad;
    uint8_t grp_n;
    int32_t grp_paint;
    bool paint_active;
} priv_t;

_Static_assert(sizeof(priv_t) <= sizeof(((frame_decoder_t *)0)->priv), "frame_decoder_t.priv is too small");
_Static_assert(KIOSK_MAX_OPS <= 65535 && KIOSK_MAX_PAINTS <= 32767 && KIOSK_MAX_DEFS <= 32767, "table sizes must fit their index types");

// ---------------------------------------------------------------------------------------------
// Scaling (shared by the decoder and the overlay helpers)

typedef struct { int32_t k_fx, ox8, oy8; } scale_t;

// k = min(out_w/canvas_w, out_h/canvas_h) so the whole canvas always fits; the unused strip (if the
// mode's shape differs) is split evenly into an integer px8 offset. The canvas is whatever the frame
// declares in w/h (1920×1080 from the server, so a 1080p mode draws 1:1 and 960×540 exactly 1:2);
// frames without them, and the built-in screens, are 1280×720.
static void scale_setup(uint16_t out_w, uint16_t out_h, uint16_t canvas_w, uint16_t canvas_h, scale_t *s) {
    if (out_w == 0 || out_h == 0 || canvas_w == 0 || canvas_h == 0) { s->k_fx = 1 << 16; s->ox8 = s->oy8 = 0; return; }
    int32_t kx = (int32_t)(((int64_t)out_w << 16) / canvas_w);
    int32_t ky = (int32_t)(((int64_t)out_h << 16) / canvas_h);
    s->k_fx = kx < ky ? kx : ky;
    int32_t cw8 = (int32_t)(((int64_t)canvas_w * PX8_ONE * s->k_fx + 0x8000) >> 16);
    int32_t ch8 = (int32_t)(((int64_t)canvas_h * PX8_ONE * s->k_fx + 0x8000) >> 16);
    s->ox8 = ((int32_t)out_w * PX8_ONE - cw8) / 2;
    s->oy8 = ((int32_t)out_h * PX8_ONE - ch8) / 2;
}

static int32_t clamp_px8(int64_t v) {
    if (v > PX8_MAX) return PX8_MAX;
    if (v < -PX8_MAX) return -PX8_MAX;
    return (int32_t)v;
}

// Canvas px8 → device px8 (lengths pass offset8 = 0). int64 so saturated wire values cannot wrap.
static int32_t scale_px8(const scale_t *s, int64_t c8, int32_t offset8) {
    return clamp_px8(((c8 * s->k_fx + 0x8000) >> 16) + offset8);
}

// Canvas px8 → device whole px (clip rects), rounding the device position.
static int32_t scale_px(const scale_t *s, int64_t c8, int32_t offset8) {
    return (scale_px8(s, c8, offset8) + PX8_HALF) >> PX8_SHIFT;
}

// Float canvas px → canvas px8, clamped so the cast is defined for any float (defs carry floats).
static int32_t float_to_px8(float v) {
    float t = v * (float)PX8_ONE;
    if (!(t == t)) return 0;
    if (t > 1.0e8f) t = 1.0e8f;
    if (t < -1.0e8f) t = -1.0e8f;
    return (int32_t)(t + (t >= 0 ? 0.5f : -0.5f));
}

static int32_t px8_to_int(int32_t v8) { return (v8 + PX8_HALF) >> PX8_SHIFT; }

// ---------------------------------------------------------------------------------------------
// Small helpers

static void small_reset(small_t *s, int idx) { s->len = 0; s->over = false; s->idx = (int8_t)idx; s->s[0] = 0; }

static void small_append(small_t *s, const char *data, size_t n) {
    size_t room = SMALL_CAP - 1 - s->len;
    if (n > room) { n = room; s->over = true; }
    memcpy(s->s + s->len, data, n);
    s->len = (uint8_t)(s->len + n);
    s->s[s->len] = 0;
}

static bool small_eq(const small_t *s, const char *lit) { return !s->over && strcmp(s->s, lit) == 0; }

static bool key_is(const json_stream_t *js, int level, const char *lit) { return strcmp(js->key[level], lit) == 0; }

// Def ids are base-36 ordinals ("0".."zz"); anything else, or one past the table, is invalid.
static int parse_base36(const char *s, size_t len) {
    if (len == 0 || len > 4) return -1;
    int v = 0;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        int dgt;
        if (c >= '0' && c <= '9') dgt = c - '0';
        else if (c >= 'a' && c <= 'z') dgt = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z') dgt = c - 'A' + 10;
        else return -1;
        v = v * 36 + dgt;
    }
    return v < KIOSK_MAX_DEFS ? v : -1;
}

static int b64val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

// ---------------------------------------------------------------------------------------------
// Arena strings

static void big_begin(frame_decoder_t *d, priv_t *p) {
    frame_t *f = d->frame;
    p->big_len = 0; p->big_over = false; p->big_active = true;
    if ((uint32_t)f->arena_len + 2 > ARENA_CAP) { p->big_over = true; p->op_bad = true; return; }
    f->arena_len = (uint16_t)(f->arena_len + 2);   // length header, filled in by big_end
}

static void big_append(frame_decoder_t *d, priv_t *p, const char *data, size_t n) {
    frame_t *f = d->frame;
    if (!p->big_active) return;
    uint32_t room = ARENA_CAP - f->arena_len;
    if (n > room) { f->stats_dropped_bytes += (uint32_t)(n - room); n = room; p->big_over = true; }
    if (n) {
        memcpy(f->arena + f->arena_len, data, n);
        f->arena_len = (uint16_t)(f->arena_len + n);
        p->big_len = (uint16_t)(p->big_len + n);
    }
}

static void big_end(frame_decoder_t *d, priv_t *p) {
    frame_t *f = d->frame;
    if (!p->big_active) return;
    p->big_active = false;
    if ((uint32_t)p->arena_start + 2 <= ARENA_CAP) {
        f->arena[p->arena_start] = (uint8_t)(p->big_len & 0xff);
        f->arena[p->arena_start + 1] = (uint8_t)(p->big_len >> 8);
    }
}

static void b64_feed(frame_decoder_t *d, priv_t *p, const char *data, size_t n) {
    for (size_t i = 0; i < n && !p->b64_bad; i++) {
        char c = data[i];
        if (c == '=') { p->b64_end = true; continue; }
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        int v = b64val(c);
        if (v < 0 || p->b64_end) { p->b64_bad = true; break; }
        p->b64_acc = (p->b64_acc << 6) | (uint32_t)v;
        p->b64_bits = (uint8_t)(p->b64_bits + 6);
        if (p->b64_bits >= 8) {
            p->b64_bits = (uint8_t)(p->b64_bits - 8);
            char byte = (char)((p->b64_acc >> p->b64_bits) & 0xff);
            if (p->big_len >= KIOSK_MAX_BITMAP_BYTES) { p->b64_bad = true; break; }
            big_append(d, p, &byte, 1);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Op assembly

static void op_begin(frame_decoder_t *d, priv_t *p) {
    p->op_active = true; p->op_bad = false;
    p->op_kind = 0; p->op_kind_len = 0;
    memset(p->etype, 0, sizeof p->etype);
    small_reset(&p->sa, -1); small_reset(&p->sc, -1);
    p->arena_start = d->frame->arena_len;
    p->big_active = false; p->big_over = false; p->big_len = 0;
    p->b64_acc = 0; p->b64_bits = 0; p->b64_end = false; p->b64_bad = false;
}

// Picks the staging slot for a small string element (two per op suffice for every op form).
static small_t *op_small_slot(priv_t *p, int idx) {
    if (p->sa.idx == idx) return &p->sa;
    if (p->sc.idx == idx) return &p->sc;
    if (p->sa.idx < 0) { small_reset(&p->sa, idx); return &p->sa; }
    if (p->sc.idx < 0) { small_reset(&p->sc, idx); return &p->sc; }
    return NULL;
}

static const small_t *op_small_get(const priv_t *p, int idx) {
    if (p->sa.idx == idx) return &p->sa;
    if (p->sc.idx == idx) return &p->sc;
    return NULL;
}

static bool op_is_big_string(uint8_t kind, int idx) {
    return (kind == OP_TEXT && idx == 3) || (kind == OP_BITMAP && idx == 5);
}

static void op_element(frame_decoder_t *d, priv_t *p, const json_stream_t *js, json_event_t ev, const char *data, size_t len, bool final) {
    int idx = js->index[2];
    if (idx >= OP_MAX_ELEMS) { p->op_bad = true; return; }
    switch (ev) {
    case JSON_EV_STRING:
        if (idx == 0) {
            if (len) { if (p->op_kind_len == 0) p->op_kind = (uint8_t)data[0]; p->op_kind_len = (uint8_t)(p->op_kind_len + (len > 200 ? 200 : len)); }
            if (final && p->op_kind_len != 1) p->op_bad = true;
            return;
        }
        p->etype[idx] = ET_STR;
        if (op_is_big_string(p->op_kind, idx)) {
            if (!p->big_active && p->big_len == 0 && !p->big_over) big_begin(d, p);
            if (p->op_kind == OP_BITMAP) b64_feed(d, p, data, len); else big_append(d, p, data, len);
            if (final) big_end(d, p);
        } else {
            small_t *s = op_small_slot(p, idx);
            if (!s) { p->op_bad = true; return; }
            small_append(s, data, len);
        }
        return;
    case JSON_EV_NUMBER: {
        int32_t v;
        if (p->op_kind == OP_TEXT && idx == 3) {
            // A numeric text value (a bare score) is drawn as its decimal text, like the browser.
            p->etype[idx] = ET_STR;
            big_begin(d, p); big_append(d, p, data, len); big_end(d, p);
            return;
        }
        if (!json_number_to_fixed(data, len, PX8_SHIFT, &v)) { p->etype[idx] = ET_BAD; p->op_bad = true; return; }
        p->etype[idx] = ET_NUM; p->e8[idx] = v;
        return;
    }
    case JSON_EV_TRUE: p->etype[idx] = ET_NUM; p->e8[idx] = PX8_ONE; return;
    case JSON_EV_FALSE: p->etype[idx] = ET_NUM; p->e8[idx] = 0; return;
    case JSON_EV_NULL:
        p->etype[idx] = ET_NULL;
        if (op_is_big_string(p->op_kind, idx)) { p->etype[idx] = ET_STR; big_begin(d, p); big_end(d, p); }
        return;
    default:   // nested container where a scalar belongs
        p->op_bad = true;
        return;
    }
}

static bool op_num(const priv_t *p, int idx, int32_t *c8) {
    if (p->etype[idx] != ET_NUM) return false;
    *c8 = p->e8[idx];
    return true;
}

static int32_t op_int(const priv_t *p, int idx, int32_t dflt) {
    return p->etype[idx] == ET_NUM ? px8_to_int(p->e8[idx]) : dflt;
}

// 0 = missing/unparseable, 1 = colour, 2 = JSON null ("none").
static int op_colour(const priv_t *p, int idx, rgba8_t *out) {
    if (p->etype[idx] == ET_NULL) return 2;
    const small_t *s = op_small_get(p, idx);
    if (!s || s->over || p->etype[idx] != ET_STR) return 0;
    return colour_parse(s->s, s->len, out) ? 1 : 0;
}

// Resolves a colour element into palette index + alpha. Null means draw nothing (alpha 0), like a
// missing SVG attribute would for a stroke; an unparseable string fails (the op is dropped).
static bool op_paint(frame_decoder_t *d, const priv_t *p, int idx, bool required, uint32_t dflt_rgb, uint8_t *cidx, uint8_t *alpha) {
    rgba8_t c;
    int r = op_colour(p, idx, &c);
    if (r == 0) {
        if (p->etype[idx] == ET_NONE && !required) { *cidx = palette_add(d->pal, dflt_rgb); *alpha = 255; return true; }
        return false;
    }
    if (r == 2) { *cidx = d->frame->bg_idx; *alpha = 0; return true; }
    *cidx = palette_add(d->pal, rgba8_to_rgb(c));
    *alpha = c.a;
    return true;
}

static void op_end(frame_decoder_t *d, priv_t *p) {
    frame_t *f = d->frame;
    if (!p->op_active) return;
    p->op_active = false;
    if (p->big_active) big_end(d, p);
    scale_t s = { d->k_fx, d->ox8, d->oy8 };
    op_t op;
    memset(&op, 0, sizeof op);
    bool ok = !p->op_bad;
    int32_t a = 0, b = 0, c = 0, e = 0;
    if (ok) {
        switch (p->op_kind) {
        case OP_RECT:
            ok = op_num(p, 1, &a) && op_num(p, 2, &b) && op_num(p, 3, &c) && op_num(p, 4, &e)
                 && op_paint(d, p, 5, true, 0, &op.cidx, &op.alpha);
            if (ok) {
                int32_t x0 = scale_px8(&s, a, s.ox8), y0 = scale_px8(&s, b, s.oy8);
                op.v[0] = (int16_t)x0; op.v[1] = (int16_t)y0;
                op.v[2] = (int16_t)(scale_px8(&s, (int64_t)a + c, s.ox8) - x0);
                op.v[3] = (int16_t)(scale_px8(&s, (int64_t)b + e, s.oy8) - y0);
                op.v[4] = (int16_t)scale_px8(&s, p->etype[6] == ET_NUM ? p->e8[6] : 0, 0);
            }
            break;
        case OP_LINE:
            ok = op_num(p, 1, &a) && op_num(p, 2, &b) && op_num(p, 3, &c) && op_num(p, 4, &e)
                 && op_paint(d, p, 5, true, 0, &op.cidx, &op.alpha);
            if (ok) {
                op.v[0] = (int16_t)scale_px8(&s, a, s.ox8); op.v[1] = (int16_t)scale_px8(&s, b, s.oy8);
                op.v[2] = (int16_t)scale_px8(&s, c, s.ox8); op.v[3] = (int16_t)scale_px8(&s, e, s.oy8);
                op.v[4] = (int16_t)scale_px8(&s, p->etype[6] == ET_NUM ? p->e8[6] : 2 * PX8_ONE, 0);
            }
            break;
        case OP_CIRCLE:
            ok = op_num(p, 1, &a) && op_num(p, 2, &b) && op_num(p, 3, &c) && op_paint(d, p, 4, true, 0, &op.cidx, &op.alpha);
            if (ok) {
                op.v[0] = (int16_t)scale_px8(&s, a, s.ox8); op.v[1] = (int16_t)scale_px8(&s, b, s.oy8);
                op.v[2] = (int16_t)scale_px8(&s, c, 0);
            }
            break;
        case OP_TEXT:
            ok = op_num(p, 1, &a) && op_num(p, 2, &b) && p->etype[3] == ET_STR && op_num(p, 4, &c)
                 && op_paint(d, p, 5, true, 0, &op.cidx, &op.alpha);
            if (ok) {
                int32_t align = op_int(p, 6, 0), weight = op_int(p, 7, 0);
                int32_t size8 = scale_px8(&s, c, 0);
                int32_t size_px = px8_to_int(size8);
                // A positive size that rounds to nothing still draws (min 1); a zero size draws
                // nothing, as font-size:0 does in the browser.
                if (c > 0 && size_px < 1) size_px = 1;
                if (c <= 0) size_px = 0;
                op.v[0] = (int16_t)scale_px8(&s, a, s.ox8); op.v[1] = (int16_t)scale_px8(&s, b, s.oy8);
                op.v[2] = (int16_t)(size_px > 4095 ? 4095 : size_px);
                op.aux = (uint8_t)((align >= 0 && align <= 2 ? align : 0) | (weight ? 4 : 0));
                op.str = p->arena_start;
            }
            break;
        case OP_BITMAP: {
            int32_t modules = op_int(p, 4, 0);
            ok = op_num(p, 1, &a) && op_num(p, 2, &b) && op_num(p, 3, &c) && p->etype[5] == ET_STR
                 && !p->b64_bad && !p->big_over && modules >= 1 && modules <= BITMAP_MAX_MODULES
                 && (uint32_t)p->big_len >= (uint32_t)modules * (uint32_t)((modules + 7) / 8)
                 && op_paint(d, p, 6, false, 0x000000, &op.cidx, &op.alpha);
            if (ok) {
                int32_t x0 = scale_px8(&s, a, s.ox8);
                op.v[0] = (int16_t)x0; op.v[1] = (int16_t)scale_px8(&s, b, s.oy8);
                op.v[2] = (int16_t)(scale_px8(&s, (int64_t)a + c, s.ox8) - x0);
                op.v[3] = (int16_t)modules;
                op.str = p->arena_start;
            }
            break;
        }
        case OP_ICON: {
            const small_t *name = op_small_get(p, 4);
            int icon = (name && !name->over && p->etype[4] == ET_STR) ? icon_lookup(name->s, name->len) : -1;
            ok = op_num(p, 1, &a) && op_num(p, 2, &b) && op_num(p, 3, &c) && icon >= 0
                 && op_paint(d, p, 5, true, 0, &op.cidx, &op.alpha);
            if (ok) {
                op.v[0] = (int16_t)scale_px8(&s, a, s.ox8); op.v[1] = (int16_t)scale_px8(&s, b, s.oy8);
                op.v[2] = (int16_t)scale_px8(&s, c, 0);
                op.aux = (uint8_t)icon;
            }
            break;
        }
        case OP_CLIP:
            if (p->etype[1] == ET_NONE) { op.aux = 0; break; }   // ["k"]: clear
            ok = op_num(p, 1, &a) && op_num(p, 2, &b) && op_num(p, 3, &c) && op_num(p, 4, &e);
            if (ok) {
                int32_t x0 = scale_px(&s, a, s.ox8), y0 = scale_px(&s, b, s.oy8);
                int32_t x1 = scale_px(&s, (int64_t)a + c, s.ox8), y1 = scale_px(&s, (int64_t)b + e, s.oy8);
                op.aux = 1;
                op.v[0] = (int16_t)x0; op.v[1] = (int16_t)y0; op.v[2] = (int16_t)x1; op.v[3] = (int16_t)y1;
            }
            break;
        case OP_USE: {
            const small_t *id = op_small_get(p, 1);
            int def = (id && !id->over && p->etype[1] == ET_STR) ? parse_base36(id->s, id->len) : -1;
            int32_t paint = op_int(p, 2, -1);
            ok = def >= 0 && paint >= 0 && paint < KIOSK_MAX_PAINTS;
            if (ok) { op.v[0] = (int16_t)def; op.v[1] = (int16_t)paint; }
            // Placed form ["u", id, paint, x, y, s]: only when all three are numbers, otherwise
            // the extra elements are ignored and it is drawn where it was defined.
            if (ok && op_num(p, 3, &a) && op_num(p, 4, &b) && op_num(p, 5, &c)) {
                int32_t sc = px8_to_int(c);
                ok = sc > 0 && sc <= RASTER_XF_S_MAX;
                if (ok) {
                    op.aux = 1;
                    op.v[2] = (int16_t)scale_px8(&s, a, s.ox8); op.v[3] = (int16_t)scale_px8(&s, b, s.oy8);
                    op.v[4] = (int16_t)sc;
                }
            }
            break;
        }
        default:
            ok = false;   // unknown op letter: skipped
            break;
        }
    }
    if (ok && f->nops >= KIOSK_MAX_OPS) { ok = false; p->overflow_drops++; }
    if (!ok) {
        f->arena_len = p->arena_start;   // whatever this op staged is garbage now
        f->stats_dropped_ops++;
        return;
    }
    op.kind = p->op_kind;
    f->ops[f->nops++] = op;
}

// ---------------------------------------------------------------------------------------------
// Static set

static void defs_begin(frame_decoder_t *d, priv_t *p) {
    frame_t *f = d->frame;
    if (p->geom_begun) return;   // a second "defs" object is ignored
    p->geom_begun = true;
    d->had_static_defs = true;
    p->geom_ok = d->geom != NULL && d->scratch != NULL
              && d->scratch_len >= (size_t)KIOSK_DECODE_RECORDER_BYTES + GEOM_SCRATCH_MIN
              && geom_store_begin(d->geom, f->static_id[0] ? f->static_id : NULL,
                                  d->scratch + KIOSK_DECODE_RECORDER_BYTES, d->scratch_len - KIOSK_DECODE_RECORDER_BYTES);
}

static void def_begin(frame_decoder_t *d, priv_t *p) {
    p->def_active = true; p->def_bad = false;
    p->def_kind = DEF_NONE;
    p->grp_list = false; p->grp_member = false; p->grp_n = 0;
    small_reset(&p->kind, 0);
    memset(p->def_has, 0, sizeof p->def_has);
    if (p->geom_ok) path_recorder_init(&p->rec, d->scratch, KIOSK_DECODE_RECORDER_BYTES);
}

static void def_element(frame_decoder_t *d, priv_t *p, const json_stream_t *js, json_event_t ev, const char *data, size_t len, bool final) {
    int idx = js->index[3];
    if (!p->def_active || p->def_bad) return;
    switch (ev) {
    case JSON_EV_STRING:
        if (idx == 0) {
            small_append(&p->kind, data, len);
            if (final) {
                p->def_kind = small_eq(&p->kind, "path") ? DEF_PATH : small_eq(&p->kind, "poly") ? DEF_POLY
                            : small_eq(&p->kind, "circle") ? DEF_CIRCLE : small_eq(&p->kind, "group") ? DEF_GROUP : DEF_UNKNOWN;
            }
        } else if (idx == 1 && p->geom_ok) {
            if (p->def_kind == DEF_PATH) { if (!path_recorder_feed(&p->rec, data, len)) p->def_bad = true; }
            else if (p->def_kind == DEF_POLY) { if (!path_recorder_feed_points(&p->rec, data, len)) p->def_bad = true; }
        }
        return;
    case JSON_EV_NUMBER: {
        // path/poly: dx dy sx sy at 2..5; circle: cx cy r at 1..3.
        int slot = p->def_kind == DEF_CIRCLE ? idx - 1 : idx - 2;
        if (slot < 0 || slot >= 4) return;
        float v;
        if (!json_number_to_float(data, len, &v)) { p->def_bad = true; return; }
        p->def_f[slot] = v; p->def_has[slot] = true;
        return;
    }
    case JSON_EV_NULL: case JSON_EV_TRUE: case JSON_EV_FALSE:
        return;
    case JSON_EV_ARR_START:
        if (idx == 1 && p->def_kind == DEF_GROUP && p->grp_n == 0 && !p->grp_list) { p->grp_list = true; return; }
        p->def_bad = true;
        return;
    default:
        p->def_bad = true;
        return;
    }
}

// Group members: ["group", [[id, paint], ...]]. A member that does not parse is skipped, not the
// group; members past GEOM_GROUP_MAX are dropped.
static void member_begin(priv_t *p) {
    p->grp_member = true; p->grp_mbad = false; p->grp_paint = -1;
    small_reset(&p->sa, 0);
}

static void member_element(priv_t *p, const json_stream_t *js, json_event_t ev, const char *data, size_t len) {
    int idx = js->index[5];
    if (!p->grp_member) return;
    if (idx == 0 && (ev == JSON_EV_STRING || ev == JSON_EV_NUMBER)) small_append(&p->sa, data, len);   // a bare number is read as the id's digits
    else if (idx == 1 && ev == JSON_EV_NUMBER) { int32_t v; if (json_number_to_int(data, len, &v)) p->grp_paint = v; }
    else if (idx <= 1) p->grp_mbad = true;
}

static void member_end(frame_decoder_t *d, priv_t *p) {
    if (!p->grp_member) return;
    p->grp_member = false;
    int id = p->sa.over ? -1 : parse_base36(p->sa.s, p->sa.len);
    if (p->grp_mbad || id < 0 || p->grp_paint < 0 || p->grp_paint >= KIOSK_MAX_PAINTS) return;
    if (!p->geom_ok || p->grp_n >= GEOM_GROUP_MAX) return;
    uint16_t m[2] = { (uint16_t)id, (uint16_t)p->grp_paint };
    memcpy(d->scratch + (size_t)p->grp_n * 4, m, 4);
    p->grp_n++;
}

typedef struct { geom_store_t *g; bool ok; } vert_ctx_t;

static void def_vertex_cb(void *ctx, int32_t x8, int32_t y8, bool contour_start) {
    vert_ctx_t *v = (vert_ctx_t *)ctx;
    if (v->ok && !geom_store_add_vertex(v->g, x8, y8, contour_start)) v->ok = false;
}

static void def_end(frame_decoder_t *d, priv_t *p) {
    frame_t *f = d->frame;
    if (!p->def_active) return;
    p->def_active = false;
    if (!p->geom_ok) return;   // the store failure is reported at finish; nothing to count per def
    bool ok = !p->def_bad && p->def_id_ok;
    scale_t s = { d->k_fx, d->ox8, d->oy8 };
    if (ok && p->def_kind == DEF_CIRCLE) {
        ok = p->def_has[0] && p->def_has[1] && p->def_has[2];
        if (ok) {
            int32_t cx8 = scale_px8(&s, float_to_px8(p->def_f[0]), s.ox8);
            int32_t cy8 = scale_px8(&s, float_to_px8(p->def_f[1]), s.oy8);
            int32_t r8 = scale_px8(&s, float_to_px8(p->def_f[2]), 0);
            ok = r8 >= 0 && geom_store_add_circle(d->geom, p->def_id, cx8, cy8, r8);
        }
    } else if (ok && p->def_kind == DEF_GROUP) {
        _Static_assert(GEOM_GROUP_MAX * 4 <= KIOSK_DECODE_RECORDER_BYTES, "group members are staged in the recorder area");
        ok = geom_store_add_group(d->geom, p->def_id, (const uint16_t *)(const void *)d->scratch, p->grp_n);
    } else if (ok && (p->def_kind == DEF_PATH || p->def_kind == DEF_POLY)) {
        ok = path_recorder_finish(&p->rec);   // false: overflow or syntax error → def dropped
        if (ok) {
            float k = (float)d->k_fx / 65536.0f;
            float dx = p->def_has[0] ? p->def_f[0] : 0.0f, dy = p->def_has[1] ? p->def_f[1] : 0.0f;
            float sx = p->def_has[2] ? p->def_f[2] : 1.0f, sy = p->def_has[3] ? p->def_f[3] : 1.0f;
            affine_t t = { sx * k, sy * k, (float)d->ox8 / PX8_ONE + k * dx, (float)d->oy8 / PX8_ONE + k * dy };
            ok = geom_store_begin_poly(d->geom, p->def_id);
            if (ok) {
                vert_ctx_t vc = { d->geom, true };
                path_flattener_t fl;
                path_flattener_init(&fl, &t, KIOSK_PATH_TOLERANCE, def_vertex_cb, &vc);
                bool replayed = path_flattener_replay(&fl, p->rec.buf, p->rec.len);
                ok = geom_store_end_poly(d->geom) && replayed && vc.ok;
            }
        }
    } else {
        ok = false;
    }
    if (!ok) f->stats_dropped_defs++;
}

static void paint_begin(priv_t *p) {
    p->paint_active = true;
    small_reset(&p->sb, 0); small_reset(&p->sc, 1);
    memset(p->etype, 0, sizeof p->etype);
}

static void paint_element(priv_t *p, const json_stream_t *js, json_event_t ev, const char *data, size_t len) {
    int idx = js->index[3];
    if (!p->paint_active || idx >= OP_MAX_ELEMS) return;
    switch (ev) {
    case JSON_EV_STRING:
        p->etype[idx] = ET_STR;
        if (idx == 0) small_append(&p->sb, data, len);
        else if (idx == 1) small_append(&p->sc, data, len);
        return;
    case JSON_EV_NUMBER: {
        int32_t v;
        if (json_number_to_fixed(data, len, PX8_SHIFT, &v)) { p->etype[idx] = ET_NUM; p->e8[idx] = v; }
        return;
    }
    case JSON_EV_NULL: p->etype[idx] = ET_NULL; return;
    default: return;
    }
}

// A paint side that does not parse ("url(#hatch-pattern-3)", null) becomes "none", which is what
// the browser draws for an unresolvable paint server.
static void paint_side(frame_decoder_t *d, const priv_t *p, const small_t *s, int idx, uint8_t *cidx, uint8_t *alpha) {
    rgba8_t c;
    *cidx = 0; *alpha = 0;
    if (p->etype[idx] == ET_STR && !s->over && colour_parse(s->s, s->len, &c)) {
        *cidx = palette_add(d->pal, rgba8_to_rgb(c));
        *alpha = c.a;
    }
}

static void paint_end(frame_decoder_t *d, priv_t *p) {
    frame_t *f = d->frame;
    if (!p->paint_active) return;
    p->paint_active = false;
    if (f->npaints >= KIOSK_MAX_PAINTS) { f->stats_dropped_ops++; return; }
    paint_t pt;
    paint_side(d, p, &p->sb, 0, &pt.fill_idx, &pt.fill_alpha);
    paint_side(d, p, &p->sc, 1, &pt.stroke_idx, &pt.stroke_alpha);
    scale_t s = { d->k_fx, d->ox8, d->oy8 };
    int32_t w8 = p->etype[2] == ET_NUM ? scale_px8(&s, p->e8[2], 0) : 0;
    pt.width8 = (int16_t)(w8 < 0 ? 0 : w8);
    f->paints[f->npaints++] = pt;
}

// ---------------------------------------------------------------------------------------------
// Top level

static void static_id_chunk(frame_decoder_t *d, priv_t *p, const char *data, size_t len, bool final) {
    frame_t *f = d->frame;
    size_t room = KIOSK_MAX_STATIC_ID - 1 - p->static_id_len;
    if (len > room) len = room;   // an over-long id can never match a stored set; harmless
    memcpy(f->static_id + p->static_id_len, data, len);
    p->static_id_len = (uint8_t)(p->static_id_len + len);
    f->static_id[p->static_id_len] = 0;
    if (final && p->geom_begun && p->geom_ok) geom_store_set_id(d->geom, f->static_id);
}

static void top_scalar(frame_decoder_t *d, priv_t *p, const json_stream_t *js, json_event_t ev, const char *data, size_t len, bool final) {
    frame_t *f = d->frame;
    if (key_is(js, 0, "v")) {
        int32_t v;
        if (ev == JSON_EV_NUMBER && json_number_to_int(data, len, &v)) { p->have_v = true; p->v = v; f->version = (uint8_t)(v >= 0 && v < 256 ? v : 255); }
        else if (ev != JSON_EV_NUMBER) { p->have_v = true; p->v = -1; }
    } else if (key_is(js, 0, "rev")) {
        if (ev == JSON_EV_NUMBER) {
            // Only the low 32 bits are kept, for diagnostics; wraparound is fine.
            uint32_t r = 0;
            for (size_t i = 0; i < len && data[i] >= '0' && data[i] <= '9'; i++) r = r * 10u + (uint32_t)(data[i] - '0');
            f->rev_lo = r;
        }
    } else if (key_is(js, 0, "next_ms")) {
        int32_t v;
        if (ev == JSON_EV_NUMBER && json_number_to_int(data, len, &v)) f->next_ms = v < 0 ? 0 : (uint32_t)v;
    } else if (key_is(js, 0, "next_url")) {
        if (ev == JSON_EV_STRING) {
            size_t room = KIOSK_MAX_URL - 1 - p->next_url_len;
            if (len > room) { f->next_url_truncated = true; len = room; }
            memcpy(f->next_url + p->next_url_len, data, len);
            p->next_url_len = (uint16_t)(p->next_url_len + len);
            f->next_url[p->next_url_len] = 0;
        } else if (ev == JSON_EV_NULL) {
            f->next_url[0] = 0; p->next_url_len = 0;
        }
    } else if (key_is(js, 0, "bg")) {
        if (ev == JSON_EV_STRING) {
            small_append(&p->sb, data, len);
            if (final) {
                rgba8_t c;
                if (!p->sb.over && colour_parse(p->sb.s, p->sb.len, &c)) f->bg_idx = palette_add(d->pal, rgba8_to_rgb(c));
                small_reset(&p->sb, 0);
            }
        }
    }
    else if (key_is(js, 0, "w") || key_is(js, 0, "h")) {
        int32_t v;
        bool is_w = key_is(js, 0, "w");
        if (ev == JSON_EV_NUMBER && json_number_to_int(data, len, &v) && (is_w ? (v >= 320 && v <= 3840) : (v >= 180 && v <= 2160))) {
            if (is_w) p->canvas_w = (uint16_t)v;
            else p->canvas_h = (uint16_t)v;
            // Coordinates are scaled as they stream in, so a size that arrives after drawing began
            // cannot apply; the server sends w and h first.
            if (f->nops == 0 && !p->geom_begun) {
                scale_t s;
                scale_setup(d->out_w, d->out_h, p->canvas_w ? p->canvas_w : CANVAS_W, p->canvas_h ? p->canvas_h : CANVAS_H, &s);
                d->k_fx = s.k_fx; d->ox8 = s.ox8; d->oy8 = s.oy8;
                f->ox8 = s.ox8; f->oy8 = s.oy8;
            }
        }
    }
    // Unknown keys are ignored.
}

static bool on_event(void *ctx, const json_stream_t *js, json_event_t ev, const char *data, size_t len, bool final) {
    frame_decoder_t *d = (frame_decoder_t *)ctx;
    priv_t *p = (priv_t *)d->priv;
    uint8_t depth = js->depth;
    if (ev == JSON_EV_KEY) {
        if (depth == 3 && p->sec == SEC_DEFS) {
            int id = parse_base36(data, len);
            p->def_id_ok = id >= 0;
            p->def_id = (uint16_t)(id >= 0 ? id : 0);
        }
        return true;
    }
    switch (depth) {
    case 0:
        if (ev == JSON_EV_OBJ_START) { p->top_ok = true; return true; }
        if (ev == JSON_EV_OBJ_END) return true;
        return false;   // a frame is an object; anything else is not worth parsing further
    case 1:
        switch (ev) {
        case JSON_EV_OBJ_START: p->sec = key_is(js, 0, "static") ? SEC_STATIC : SEC_TOP; return true;
        case JSON_EV_ARR_START: p->sec = key_is(js, 0, "ops") ? SEC_OPS : SEC_TOP; return true;
        case JSON_EV_OBJ_END: case JSON_EV_ARR_END: p->sec = SEC_TOP; return true;
        default: top_scalar(d, p, js, ev, data, len, final); return true;
        }
    case 2:
        if (p->sec == SEC_OPS) {
            if (ev == JSON_EV_ARR_START) op_begin(d, p);
            else if (ev == JSON_EV_ARR_END) op_end(d, p);
            return true;   // a non-array op is skipped (op_active stays false)
        }
        if (p->sec == SEC_STATIC) {
            if (ev == JSON_EV_OBJ_START) { if (key_is(js, 1, "defs")) { p->sec = SEC_DEFS; defs_begin(d, p); } }
            else if (ev == JSON_EV_ARR_START) { if (key_is(js, 1, "paints")) p->sec = SEC_PAINTS; }
            else if (ev == JSON_EV_STRING) { if (key_is(js, 1, "id")) static_id_chunk(d, p, data, len, final); }
            return true;
        }
        if (p->sec == SEC_DEFS || p->sec == SEC_PAINTS) {
            if (ev == JSON_EV_OBJ_END || ev == JSON_EV_ARR_END) p->sec = SEC_STATIC;
            return true;
        }
        return true;
    case 3:
        if (p->sec == SEC_OPS) {
            if (p->op_active) {
                if (ev == JSON_EV_ARR_END || ev == JSON_EV_OBJ_END) return true;   // end of a nested (bad) container
                op_element(d, p, js, ev, data, len, final);
            }
            return true;
        }
        if (p->sec == SEC_DEFS) {
            if (ev == JSON_EV_ARR_START) def_begin(d, p);
            else if (ev == JSON_EV_ARR_END) def_end(d, p);
            return true;
        }
        if (p->sec == SEC_PAINTS) {
            if (ev == JSON_EV_ARR_START) paint_begin(p);
            else if (ev == JSON_EV_ARR_END) paint_end(d, p);
            return true;
        }
        return true;
    case 4:
        if (p->sec == SEC_DEFS && p->def_active) {
            if (ev == JSON_EV_ARR_END && p->grp_list) { p->grp_list = false; return true; }
            if (ev == JSON_EV_ARR_END || ev == JSON_EV_OBJ_END) return true;
            def_element(d, p, js, ev, data, len, final);
        } else if (p->sec == SEC_PAINTS && p->paint_active) {
            if (ev == JSON_EV_ARR_END || ev == JSON_EV_OBJ_END) return true;
            paint_element(p, js, ev, data, len);
        }
        return true;
    case 5:
        if (p->sec == SEC_DEFS && p->def_active && p->grp_list) {
            if (ev == JSON_EV_ARR_START) member_begin(p);
            else if (ev == JSON_EV_ARR_END) member_end(d, p);
        }
        return true;   // a member that is not an array is skipped
    case 6:
        if (p->sec == SEC_DEFS && p->def_active && p->grp_member) {
            if (ev == JSON_EV_ARR_START || ev == JSON_EV_OBJ_START) p->grp_mbad = true;
            else if (ev != JSON_EV_ARR_END && ev != JSON_EV_OBJ_END) member_element(p, js, ev, data, len);
        }
        return true;
    default:
        return true;   // deeper nesting never carries anything we draw
    }
}

void frame_decoder_init(frame_decoder_t *d, frame_t *frame, palette_t *pal, geom_store_t *geom, uint8_t *scratch, size_t scratch_len, uint16_t out_w, uint16_t out_h) {
    memset(d, 0, sizeof *d);
    d->frame = frame; d->pal = pal; d->geom = geom;
    d->scratch = scratch; d->scratch_len = scratch_len;
    d->out_w = out_w; d->out_h = out_h;
    scale_t s;
    scale_setup(out_w, out_h, CANVAS_W, CANVAS_H, &s);
    d->k_fx = s.k_fx; d->ox8 = s.ox8; d->oy8 = s.oy8;
    d->status = FD_OK;
    json_stream_init(&d->js, on_event, d);

    // The op table and arena are not cleared: nops/arena_len are the only truth about them.
    frame->version = 0;
    frame->w = out_w; frame->h = out_h;
    frame->ox8 = s.ox8; frame->oy8 = s.oy8;
    frame->nops = 0; frame->arena_len = 0; frame->npaints = 0;
    frame->static_id[0] = 0; frame->next_url[0] = 0; frame->next_url_truncated = false;
    frame->next_ms = 0; frame->rev_lo = 0;
    frame->stats_dropped_ops = 0; frame->stats_dropped_bytes = 0; frame->stats_dropped_defs = 0;

    if (pal->count > PALETTE_RESET_THRESHOLD) palette_reset(pal);
    frame->bg_idx = palette_add(pal, 0x0d1b2a);   // the app's background, used when bg is absent or unparseable

    priv_t *p = (priv_t *)d->priv;
    small_reset(&p->sb, 0); small_reset(&p->sa, -1); small_reset(&p->sc, -1); small_reset(&p->kind, 0);
}

bool frame_decoder_feed(frame_decoder_t *d, const char *data, size_t len) {
    if (d->status != FD_OK) return false;
    d->bytes_fed += (uint32_t)len;
    if (!json_stream_feed(&d->js, data, len)) { d->status = FD_ERR_JSON; return false; }
    return true;
}

frame_status_t frame_decoder_finish(frame_decoder_t *d) {
    priv_t *p = (priv_t *)d->priv;
    frame_t *f = d->frame;
    bool store_open = p->geom_begun && p->geom_ok;
    if (d->status != FD_OK || !json_stream_finish(&d->js) || !p->top_ok) {
        if (store_open) geom_store_abort(d->geom);
        return d->status = FD_ERR_JSON;
    }
    if (!p->have_v || p->v != KIOSK_PROTOCOL_VERSION) {
        if (store_open) geom_store_abort(d->geom);
        return d->status = FD_ERR_VERSION;
    }
    if (p->geom_begun) {
        if (!p->geom_ok) return d->status = FD_ERR_STATIC_STORE;
        if (!geom_store_commit(d->geom)) { geom_store_abort(d->geom); return d->status = FD_ERR_STATIC_STORE; }
        d->static_ready = f->static_id[0] != 0;
    }
    if (f->static_id[0] && !d->static_ready) {
        const char *open = (d->geom && geom_store_is_open(d->geom)) ? geom_store_id(d->geom) : NULL;
        if (open && strcmp(open, f->static_id) == 0) d->static_ready = true;
        else return d->status = FD_ERR_STATIC_MISSING;
    }
    if (p->overflow_drops) return d->status = FD_ERR_TOO_MANY_OPS;
    return d->status = FD_OK;
}

// ---------------------------------------------------------------------------------------------
// Renderer

// Flattened vertices of the icon most recently drawn. Icons are re-flattened per band; the one
// entry cache saves that for the common single-icon frame. Read-only tables aside, this is the
// only static state of the module, and rendering never overlaps decoding.
static int16_t icon_verts[ICON_VERT_MAX * 2];
static uint32_t icon_nverts;
static bool icon_overflow;
static int32_t icon_key[4] = { -1, 0, 0, 0 };

static void icon_vertex_cb(void *ctx, int32_t x8, int32_t y8, bool contour_start) {
    (void)ctx;
    if (contour_start && icon_nverts) {
        if (icon_nverts >= ICON_VERT_MAX) { icon_overflow = true; return; }
        icon_verts[icon_nverts * 2] = GEOM_BREAK; icon_verts[icon_nverts * 2 + 1] = 0;
        icon_nverts++;
    }
    if (icon_nverts >= ICON_VERT_MAX) { icon_overflow = true; return; }
    icon_verts[icon_nverts * 2] = (int16_t)clamp_px8(x8);
    icon_verts[icon_nverts * 2 + 1] = (int16_t)clamp_px8(y8);
    icon_nverts++;
}

static bool icon_prepare(int idx, int32_t x8, int32_t y8, int32_t size8) {
    if (icon_key[0] == idx && icon_key[1] == x8 && icon_key[2] == y8 && icon_key[3] == size8) return !icon_overflow;
    icon_key[0] = idx; icon_key[1] = x8; icon_key[2] = y8; icon_key[3] = size8;
    icon_nverts = 0; icon_overflow = false;
    float sc = (float)size8 / (PX8_ONE * 24.0f);   // icon units → device px
    affine_t t = { sc, sc, (float)x8 / PX8_ONE, (float)y8 / PX8_ONE };
    if (!path_flatten_string(icon_defs[idx].d, &t, ICON_TOL, icon_vertex_cb, NULL)) icon_overflow = true;
    return !icon_overflow;
}

static const uint8_t *arena_str(const frame_t *f, uint16_t off, size_t *len) {
    if ((uint32_t)off + 2 > f->arena_len) { *len = 0; return NULL; }
    size_t n = (size_t)f->arena[off] | ((size_t)f->arena[off + 1] << 8);
    if ((uint32_t)off + 2 + n > f->arena_len) { *len = 0; return NULL; }
    *len = n;
    return f->arena + off + 2;
}

static inline int32_t px_floor(int32_t v8) { return v8 >> PX8_SHIFT; }
static inline int32_t px_ceil(int32_t v8) { return (v8 + PX8_ONE - 1) >> PX8_SHIFT; }

// Draws one POLY or CIRCLE record with a paint: where it was defined, or placed through `xf`.
// A placement whose box leaves the ±PX8_MAX range of stored geometry is not drawn (see
// docs/RENDERING.md): clamping its vertices would bend edges that can still be on screen.
static bool draw_geom(raster_t *r, const geom_rec_t *rec, const paint_t *pt, const raster_xf_t *xf) {
    bool fill = pt->fill_alpha != 0, stroke = pt->stroke_alpha != 0 && pt->width8 > 0;
    if (!fill && !stroke) return false;
    int32_t w8 = pt->width8;
    if (xf && stroke) { w8 = raster_xf_len(xf, w8); if (w8 < 1) w8 = 1; }
    int32_t pad = stroke ? (w8 + PX8_ONE - 1) / PX8_ONE / 2 + 1 : 0;
    if (rec->kind == GEOM_POLY) {
        const int16_t *v = geom_rec_verts(rec);
        if (rec->count < 2) return false;
        if (!xf) {
            if (!raster_band_intersects(r, rec->bx0 - pad, rec->by0 - pad, rec->bx1 + pad, rec->by1 + pad)) return false;
            if (fill) raster_fill_poly(r, v, rec->count, FILL_NONZERO, pt->fill_idx, pt->fill_alpha);
            if (stroke) raster_stroke_poly(r, v, rec->count, pt->width8, true, false, pt->stroke_idx, pt->stroke_alpha);
            return true;
        }
        // The mapping is monotonic in x and in y, so the box maps onto the box of the mapped vertices.
        int32_t x0 = raster_xf_x(xf, rec->bx0 * PX8_ONE), x1 = raster_xf_x(xf, rec->bx1 * PX8_ONE);
        int32_t y0 = raster_xf_y(xf, rec->by0 * PX8_ONE), y1 = raster_xf_y(xf, rec->by1 * PX8_ONE);
        if (x0 < -PX8_MAX || y0 < -PX8_MAX || x1 > PX8_MAX || y1 > PX8_MAX) return false;
        if (!raster_band_intersects(r, px_floor(x0) - pad, px_floor(y0) - pad, px_ceil(x1) + pad, px_ceil(y1) + pad)) return false;
        if (fill) raster_fill_poly_xf(r, v, rec->count, FILL_NONZERO, xf, pt->fill_idx, pt->fill_alpha);
        if (stroke) raster_stroke_poly_xf(r, v, rec->count, w8, true, xf, pt->stroke_idx, pt->stroke_alpha);
        return true;
    }
    if (rec->kind == GEOM_CIRCLE) {
        const int32_t *c = geom_rec_circle(rec);
        int32_t cx = c[0], cy = c[1], rr = c[2];
        if (!xf) {
            if (!raster_band_intersects(r, rec->bx0 - pad, rec->by0 - pad, rec->bx1 + pad, rec->by1 + pad)) return false;
        } else {
            cx = raster_xf_x(xf, cx); cy = raster_xf_y(xf, cy); rr = raster_xf_len(xf, rr);
            if (cx - rr < -PX8_MAX || cy - rr < -PX8_MAX || cx + rr > PX8_MAX || cy + rr > PX8_MAX) return false;
            if (!raster_band_intersects(r, px_floor(cx - rr) - pad, px_floor(cy - rr) - pad, px_ceil(cx + rr) + pad, px_ceil(cy + rr) + pad)) return false;
        }
        if (fill) raster_fill_circle(r, cx, cy, rr, pt->fill_idx, pt->fill_alpha);
        if (stroke) raster_stroke_circle(r, cx, cy, rr, w8, pt->stroke_idx, pt->stroke_alpha);
        return true;
    }
    return false;
}

// Draws one op into the current band. Returns true if it drew (or set clip), false if culled.
static bool draw_op(const frame_t *f, const geom_store_t *geom, raster_t *r, const op_t *op) {
    switch (op->kind) {
    case OP_CLIP:
        if (op->aux) raster_set_clip(r, op->v[0], op->v[1], op->v[2], op->v[3]);
        else raster_clear_clip(r);
        return true;
    case OP_RECT:
        if (!op->alpha || op->v[2] <= 0 || op->v[3] <= 0) return false;
        if (!raster_band_intersects(r, px_floor(op->v[0]), px_floor(op->v[1]), px_ceil(op->v[0] + op->v[2]), px_ceil(op->v[1] + op->v[3]))) return false;
        raster_fill_rect(r, op->v[0], op->v[1], op->v[2], op->v[3], op->v[4], op->cidx, op->alpha);
        return true;
    case OP_LINE: {
        if (!op->alpha || op->v[4] <= 0) return false;
        int32_t hw = (op->v[4] + 1) / 2 + PX8_ONE;
        int32_t x0 = op->v[0] < op->v[2] ? op->v[0] : op->v[2], x1 = op->v[0] < op->v[2] ? op->v[2] : op->v[0];
        int32_t y0 = op->v[1] < op->v[3] ? op->v[1] : op->v[3], y1 = op->v[1] < op->v[3] ? op->v[3] : op->v[1];
        if (!raster_band_intersects(r, px_floor(x0 - hw), px_floor(y0 - hw), px_ceil(x1 + hw), px_ceil(y1 + hw))) return false;
        raster_line(r, op->v[0], op->v[1], op->v[2], op->v[3], op->v[4], op->cidx, op->alpha);
        return true;
    }
    case OP_CIRCLE:
        if (!op->alpha || op->v[2] <= 0) return false;
        if (!raster_band_intersects(r, px_floor(op->v[0] - op->v[2]), px_floor(op->v[1] - op->v[2]), px_ceil(op->v[0] + op->v[2]), px_ceil(op->v[1] + op->v[2]))) return false;
        raster_fill_circle(r, op->v[0], op->v[1], op->v[2], op->cidx, op->alpha);
        return true;
    case OP_TEXT: {
        size_t len;
        const uint8_t *s = arena_str(f, op->str, &len);
        int size_px = op->v[2];
        bool bold = (op->aux & 4) != 0;
        if (!op->alpha || !s || len == 0 || size_px <= 0) return false;
        int32_t asc8, desc8;
        font_extent(size_px, bold, &asc8, &desc8);
        // Horizontal culling would need a measure per band; the band test on y is what matters.
        if (!raster_band_intersects(r, 0, px_floor(op->v[1] - asc8) - 1, r->width, px_ceil(op->v[1] + desc8) + 1)) return false;
        font_draw(r, op->v[0], op->v[1], (const char *)s, len, size_px, bold, (uint8_t)(op->aux & 3), op->cidx, op->alpha);
        return true;
    }
    case OP_BITMAP: {
        size_t len;
        const uint8_t *bits = arena_str(f, op->str, &len);
        uint32_t modules = (uint32_t)op->v[3];
        if (!op->alpha || !bits || op->v[2] <= 0 || modules == 0 || len < modules * ((modules + 7) / 8)) return false;
        if (!raster_band_intersects(r, px_floor(op->v[0]), px_floor(op->v[1]), px_ceil(op->v[0] + op->v[2]), px_ceil(op->v[1] + op->v[2]))) return false;
        raster_bitmap(r, op->v[0], op->v[1], op->v[2], (uint16_t)modules, bits, op->cidx);
        return true;
    }
    case OP_ICON: {
        if (!op->alpha || op->aux >= ICON_COUNT || op->v[2] <= 0) return false;
        const icon_def_t *ic = &icon_defs[op->aux];
        int32_t pad = ic->stroke ? op->v[2] / 12 + PX8_ONE : 0;   // half the stroke width plus a pixel
        if (!raster_band_intersects(r, px_floor(op->v[0] - pad), px_floor(op->v[1] - pad), px_ceil(op->v[0] + op->v[2] + pad), px_ceil(op->v[1] + op->v[2] + pad))) return false;
        if (!icon_prepare(op->aux, op->v[0], op->v[1], op->v[2]) || icon_nverts < 2) return false;
        if (ic->stroke) raster_stroke_poly(r, icon_verts, icon_nverts, (op->v[2] * 2) / 24, false, true, op->cidx, op->alpha);
        else raster_fill_poly_aa(r, icon_verts, icon_nverts, ic->evenodd ? FILL_EVENODD : FILL_NONZERO, 3, op->cidx, op->alpha);
        return true;
    }
    case OP_USE: {
        const geom_rec_t *rec = geom ? geom_store_get(geom, (uint16_t)op->v[0]) : NULL;
        if (!rec) return false;
        raster_xf_t xf;
        if (op->aux) {
            xf.s_q10 = (op->v[4] * 1024 + 500) / 1000;
            xf.ox8 = f->ox8; xf.oy8 = f->oy8;
            xf.tx8 = op->v[2]; xf.ty8 = op->v[3];
        }
        if (rec->kind != GEOM_GROUP) {
            if (op->v[1] < 0 || op->v[1] >= (int16_t)f->npaints) return false;
            return draw_geom(r, rec, &f->paints[op->v[1]], op->aux ? &xf : NULL);
        }
        // A group draws its members in order with their own paints (the op's paint is unused).
        // Each member is culled against the band by its own box, which rejects every band the
        // union of the members' boxes would and costs no extra pass over them.
        const uint16_t *m = geom_rec_members(rec);
        bool drew = false;
        for (uint32_t i = 0; i < rec->count; i++) {
            uint16_t mid = m[i * 2], mpaint = m[i * 2 + 1];
            if (mpaint >= f->npaints) continue;
            const geom_rec_t *mr = geom_store_get(geom, mid);
            if (!mr || mr->kind == GEOM_GROUP) continue;
            if (draw_geom(r, mr, &f->paints[mpaint], op->aux ? &xf : NULL)) drew = true;
        }
        return drew;
    }
    default:
        return false;
    }
}

void frame_render(const frame_t *f, palette_t *pal, const geom_store_t *geom, raster_t *r,
                  const op_t *extra, uint16_t nextra, frame_line_sink_t sink, void *ctx, render_stats_t *stats) {
    render_stats_t st;
    memset(&st, 0, sizeof st);
    uint32_t ev0 = r->stats_edge_visits, sp0 = r->stats_spans;
    (void)pal;   // blends go through r->pal, which the caller set up with the same palette
    uint16_t height = r->height, width = r->width;
    for (int32_t y0 = 0; y0 < height; y0 += KIOSK_BAND_LINES) {
        int32_t lines = height - y0 < KIOSK_BAND_LINES ? height - y0 : KIOSK_BAND_LINES;
        raster_begin_band(r, (int16_t)y0, (int16_t)lines, f->bg_idx);
        // The clip is part of the op stream, so every band replays it from the first op.
        raster_clear_clip(r);
        for (uint32_t i = 0; i < f->nops; i++) {
            if (draw_op(f, geom, r, &f->ops[i])) st.ops_drawn++; else st.ops_skipped++;
        }
        for (uint32_t i = 0; i < nextra; i++) {
            if (draw_op(f, geom, r, &extra[i])) st.ops_drawn++; else st.ops_skipped++;
        }
        if (sink) {
            for (int32_t i = 0; i < lines; i++) sink(ctx, (uint16_t)(y0 + i), r->band + (size_t)i * width, width);
        }
    }
    raster_clear_clip(r);
    st.edge_visits = r->stats_edge_visits - ev0;
    st.spans = r->stats_spans - sp0;
    if (stats) *stats = st;
}

// ---------------------------------------------------------------------------------------------
// Overlay helpers (canvas-space integers → device ops, same scaling as the decoder)

static bool fits_i16(int32_t v) { return v >= INT16_MIN && v <= INT16_MAX; }

bool frame_make_overlay_icon(op_t *op, uint16_t out_w, uint16_t out_h, int x, int y, int size, int icon_index, uint8_t cidx) {
    if (!op || icon_index < 0 || icon_index >= ICON_COUNT || size <= 0) return false;
    scale_t s;
    scale_setup(out_w, out_h, CANVAS_W, CANVAS_H, &s);
    int32_t x8 = scale_px8(&s, x * PX8_ONE, s.ox8), y8 = scale_px8(&s, y * PX8_ONE, s.oy8), size8 = scale_px8(&s, size * PX8_ONE, 0);
    if (!fits_i16(x8) || !fits_i16(y8) || !fits_i16(size8) || size8 <= 0) return false;
    memset(op, 0, sizeof *op);
    op->kind = OP_ICON; op->aux = (uint8_t)icon_index; op->cidx = cidx; op->alpha = 255;
    op->v[0] = (int16_t)x8; op->v[1] = (int16_t)y8; op->v[2] = (int16_t)size8;
    return true;
}

bool frame_make_overlay_rect(op_t *op, uint16_t out_w, uint16_t out_h, int x, int y, int w, int h, int radius, uint8_t cidx, uint8_t alpha) {
    if (!op || w < 0 || h < 0 || radius < 0) return false;
    scale_t s;
    scale_setup(out_w, out_h, CANVAS_W, CANVAS_H, &s);
    int32_t x0 = scale_px8(&s, x * PX8_ONE, s.ox8), y0 = scale_px8(&s, y * PX8_ONE, s.oy8);
    int32_t x1 = scale_px8(&s, (x + w) * PX8_ONE, s.ox8), y1 = scale_px8(&s, (y + h) * PX8_ONE, s.oy8);
    int32_t r8 = scale_px8(&s, radius * PX8_ONE, 0);
    if (!fits_i16(x0) || !fits_i16(y0) || !fits_i16(x1 - x0) || !fits_i16(y1 - y0) || !fits_i16(r8)) return false;
    memset(op, 0, sizeof *op);
    op->kind = OP_RECT; op->cidx = cidx; op->alpha = alpha;
    op->v[0] = (int16_t)x0; op->v[1] = (int16_t)y0; op->v[2] = (int16_t)(x1 - x0); op->v[3] = (int16_t)(y1 - y0); op->v[4] = (int16_t)r8;
    return true;
}
