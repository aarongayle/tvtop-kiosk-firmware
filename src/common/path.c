// SVG path scanner, recorder and flattener. See path.h for the contract.
//
// One scanner (scan_feed) tokenises path text into "command instance starts" and "completed
// numbers"; the recorder and the flattener are two sinks on that token stream. Implicit command
// repetition is resolved by the scanner (so a recording only ever holds explicit commands), as is
// the arc-flag rule (a lone '0'/'1' is a whole number when an A/a command expects a flag).
//
// Recording format (private to this file): [u8 command letter][nargs encoded numbers], repeated;
// a points-attribute recording starts with REC_POLY_MARK (0xFF, never a command letter) so that
// finish() can close it with Z without the recorder needing state of its own. A number
// is stored as a decimal (int, scale 10^-k) whenever the text was a plain decimal with at most two
// fractional digits, in the shortest of:
//   0x00..0xEF        one byte, k=1, value = (b - 120) / 10           (covers -12.0 .. +11.9)
//   0xF0 + int16      k=1     0xF1 + int16   k=2     0xF2 + int32   k=2     0xF3 + int32   k=0
//   0xF4 + float      anything else (exponents, > 2 decimals, huge)
// The map data the server emits is dominated by short relative curve coordinates ("c0.4,0.3,...")
// so this averages ~1.15 bytes per number, against 4 for a raw float: it is what lets the largest
// Europe territories (24 KB of text) fit the 9 KB decode recorder. Decoding goes through the same
// parts_to_float() as the text parser, so a replay produces bit-identical floats to a direct feed
// (the recorder verifies that per number and falls back to the raw float otherwise).
#include "path.h"
#include <math.h>
#include <string.h>

// ---------------------------------------------------------------------------------------------
// Numbers

typedef struct {
    float value;
    int32_t dec;     // decimal mantissa with sign, valid when compact
    uint8_t k;       // value == dec / 10^k when compact (k in 0..2)
    bool compact;
} pnum_t;

// 10^0..10^10 are exact in float, so one division by them is correctly rounded (this is what makes
// 12/10 and 120/100 yield the same float — the recorder relies on it when it rescales a decimal).
static const float pow10f_tab[11] = { 1e0f, 1e1f, 1e2f, 1e3f, 1e4f, 1e5f, 1e6f, 1e7f, 1e8f, 1e9f, 1e10f };

#define PATH_NUM_LIMIT 1.0e9f   // source-space magnitudes beyond this are clamped (keeps math finite)

static float parts_to_float(uint32_t mant, int e, bool neg) {
    float v = (float)mant;
    while (e > 10) { v *= 1e10f; e -= 10; if (v > PATH_NUM_LIMIT) break; }
    if (e > 0) v *= pow10f_tab[e];
    while (e < -10) { v /= 1e10f; e += 10; }
    if (e < 0) v /= pow10f_tab[-e];
    if (!(v <= PATH_NUM_LIMIT)) v = PATH_NUM_LIMIT;   // also catches inf/NaN
    return neg ? -v : v;
}

static void num_parse(const char *s, uint8_t n, pnum_t *out) {
    uint8_t i = 0;
    bool neg = false, has_exp = false, truncated = false;
    uint32_t mant = 0;
    int ndig = 0, e = 0;
    if (i < n && (s[i] == '-' || s[i] == '+')) { neg = s[i] == '-'; i++; }
    for (; i < n && s[i] >= '0' && s[i] <= '9'; i++) {
        if (ndig < 9) { mant = mant * 10 + (uint32_t)(s[i] - '0'); if (mant) ndig++; }
        else { e++; truncated = true; }
    }
    if (i < n && s[i] == '.') {
        i++;
        for (; i < n && s[i] >= '0' && s[i] <= '9'; i++) {
            if (ndig < 9) { mant = mant * 10 + (uint32_t)(s[i] - '0'); if (mant) ndig++; e--; }
            else truncated = true;
        }
    }
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        bool eneg = false; int ev = 0;
        has_exp = true; i++;
        if (i < n && (s[i] == '-' || s[i] == '+')) { eneg = s[i] == '-'; i++; }
        for (; i < n && s[i] >= '0' && s[i] <= '9'; i++) if (ev < 1000) ev = ev * 10 + (s[i] - '0');
        e += eneg ? -ev : ev;
    }
    if (mant == 0) e = 0;
    out->value = parts_to_float(mant, e, neg);
    out->compact = !has_exp && !truncated && e <= 0 && e >= -2 && mant <= (uint32_t)INT32_MAX;
    out->dec = neg ? -(int32_t)mant : (int32_t)mant;
    out->k = (uint8_t)(-e);
}

