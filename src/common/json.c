// Streaming JSON tokenizer (see json.h). One explicit state machine driven byte by byte; the only
// lookahead-free construct in JSON is the number, which is terminated by the first byte that
// cannot continue it — that byte is then re-processed in the "after value" state.
//
// Event/depth convention (consumers navigate by (depth, key, index)):
//   * every value event — scalars, OBJ_START, ARR_START — is delivered at the depth of the
//     container it lives in, with js->index[depth-1] / js->key[depth-1] describing its slot;
//   * OBJ_END / ARR_END are delivered after the pop, i.e. at the same depth as their START.
// So stack[i], index[i] and key[i] all describe the i-th open container (0-based).
#include "json.h"
#include <string.h>

enum {
    ST_VALUE,       // expecting a value
    ST_ARR_FIRST,   // after '[': a value or ']'
    ST_OBJ_FIRST,   // after '{': a key or '}'
    ST_OBJ_KEY,     // after ',' in an object: a key
    ST_STRING,      // inside a string; in_key says whether it is a key or a value
    ST_ESCAPE,      // after '\'
    ST_UHEX,        // inside \uXXXX; sub = hex digits consumed so far
    ST_COLON,       // between a key and its value
    ST_NUMBER,      // sub = number grammar sub-state (see number_step)
    ST_LITERAL,     // sub = first letter of true/false/null, numlen = letters matched
    ST_AFTER,       // after a value inside a container: ',' or the closing bracket
    ST_DONE,        // top-level value complete; everything else is ignored
    ST_ERROR,
};

static bool is_ws(char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; }
static bool is_digit(char c) { return c >= '0' && c <= '9'; }

static bool fail(json_stream_t *js) {
    js->error = true;
    js->st = ST_ERROR;
    return false;
}

static bool emit(json_stream_t *js, json_event_t ev, const char *data, size_t len, bool final) {
    if (js->cb(js->ctx, js, ev, data, len, final)) return true;
    return fail(js);
}

// Decoded bytes of an escape go either into the key buffer or straight out as a string chunk.
static bool emit_bytes(json_stream_t *js, const char *b, size_t n) {
    if (js->in_key) {
        size_t room = JSON_KEY_MAX - js->keylen;
        if (n > room) n = room;    // keys longer than JSON_KEY_MAX are silently truncated
        memcpy(js->keybuf + js->keylen, b, n);
        js->keylen += (uint8_t)n;
        return true;
    }
    return emit(js, JSON_EV_STRING, b, n, false);
}