// ---------------------------------------------------------------------------------------------
// Scanner

static int cmd_nargs(uint8_t c) {
    switch (c) {
    case 'M': case 'm': case 'L': case 'l': case 'T': case 't': return 2;
    case 'H': case 'h': case 'V': case 'v': return 1;
    case 'C': case 'c': return 6;
    case 'S': case 's': case 'Q': case 'q': return 4;
    case 'A': case 'a': return 7;
    case 'Z': case 'z': return 0;
    default: return -1;
    }
}

typedef struct {
    bool (*on_cmd)(void *ctx, uint8_t cmd);                    // a command instance begins
    bool (*on_num)(void *ctx, uint8_t argi, const pnum_t *num); // argi-th argument of it
} scan_sink_t;

static bool scan_number_done(path_scanner_t *sc, const scan_sink_t *sink, void *ctx) {
    pnum_t num;
    if (!sc->in_num) return true;
    sc->in_num = false;
    if (sc->cmd == 0 || cmd_nargs(sc->cmd) == 0) { sc->error = true; return false; }   // number without a command
    if (sc->num_has_exp && !sc->num_prev_exp) { sc->error = true; return false; }         // "1e" with no exponent digits
    if (sc->argi >= cmd_nargs(sc->cmd)) {
        // Implicit repetition: another argument group for the same command; after M/m it is L/l.
        uint8_t c = sc->cmd;
        if (c == 'M') c = 'L'; else if (c == 'm') c = 'l';
        sc->cmd = c; sc->argi = 0;
        if (!sink->on_cmd(ctx, c)) { sc->error = true; return false; }
    }
    num_parse(sc->num, sc->numlen, &num);
    if (!sink->on_num(ctx, sc->argi, &num)) { sc->error = true; return false; }
    sc->argi++;
    return true;
}

static void scan_number_begin(path_scanner_t *sc, char c) {
    sc->in_num = true; sc->numlen = 0; sc->num_has_dot = false; sc->num_has_exp = false; sc->num_prev_exp = false;
    sc->num[sc->numlen++] = c;
    if (c == '.') sc->num_has_dot = true;
}

// Appends a character to the number buffer. Digits past the buffer are dropped: only fraction
// digits can get there in practice (an integer part of 18+ digits is already clamped to the
// magnitude limit), and 18 significant digits exceed float precision anyway.
static void scan_number_push(path_scanner_t *sc, char c) {
    if (sc->numlen < (uint8_t)(sizeof sc->num - 1)) sc->num[sc->numlen++] = c;
}

// polygon_mode: "x,y x,y ..." — letters are errors and the stream starts with an implicit M.
static bool scan_feed(path_scanner_t *sc, const char *text, size_t len, bool polygon_mode, const scan_sink_t *sink, void *ctx) {
    if (sc->error) return false;
    if (polygon_mode && sc->cmd == 0) {
        sc->cmd = 'M'; sc->argi = 0;
        if (!sink->on_cmd(ctx, 'M')) { sc->error = true; return false; }
    }
    for (size_t i = 0; i < len; i++) {
        char c = text[i];
        if (sc->in_num) {
            bool in_exp = sc->num_has_exp;
            if (c >= '0' && c <= '9') { scan_number_push(sc, c); if (in_exp) sc->num_prev_exp = true; continue; }
            if (c == '.' && !sc->num_has_dot && !in_exp) { sc->num_has_dot = true; scan_number_push(sc, c); continue; }
            if ((c == 'e' || c == 'E') && !in_exp && sc->numlen > 0 && sc->num[sc->numlen - 1] >= '0' && sc->num[sc->numlen - 1] <= '9') {
                // An exponent only follows a digit; it can never be a command letter in SVG.
                sc->num_has_exp = true; scan_number_push(sc, c); continue;
            }
            if ((c == '-' || c == '+') && in_exp && !sc->num_prev_exp && (sc->num[sc->numlen - 1] == 'e' || sc->num[sc->numlen - 1] == 'E')) {
                scan_number_push(sc, c); continue;
            }
            // Anything else ends the number ("1.5.3", "-2-3", separators, letters).
            if (!scan_number_done(sc, sink, ctx)) return false;
        }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v' || c == ',') continue;
        if ((c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+') {
            bool flag_slot = (sc->cmd == 'A' || sc->cmd == 'a') && sc->argi < 7 && (sc->argi % 7 == 3 || sc->argi % 7 == 4);
            scan_number_begin(sc, c);
            // Arc flags are single characters: "1 0" and "10" both mean two flags.
            if (flag_slot && (c == '0' || c == '1')) { if (!scan_number_done(sc, sink, ctx)) return false; }
            continue;
        }
        if (polygon_mode || cmd_nargs((uint8_t)c) < 0) { sc->error = true; return false; }
        // A new command letter: the previous command must have a complete argument group
        // (or none for a bare letter that is being replaced, which is an error per spec).
        if (sc->cmd != 0 && sc->argi != 0 && sc->argi < cmd_nargs(sc->cmd)) { sc->error = true; return false; }
        if (sc->cmd != 0 && sc->argi == 0 && cmd_nargs(sc->cmd) > 0) { sc->error = true; return false; }
        sc->cmd = (uint8_t)c; sc->argi = 0;
        if (!sink->on_cmd(ctx, (uint8_t)c)) { sc->error = true; return false; }
    }
    return true;
}

// Flushes a pending number; then reports whether the last command is complete.
static bool scan_finish(path_scanner_t *sc, bool polygon_mode, const scan_sink_t *sink, void *ctx) {
    if (sc->error) return false;
    if (!scan_number_done(sc, sink, ctx)) return false;
    if (sc->cmd != 0 && sc->argi != 0 && sc->argi < cmd_nargs(sc->cmd)) {
        // Trailing incomplete group. SVG 2 says a polygon with an odd coordinate count renders
        // the complete pairs; a path with a partial command is an error.
        if (polygon_mode) return true;
        sc->error = true; return false;
    }
    // A trailing bare command letter ("... L") is left alone: it draws nothing more and the browser
    // renders everything before it, so rejecting the whole path would lose visible geometry.
    return true;
}

// ---------------------------------------------------------------------------------------------
// Recorder

void path_recorder_init(path_recorder_t *r, uint8_t *buf, size_t cap) {
    memset(r, 0, sizeof *r);
    r->buf = buf; r->cap = cap;
}

static bool rec_put(path_recorder_t *r, const void *p, size_t n) {
    if (r->overflow || r->len + n > r->cap) { r->overflow = true; return false; }
    memcpy(r->buf + r->len, p, n);
    r->len += n;
    return true;
}

static bool rec_on_cmd(void *ctx, uint8_t cmd) {
    return rec_put((path_recorder_t *)ctx, &cmd, 1);
}

static float dec_to_float(int32_t dec, uint8_t k) {
    uint32_t mant = dec < 0 ? (uint32_t)(-(int64_t)dec) : (uint32_t)dec;
    return parts_to_float(mant, -(int)k, dec < 0);
}

// Encodes one number into b; returns the byte count (1, 3 or 5).
static size_t rec_encode(const pnum_t *num, uint8_t b[5]) {
    size_t n = 0;
    if (num->compact) {
        int32_t d = num->dec; uint8_t k = num->k;
        // Rescale to k=1 when exact (mantissas < 2^24 keep the "correctly rounded" argument valid).
        if (k == 0 && d >= -(1 << 20) && d <= (1 << 20)) { d *= 10; k = 1; }
        else if (k == 2 && d % 10 == 0 && d >= -(1 << 24) && d <= (1 << 24)) { d /= 10; k = 1; }
        if (k == 1 && d >= -120 && d <= 119) { b[0] = (uint8_t)(d + 120); n = 1; }
        else if (k == 1 && d >= INT16_MIN && d <= INT16_MAX) { b[0] = 0xF0; int16_t v = (int16_t)d; memcpy(b + 1, &v, 2); n = 3; }
        else if (k == 2 && d >= INT16_MIN && d <= INT16_MAX) { b[0] = 0xF1; int16_t v = (int16_t)d; memcpy(b + 1, &v, 2); n = 3; }
        else if (k == 2) { b[0] = 0xF2; memcpy(b + 1, &d, 4); n = 5; }
        else if (k == 0) { b[0] = 0xF3; memcpy(b + 1, &d, 4); n = 5; }
        if (n && dec_to_float(d, k) != num->value) n = 0;   // belt and braces: never change the value
    }
    if (n == 0) { b[0] = 0xF4; memcpy(b + 1, &num->value, 4); n = 5; }
    return n;
}

static bool rec_on_num(void *ctx, uint8_t argi, const pnum_t *num) {
    uint8_t b[5];
    (void)argi;
    return rec_put((path_recorder_t *)ctx, b, rec_encode(num, b));
}

static const scan_sink_t rec_sink = { rec_on_cmd, rec_on_num };

bool path_recorder_feed(path_recorder_t *r, const char *text, size_t len) {
    return scan_feed(&r->sc, text, len, false, &rec_sink, r) && !r->overflow;
}

#define REC_POLY_MARK 0xFF

bool path_recorder_feed_points(path_recorder_t *r, const char *text, size_t len) {
    if (r->sc.cmd == 0 && !r->sc.error) { uint8_t m = REC_POLY_MARK; rec_put(r, &m, 1); }   // first chunk
    return scan_feed(&r->sc, text, len, true, &rec_sink, r) && !r->overflow;
}

bool path_recorder_finish(path_recorder_t *r) {
    bool polygon = r->len > 0 && r->buf[0] == REC_POLY_MARK;
    if (!scan_finish(&r->sc, polygon, &rec_sink, r) || r->overflow) return false;
    int nargs = r->sc.cmd ? cmd_nargs(r->sc.cmd) : 0;
    if (r->sc.cmd != 0 && r->sc.argi < nargs) {
        // A trailing incomplete group is already in the buffer as [cmd][numbers]. scan_finish only
        // lets two shapes through: a bare letter (argi 0) and an odd polygon coordinate (argi 1,
        // SVG says draw the complete pairs). Back them out so a replay never meets a truncated
        // command; the dangling number's encoding is recomputed from the scanner's text buffer,
        // which still holds exactly what was encoded.
        size_t drop = 1;
        if (r->sc.argi == 1) {
            pnum_t num; uint8_t b[5];
            num_parse(r->sc.num, r->sc.numlen, &num);
            drop += rec_encode(&num, b);
        }
        if (r->len < drop) return false;   // cannot happen without overflow; stay safe
        r->len -= drop;
    }
    if (polygon) { uint8_t z = 'Z'; rec_put(r, &z, 1); }   // points are always closed
    return !r->overflow;
}

// ---------------------------------------------------------------------------------------------
// Flattener

#define FLAT_MAX_SEGS 64

static inline int32_t to_px8(float v) {
    // Round to nearest 1/8 px; clamp well inside int32 so downstream int arithmetic is safe.
    float s = v * (float)PX8_ONE;
    if (!(s == s)) return 0;
    if (s > 1.0e8f) s = 1.0e8f;
    if (s < -1.0e8f) s = -1.0e8f;
    s += s >= 0 ? 0.5f : -0.5f;
    return (int32_t)s;
}

typedef struct { float x, y; } fpt_t;

static inline fpt_t xform(const path_flattener_t *f, float x, float y) {
    fpt_t p = { f->t.dx + x * f->t.sx, f->t.dy + y * f->t.sy };
    return p;
}

static void emit_dev(path_flattener_t *f, fpt_t p, bool start) {
    f->cb(f->ctx, to_px8(p.x), to_px8(p.y), start);
    f->nverts++;
}

// Emits the contour start (current point) if the subpath has no vertices yet.
static void begin_seg(path_flattener_t *f) {
    if (!f->open) { emit_dev(f, xform(f, f->cx, f->cy), true); f->open = true; }
}

// Flattens a cubic in device space. Segment count from the second-difference bound:
// chord error <= 3/4 * max|Δ²P| / n², so n = ceil(sqrt(3 L / (4 tol))). A straight curve (L = 0)
// is one segment; n is capped so pathological curves cannot blow up vertex counts.
static void flatten_cubic(path_flattener_t *f, fpt_t p0, fpt_t p1, fpt_t p2, fpt_t p3) {
    float ax = p0.x - 2 * p1.x + p2.x, ay = p0.y - 2 * p1.y + p2.y;
    float bx = p1.x - 2 * p2.x + p3.x, by = p1.y - 2 * p2.y + p3.y;
    float l2 = ax * ax + ay * ay, m2 = bx * bx + by * by;
    float L = sqrtf(l2 > m2 ? l2 : m2);
    float tol = f->tol > 0.015625f ? f->tol : 0.015625f;
    float nf = sqrtf(0.75f * L / tol);
    int n = (int)nf; if ((float)n < nf) n++;
    if (n < 1) n = 1;
    if (n > FLAT_MAX_SEGS) n = FLAT_MAX_SEGS;
    float inv = 1.0f / (float)n;
    for (int i = 1; i < n; i++) {
        float t = (float)i * inv, u = 1.0f - t;
        float b0 = u * u * u, b1 = 3 * u * u * t, b2 = 3 * u * t * t, b3 = t * t * t;
        fpt_t p = { b0 * p0.x + b1 * p1.x + b2 * p2.x + b3 * p3.x, b0 * p0.y + b1 * p1.y + b2 * p2.y + b3 * p3.y };
        emit_dev(f, p, false);
    }
    emit_dev(f, p3, false);   // the end point exactly, no accumulated error
}

// Source-space cubic: transform the control points (affine maps Béziers to Béziers) and flatten.
static void cubic_src(path_flattener_t *f, float x1, float y1, float x2, float y2, float x, float y) {
    begin_seg(f);
    flatten_cubic(f, xform(f, f->cx, f->cy), xform(f, x1, y1), xform(f, x2, y2), xform(f, x, y));
    f->lcx = x2; f->lcy = y2;
    f->cx = x; f->cy = y;
}

// Quadratics are degree-elevated to cubics (exact), so one flattener serves both.
static void quad_src(path_flattener_t *f, float qx, float qy, float x, float y) {
    float c1x = f->cx + 2.0f / 3.0f * (qx - f->cx), c1y = f->cy + 2.0f / 3.0f * (qy - f->cy);
    float c2x = x + 2.0f / 3.0f * (qx - x), c2y = y + 2.0f / 3.0f * (qy - y);
    cubic_src(f, c1x, c1y, c2x, c2y, x, y);
    f->lcx = qx; f->lcy = qy;
}

static void line_src(path_flattener_t *f, float x, float y) {
    begin_seg(f);
    f->cx = x; f->cy = y;
    emit_dev(f, xform(f, x, y), false);
}

static float vec_angle(float ux, float uy, float vx, float vy) {
    return atan2f(ux * vy - uy * vx, ux * vx + uy * vy);
}

// SVG arc → up to 4 cubics (implementation notes F.6.5/F.6.6), in source space.
static void arc_src(path_flattener_t *f, float rx, float ry, float phi_deg, bool large, bool sweep, float x2, float y2) {
    float x1 = f->cx, y1 = f->cy;
    if (x1 == x2 && y1 == y2) return;                 // F.6.2: omit entirely
    rx = fabsf(rx); ry = fabsf(ry);
    if (rx == 0 || ry == 0) { line_src(f, x2, y2); return; }
    float phi = phi_deg * (3.14159265358979f / 180.0f);
    float cp = cosf(phi), sp = sinf(phi);
    float dx2 = (x1 - x2) * 0.5f, dy2 = (y1 - y2) * 0.5f;
    float x1p = cp * dx2 + sp * dy2, y1p = -sp * dx2 + cp * dy2;
    float lambda = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
    if (lambda > 1) { float s = sqrtf(lambda); rx *= s; ry *= s; }
    float rx2 = rx * rx, ry2 = ry * ry;
    float num = rx2 * ry2 - rx2 * y1p * y1p - ry2 * x1p * x1p;
    float den = rx2 * y1p * y1p + ry2 * x1p * x1p;
    float coef = den > 0 ? sqrtf(num > 0 ? num / den : 0) : 0;
    if (large == sweep) coef = -coef;
    float cxp = coef * rx * y1p / ry, cyp = -coef * ry * x1p / rx;
    float cx = cp * cxp - sp * cyp + (x1 + x2) * 0.5f, cy = sp * cxp + cp * cyp + (y1 + y2) * 0.5f;
    float ux = (x1p - cxp) / rx, uy = (y1p - cyp) / ry;
    float vx = (-x1p - cxp) / rx, vy = (-y1p - cyp) / ry;
    float th1 = vec_angle(1, 0, ux, uy);
    float dth = vec_angle(ux, uy, vx, vy);
    const float two_pi = 6.28318530717959f;
    if (!sweep && dth > 0) dth -= two_pi;
    else if (sweep && dth < 0) dth += two_pi;
    int nseg = (int)(fabsf(dth) / 1.5707963f) + 1;    // <= 90° per cubic
    if (nseg > 4) nseg = 4;
    float delta = dth / (float)nseg;
    float k = 4.0f / 3.0f * tanf(delta * 0.25f);
    float th = th1;
    for (int i = 0; i < nseg; i++) {
        float ca = cosf(th), sa = sinf(th), cb = cosf(th + delta), sb = sinf(th + delta);
        // E(θ) and E'(θ) of the rotated ellipse.
        float ex0 = cx + rx * cp * ca - ry * sp * sa, ey0 = cy + rx * sp * ca + ry * cp * sa;
        float dx0 = -rx * cp * sa - ry * sp * ca, dy0 = -rx * sp * sa + ry * cp * ca;
        float ex1 = cx + rx * cp * cb - ry * sp * sb, ey1 = cy + rx * sp * cb + ry * cp * sb;
        float dx1 = -rx * cp * sb - ry * sp * cb, dy1 = -rx * sp * sb + ry * cp * cb;
        float endx = (i == nseg - 1) ? x2 : ex1, endy = (i == nseg - 1) ? y2 : ey1;
        cubic_src(f, ex0 + k * dx0, ey0 + k * dy0, ex1 - k * dx1, ey1 - k * dy1, endx, endy);
        th += delta;
    }
}

static bool is_cubic_cmd(uint8_t c) { return c == 'C' || c == 'c' || c == 'S' || c == 's'; }
static bool is_quad_cmd(uint8_t c) { return c == 'Q' || c == 'q' || c == 'T' || c == 't'; }

// Executes one complete command with its (source-space) arguments.
static void flat_exec(path_flattener_t *f, uint8_t cmd, const float *a) {
    float cx = f->cx, cy = f->cy;
    switch (cmd) {
    case 'M': f->cx = a[0]; f->cy = a[1]; f->sx0 = f->cx; f->sy0 = f->cy; f->open = false; break;
    case 'm': f->cx = cx + a[0]; f->cy = cy + a[1]; f->sx0 = f->cx; f->sy0 = f->cy; f->open = false; break;
    case 'L': line_src(f, a[0], a[1]); break;
    case 'l': line_src(f, cx + a[0], cy + a[1]); break;
    case 'H': line_src(f, a[0], cy); break;
    case 'h': line_src(f, cx + a[0], cy); break;
    case 'V': line_src(f, cx, a[0]); break;
    case 'v': line_src(f, cx, cy + a[0]); break;
    case 'C': cubic_src(f, a[0], a[1], a[2], a[3], a[4], a[5]); break;
    case 'c': cubic_src(f, cx + a[0], cy + a[1], cx + a[2], cy + a[3], cx + a[4], cy + a[5]); break;
    case 'S': case 's': {
        float x1 = cx, y1 = cy;
        if (is_cubic_cmd(f->last_cmd)) { x1 = 2 * cx - f->lcx; y1 = 2 * cy - f->lcy; }
        if (cmd == 'S') cubic_src(f, x1, y1, a[0], a[1], a[2], a[3]);
        else cubic_src(f, x1, y1, cx + a[0], cy + a[1], cx + a[2], cy + a[3]);
        break;
    }
    case 'Q': quad_src(f, a[0], a[1], a[2], a[3]); break;
    case 'q': quad_src(f, cx + a[0], cy + a[1], cx + a[2], cy + a[3]); break;
    case 'T': case 't': {
        float qx = cx, qy = cy;
        if (is_quad_cmd(f->last_cmd)) { qx = 2 * cx - f->lcx; qy = 2 * cy - f->lcy; }
        if (cmd == 'T') quad_src(f, qx, qy, a[0], a[1]);
        else quad_src(f, qx, qy, cx + a[0], cy + a[1]);
        break;
    }
    case 'A': arc_src(f, a[0], a[1], a[2], a[3] != 0, a[4] != 0, a[5], a[6]); break;
    case 'a': arc_src(f, a[0], a[1], a[2], a[3] != 0, a[4] != 0, cx + a[5], cy + a[6]); break;
    case 'Z': case 'z': f->cx = f->sx0; f->cy = f->sy0; f->open = false; break;
    default: return;
    }
    f->last_cmd = cmd;
}

void path_flattener_init(path_flattener_t *f, const affine_t *t, float tolerance_px, path_vertex_cb cb, void *ctx) {
    memset(f, 0, sizeof *f);
    f->t = *t;
    // A non-finite transform component would poison every vertex; a zero scale is the documented
    // degenerate (collapsed but valid output).
    if (!(f->t.sx == f->t.sx)) f->t.sx = 0; if (!(f->t.sy == f->t.sy)) f->t.sy = 0;
    if (!(f->t.dx == f->t.dx)) f->t.dx = 0; if (!(f->t.dy == f->t.dy)) f->t.dy = 0;
    f->tol = tolerance_px;
    f->cb = cb; f->ctx = ctx;
}

static bool flat_on_cmd(void *ctx, uint8_t cmd) {
    path_flattener_t *f = (path_flattener_t *)ctx;
    if (cmd == 'Z' || cmd == 'z') flat_exec(f, cmd, NULL);
    return true;
}

static bool flat_on_num(void *ctx, uint8_t argi, const pnum_t *num) {
    path_flattener_t *f = (path_flattener_t *)ctx;
    if (argi >= 8) return false;
    f->sc.args[argi] = num->value;
    if ((int)argi + 1 == cmd_nargs(f->sc.cmd)) flat_exec(f, f->sc.cmd, f->sc.args);
    return true;
}

static const scan_sink_t flat_sink = { flat_on_cmd, flat_on_num };

bool path_flattener_feed(path_flattener_t *f, const char *text, size_t len) {
    return scan_feed(&f->sc, text, len, false, &flat_sink, f);
}

bool path_flattener_feed_points(path_flattener_t *f, const char *text, size_t len) {
    f->polygon_mode = true;
    return scan_feed(&f->sc, text, len, true, &flat_sink, f);
}

bool path_flattener_finish(path_flattener_t *f) {
    bool ok = scan_finish(&f->sc, f->polygon_mode, &flat_sink, f);
    if (f->polygon_mode && ok) flat_exec(f, 'Z', NULL);
    return ok;
}

bool path_flattener_replay(path_flattener_t *f, const uint8_t *rec, size_t len) {
    size_t i = 0;
    float a[8];
    while (i < len) {
        uint8_t cmd = rec[i++];
        if (cmd == REC_POLY_MARK) continue;
        int n = cmd_nargs(cmd);
        if (n < 0) return false;
        for (int k = 0; k < n; k++) {
            if (i >= len) return false;   // truncated recording
            uint8_t b = rec[i++];
            if (b < 0xF0) { a[k] = dec_to_float((int32_t)b - 120, 1); continue; }
            if (b == 0xF0 || b == 0xF1) {
                int16_t v; if (i + 2 > len) return false;
                memcpy(&v, rec + i, 2); i += 2;
                a[k] = dec_to_float(v, b == 0xF0 ? 1 : 2);
            } else if (b == 0xF2 || b == 0xF3) {
                int32_t v; if (i + 4 > len) return false;
                memcpy(&v, rec + i, 4); i += 4;
                a[k] = dec_to_float(v, b == 0xF2 ? 2 : 0);
            } else if (b == 0xF4) {
                float v; if (i + 4 > len) return false;
                memcpy(&v, rec + i, 4); i += 4;
                a[k] = v;
            } else return false;
        }
        flat_exec(f, cmd, a);
    }
    return true;
}

bool path_flatten_string(const char *d, const affine_t *t, float tol, path_vertex_cb cb, void *ctx) {
    path_flattener_t f;
    path_flattener_init(&f, t, tol, cb, ctx);
    bool ok = path_flattener_feed(&f, d, strlen(d));
    return path_flattener_finish(&f) && ok;
}