static bool emit_codepoint(json_stream_t *js, uint32_t cp) {
    char b[4];
    size_t n;
    if (cp < 0x80) { b[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { b[0] = (char)(0xC0 | (cp >> 6)); b[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) {
        b[0] = (char)(0xE0 | (cp >> 12)); b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[2] = (char)(0x80 | (cp & 0x3F)); n = 3;
    } else {
        b[0] = (char)(0xF0 | (cp >> 18)); b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        b[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[3] = (char)(0x80 | (cp & 0x3F)); n = 4;
    }
    return emit_bytes(js, b, n);
}

// A high surrogate not followed by a low one becomes U+FFFD (lenient, like browsers) rather than
// aborting the frame.
static bool flush_pending_surrogate(json_stream_t *js) {
    if (!js->pending_surrogate) return true;
    js->pending_surrogate = 0;
    return emit_codepoint(js, 0xFFFD);
}

static bool value_done(json_stream_t *js) {
    if (js->depth == 0) { js->complete = true; js->st = ST_DONE; }
    else js->st = ST_AFTER;
    return true;
}

static bool push(json_stream_t *js, char kind) {
    if (js->depth >= JSON_MAX_DEPTH) return fail(js);
    if (!emit(js, kind == 'o' ? JSON_EV_OBJ_START : JSON_EV_ARR_START, NULL, 0, true)) return false;
    js->stack[js->depth] = (uint8_t)kind;
    js->index[js->depth] = 0;
    js->key[js->depth][0] = 0;
    js->depth++;
    js->st = kind == 'o' ? ST_OBJ_FIRST : ST_ARR_FIRST;
    return true;
}

static bool pop(json_stream_t *js, char kind) {
    if (js->depth == 0 || js->stack[js->depth - 1] != (uint8_t)kind) return fail(js);
    js->depth--;
    if (!emit(js, kind == 'o' ? JSON_EV_OBJ_END : JSON_EV_ARR_END, NULL, 0, true)) return false;
    return value_done(js);
}

static bool begin_key(json_stream_t *js) {
    js->in_key = true;
    js->keylen = 0;
    js->st = ST_STRING;
    return true;
}

// First byte of a value. Returns false on error.
static bool begin_value(json_stream_t *js, char c) {
    switch (c) {
    case '{': return push(js, 'o');
    case '[': return push(js, 'a');
    case '"': js->in_key = false; js->st = ST_STRING; return true;
    case 't': case 'f': case 'n':
        js->st = ST_LITERAL; js->sub = (uint8_t)c; js->numlen = 1; return true;
    default:
        if (c == '-' || is_digit(c)) {
            js->st = ST_NUMBER;
            js->num[0] = c; js->numlen = 1;
            js->sub = c == '-' ? 0 : (c == '0' ? 1 : 2);
            return true;
        }
        return fail(js);
    }
}

static bool number_terminal(uint8_t sub) { return sub == 1 || sub == 2 || sub == 4 || sub == 7; }

static bool end_number(json_stream_t *js) {
    if (!number_terminal(js->sub)) return fail(js);
    if (!emit(js, JSON_EV_NUMBER, js->num, js->numlen, true)) return false;
    return value_done(js);
}

// Number grammar: -? (0 | [1-9][0-9]*) (. [0-9]+)? ([eE] [+-]? [0-9]+)?
// Returns 1 if c was consumed, 0 if the number ended before c, -1 on error.
static int number_step(json_stream_t *js, char c) {
    uint8_t s = js->sub, ns;
    bool d = is_digit(c);
    switch (s) {
    case 0: if (c == '0') ns = 1; else if (d) ns = 2; else return -1; break;
    case 1: if (c == '.') ns = 3; else if (c == 'e' || c == 'E') ns = 5; else if (d) return -1; else return 0; break;
    case 2: if (d) ns = 2; else if (c == '.') ns = 3; else if (c == 'e' || c == 'E') ns = 5; else return 0; break;
    case 3: if (d) ns = 4; else return -1; break;
    case 4: if (d) ns = 4; else if (c == 'e' || c == 'E') ns = 5; else return 0; break;
    case 5: if (c == '+' || c == '-') ns = 6; else if (d) ns = 7; else return -1; break;
    case 6: if (d) ns = 7; else return -1; break;
    default: if (d) ns = 7; else return 0; break;
    }
    if (js->numlen >= JSON_NUM_MAX) return -1;
    js->num[js->numlen++] = c;
    js->sub = ns;
    return 1;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool end_uescape(json_stream_t *js) {
    uint32_t cp = js->uescape;
    if (js->pending_surrogate) {
        if (cp >= 0xDC00 && cp <= 0xDFFF) {
            cp = 0x10000 + (((uint32_t)js->pending_surrogate - 0xD800) << 10) + (cp - 0xDC00);
            js->pending_surrogate = 0;
            return emit_codepoint(js, cp);
        }
        if (!flush_pending_surrogate(js)) return false;
    }
    if (cp >= 0xD800 && cp <= 0xDBFF) { js->pending_surrogate = (uint16_t)cp; return true; }
    if (cp >= 0xDC00 && cp <= 0xDFFF) cp = 0xFFFD;   // lone low surrogate
    return emit_codepoint(js, cp);
}

void json_stream_init(json_stream_t *js, json_cb_t cb, void *ctx) {
    memset(js, 0, sizeof *js);
    js->cb = cb;
    js->ctx = ctx;
    js->st = ST_VALUE;
}

bool json_stream_feed(json_stream_t *js, const char *data, size_t len) {
    if (js->error) return false;
    size_t i = 0;
    while (i < len) {
        char c = data[i];
        switch (js->st) {
        case ST_DONE:
            return true;
        case ST_ERROR:
            return false;

        case ST_STRING: {
            // A high surrogate is only completed by an immediately following \uDCxx, so it stays
            // pending while the next byte is a backslash (ST_ESCAPE decides) or not yet received.
            if (js->pending_surrogate && c != '\\' && !flush_pending_surrogate(js)) return false;
            // Deliver the longest run of plain bytes straight from the caller's buffer.
            size_t start = i;
            while (i < len && data[i] != '"' && data[i] != '\\' && (unsigned char)data[i] >= 0x20) i++;
            bool closing = i < len && data[i] == '"';
            if (js->in_key) {
                if (i > start && !emit_bytes(js, data + start, i - start)) return false;
            } else if (i > start || closing) {
                if (!emit(js, JSON_EV_STRING, data + start, i - start, closing)) return false;
            }
            if (i >= len) continue;   // chunk ended mid-string; the loop condition ends the feed
            c = data[i++];
            if (closing) {
                if (js->in_key) {
                    js->keybuf[js->keylen] = 0;
                    memcpy(js->key[js->depth - 1], js->keybuf, (size_t)js->keylen + 1);
                    js->in_key = false;
                    if (!emit(js, JSON_EV_KEY, js->keybuf, js->keylen, true)) return false;
                    js->st = ST_COLON;
                } else {
                    value_done(js);
                }
            } else if (c == '\\') {
                js->st = ST_ESCAPE;
            } else {
                return fail(js);   // raw control character
            }
            continue;
        }

        case ST_ESCAPE: {
            i++;
            char b;
            switch (c) {
            case '"': b = '"'; break;
            case '\\': b = '\\'; break;
            case '/': b = '/'; break;
            case 'b': b = '\b'; break;
            case 'f': b = '\f'; break;
            case 'n': b = '\n'; break;
            case 'r': b = '\r'; break;
            case 't': b = '\t'; break;
            case 'u': js->st = ST_UHEX; js->sub = 0; js->uescape = 0; continue;
            default: return fail(js);
            }
            if (!flush_pending_surrogate(js)) return false;
            if (!emit_bytes(js, &b, 1)) return false;
            js->st = ST_STRING;
            continue;
        }

        case ST_UHEX: {
            i++;
            int v = hexval(c);
            if (v < 0) return fail(js);
            js->uescape = (uint16_t)((js->uescape << 4) | (unsigned)v);
            if (++js->sub == 4) {
                if (!end_uescape(js)) return false;
                js->st = ST_STRING;
            }
            continue;
        }

        case ST_NUMBER: {
            int r = number_step(js, c);
            if (r < 0) return fail(js);
            if (r > 0) { i++; continue; }
            if (!end_number(js)) return false;
            continue;   // re-process c in the new state
        }

        case ST_LITERAL: {
            i++;
            const char *lit = js->sub == 't' ? "true" : js->sub == 'f' ? "false" : "null";
            if (lit[js->numlen] != c) return fail(js);
            js->numlen++;
            if (lit[js->numlen] == 0) {
                json_event_t ev = js->sub == 't' ? JSON_EV_TRUE : js->sub == 'f' ? JSON_EV_FALSE : JSON_EV_NULL;
                if (!emit(js, ev, NULL, 0, true)) return false;
                value_done(js);
            }
            continue;
        }

        default:
            break;
        }

        // Token-boundary states: whitespace is insignificant.
        i++;
        if (is_ws(c)) continue;
        switch (js->st) {
        case ST_VALUE:
            if (!begin_value(js, c)) return false;
            break;
        case ST_ARR_FIRST:
            if (c == ']') { if (!pop(js, 'a')) return false; }
            else if (!begin_value(js, c)) return false;
            break;
        case ST_OBJ_FIRST:
            if (c == '}') { if (!pop(js, 'o')) return false; }
            else if (c == '"') begin_key(js);
            else return fail(js);
            break;
        case ST_OBJ_KEY:
            if (c == '"') begin_key(js);
            else return fail(js);
            break;
        case ST_COLON:
            if (c != ':') return fail(js);
            js->st = ST_VALUE;
            break;
        case ST_AFTER:
            if (c == ',') {
                js->index[js->depth - 1]++;
                js->st = js->stack[js->depth - 1] == 'o' ? ST_OBJ_KEY : ST_VALUE;
            } else if (c == ']') {
                if (!pop(js, 'a')) return false;
            } else if (c == '}') {
                if (!pop(js, 'o')) return false;
            } else {
                return fail(js);
            }
            break;
        default:
            return fail(js);
        }
    }
    return true;
}

bool json_stream_finish(json_stream_t *js) {
    if (js->error) return false;
    if (js->complete) return true;
    // A top-level number has no terminator other than end of input.
    if (js->st == ST_NUMBER && js->depth == 0 && end_number(js)) return true;
    fail(js);
    return false;
}

// ---- number text helpers ------------------------------------------------------------------

typedef struct {
    bool neg;
    uint64_t mant;      // significant digits, at most 18 kept (< 10^18)
    int exp10;          // value = mant * 10^exp10
    bool sticky;        // dropped digits were not all zero
} decnum_t;

// Validates the JSON number grammar and splits the text into mantissa and decimal exponent.
static bool parse_dec(const char *t, size_t len, decnum_t *d) {
    size_t i = 0;
    memset(d, 0, sizeof *d);
    if (i < len && t[i] == '-') { d->neg = true; i++; }
    if (i >= len || !is_digit(t[i])) return false;
    bool leading_zero = t[i] == '0';
    size_t int_start = i;
    while (i < len && is_digit(t[i])) {
        unsigned dig = (unsigned)(t[i] - '0');
        if (d->mant < 100000000000000000ULL) d->mant = d->mant * 10 + dig;
        else { d->exp10++; if (dig) d->sticky = true; }
        i++;
    }
    if (leading_zero && i - int_start > 1) return false;
    if (i < len && t[i] == '.') {
        i++;
        if (i >= len || !is_digit(t[i])) return false;
        while (i < len && is_digit(t[i])) {
            unsigned dig = (unsigned)(t[i] - '0');
            if (d->mant < 100000000000000000ULL) { d->mant = d->mant * 10 + dig; d->exp10--; }
            else if (dig) d->sticky = true;
            i++;
        }
    }
    if (i < len && (t[i] == 'e' || t[i] == 'E')) {
        i++;
        bool eneg = false;
        if (i < len && (t[i] == '+' || t[i] == '-')) { eneg = t[i] == '-'; i++; }
        if (i >= len || !is_digit(t[i])) return false;
        int e = 0;
        while (i < len && is_digit(t[i])) {
            if (e < 10000) e = e * 10 + (t[i] - '0');   // clamp: anything this big saturates anyway
            i++;
        }
        d->exp10 += eneg ? -e : e;
    }
    return i == len;
}

static const uint64_t pow10_u64[20] = {
    1ULL, 10ULL, 100ULL, 1000ULL, 10000ULL, 100000ULL, 1000000ULL, 10000000ULL, 100000000ULL,
    1000000000ULL, 10000000000ULL, 100000000000ULL, 1000000000000ULL, 10000000000000ULL,
    100000000000000ULL, 1000000000000000ULL, 10000000000000000ULL, 100000000000000000ULL,
    1000000000000000000ULL, 10000000000000000000ULL,
};

static int32_t saturate(bool neg, uint64_t mag) {
    if (neg) return mag >= 0x80000000ULL ? INT32_MIN : -(int32_t)mag;
    return mag >= 0x7FFFFFFFULL ? INT32_MAX : (int32_t)mag;
}

bool json_number_to_fixed(const char *text, size_t len, int frac_bits, int32_t *out) {
    decnum_t d;
    if (frac_bits < 0 || frac_bits > 30 || !parse_dec(text, len, &d)) return false;
    uint64_t m = d.mant;
    int e = d.exp10;
    if (m == 0) { *out = 0; return true; }
    // Fold 2^frac_bits into the mantissa. If that would overflow, trade trailing decimal digits
    // for headroom: at least 64 - frac_bits >= 34 bits of mantissa survive, more than an int32
    // result can express, so the rounding below is unaffected.
    while (m > (UINT64_MAX >> frac_bits)) { if (m % 10) d.sticky = true; m /= 10; e++; }
    m <<= frac_bits;
    if (e > 0) {
        while (e > 0) {
            if (m >= 0x80000000ULL) { *out = saturate(d.neg, 0x80000000ULL); return true; }
            m *= 10; e--;
        }
        *out = saturate(d.neg, m);
        return true;
    }
    uint64_t q;
    if (-e >= 20) {
        q = 0;   // m < 2^64 < 5*10^19, so the value is below one half
    } else {
        uint64_t den = pow10_u64[-e];
        q = m / den;
        uint64_t r = m % den;
        // Round to nearest, ties away from zero.
        if (r >= den - r) q++;
    }
    *out = saturate(d.neg, q);
    return true;
}

bool json_number_to_int(const char *text, size_t len, int32_t *out) {
    return json_number_to_fixed(text, len, 0, out);
}

bool json_number_to_float(const char *text, size_t len, float *out) {
    decnum_t d;
    if (!parse_dec(text, len, &d)) return false;
    // Decode-time only: double keeps the mantissa exact and one rounding step at the end.
    double v = (double)d.mant;
    int e = d.exp10;
    if (e > 350) e = 350;
    if (e < -350) e = -350;
    while (e > 0) { v *= 10.0; e--; }
    while (e < 0) { v /= 10.0; e++; }
    if (!(v <= 3.4028234663852886e38)) v = 3.4028234663852886e38;   // also catches inf
    *out = (float)(d.neg ? -v : v);
    return true;
}
