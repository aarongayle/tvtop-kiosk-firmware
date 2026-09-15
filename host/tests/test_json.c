// json.c tests: chunk-boundary independence on real fixtures, escapes, depth, malformed input,
// number helpers. Run from the repo root (fixture paths are relative).
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

// ---- event recorder: string chunks are concatenated so chunking does not affect the record ----
typedef struct {
    char *buf; size_t len, cap;
    char str[32768]; size_t strlen_;   // pending string value
    bool overflow;
} rec_t;

static void rec_put(rec_t *r, const void *d, size_t n) {
    if (r->len + n > r->cap) {
        r->cap = (r->len + n) * 2 + 1024;
        r->buf = realloc(r->buf, r->cap);
    }
    memcpy(r->buf + r->len, d, n);
    r->len += n;
}

static bool rec_cb(void *ctx, const json_stream_t *js, json_event_t ev, const char *data, size_t len, bool final) {
    rec_t *r = ctx;
    if (ev == JSON_EV_STRING) {
        if (r->strlen_ + len > sizeof r->str) { r->overflow = true; return false; }
        memcpy(r->str + r->strlen_, data, len);
        r->strlen_ += len;
        if (!final) return true;
        data = r->str; len = r->strlen_; r->strlen_ = 0;
    }
    char hdr[96];
    int n = snprintf(hdr, sizeof hdr, "%d d%u i%u k%s:", (int)ev, js->depth,
                     js->depth ? js->index[js->depth - 1] : 0,
                     js->depth ? js->key[js->depth - 1] : "");
    rec_put(r, hdr, (size_t)n);
    rec_put(r, data, len);
    rec_put(r, "\n", 1);
    return true;
}

// Parses text in chunks of `chunk` bytes; returns the feed/finish result and fills the record.
static bool run(const char *text, size_t len, size_t chunk, rec_t *r) {
    memset(r, 0, sizeof *r);
    json_stream_t js;
    json_stream_init(&js, rec_cb, r);
    bool ok = true;
    for (size_t i = 0; i < len && ok; i += chunk) {
        size_t n = len - i < chunk ? len - i : chunk;
        ok = json_stream_feed(&js, text + i, n);
    }
    ok = json_stream_finish(&js) && ok;
    return ok;
}

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { printf("cannot open %s (run from the repo root)\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) exit(1);
    fclose(f);
    b[n] = 0;
    *len = (size_t)n;
    return b;
}

static void test_fixture_chunking(const char *path) {
    size_t len;
    char *text = read_file(path, &len);
    rec_t whole, one, seven, big;
    CHECK(run(text, len, len, &whole));
    CHECK(run(text, len, 1, &one));
    CHECK(run(text, len, 7, &seven));
    CHECK(run(text, len, 1460, &big));
    CHECK(whole.len > 0);
    CHECK(whole.len == one.len && memcmp(whole.buf, one.buf, whole.len) == 0);
    CHECK(whole.len == seven.len && memcmp(whole.buf, seven.buf, whole.len) == 0);
    CHECK(whole.len == big.len && memcmp(whole.buf, big.buf, whole.len) == 0);
    printf("%s: %zu bytes -> %zu bytes of events\n", path, len, whole.len);
    free(whole.buf); free(one.buf); free(seven.buf); free(big.buf); free(text);
}

// Every split point of a small input must give the same record as feeding it whole.
static void expect_all_splits(const char *text, const char *expect) {
    size_t len = strlen(text);
    rec_t whole;
    CHECK(run(text, len, len, &whole));
    if (!(whole.len == strlen(expect) && memcmp(whole.buf, expect, whole.len) == 0)) {
        failures++;
        printf("FAIL record mismatch for %s\n  got:\n%.*s  want:\n%s", text, (int)whole.len, whole.buf, expect);
    }
    for (size_t split = 1; split < len; split++) {
        rec_t r; memset(&r, 0, sizeof r);
        json_stream_t js;
        json_stream_init(&js, rec_cb, &r);
        bool ok = json_stream_feed(&js, text, split) && json_stream_feed(&js, text + split, len - split) && json_stream_finish(&js);
        CHECK(ok);
        if (!(r.len == whole.len && memcmp(r.buf, whole.buf, r.len) == 0)) {
            failures++;
            printf("FAIL split at %zu differs for %s\n", split, text);
        }
        free(r.buf);
    }
    free(whole.buf);
}

static void expect_error(const char *text) {
    rec_t r;
    bool ok = run(text, strlen(text), strlen(text), &r);
    if (ok) { failures++; printf("FAIL accepted malformed input: %s\n", text); }
    free(r.buf);
    // Byte at a time must fail too, and must not crash.
    ok = run(text, strlen(text), 1, &r);
    if (ok) { failures++; printf("FAIL accepted malformed input (1-byte feed): %s\n", text); }
    free(r.buf);
}

static void expect_ok(const char *text) {
    rec_t r;
    bool ok = run(text, strlen(text), strlen(text), &r);
    if (!ok) { failures++; printf("FAIL rejected valid input: %s\n", text); }
    free(r.buf);
}

static void test_escapes(void) {
    // \u00e9 -> C3 A9, surrogate pair \ud83d\ude00 -> F0 9F 98 80
    expect_all_splits("\"a\\u00e9\\ud83d\\ude00\\n\\\"\\\\\\/\\b\\f\\r\\t\\u0041\"",
                      "5 d0 i0 k:a\xC3\xA9\xF0\x9F\x98\x80\n\"\\/\b\f\r\t" "A\n");
    // Lone surrogates become U+FFFD instead of aborting the frame.
    expect_all_splits("[\"\\ud83dx\"]", "2 d0 i0 k:\n5 d1 i0 k:\xEF\xBF\xBDx\n3 d0 i0 k:\n");
    expect_all_splits("[\"\\ude00\"]", "2 d0 i0 k:\n5 d1 i0 k:\xEF\xBF\xBD\n3 d0 i0 k:\n");
    expect_all_splits("[\"\\ud83d\"]", "2 d0 i0 k:\n5 d1 i0 k:\xEF\xBF\xBD\n3 d0 i0 k:\n");
    expect_all_splits("[\"\\ud83d\\u0041\"]", "2 d0 i0 k:\n5 d1 i0 k:\xEF\xBF\xBD" "A\n3 d0 i0 k:\n");
    // Upper-case hex, 2- and 3-byte sequences, escapes in keys.
    expect_all_splits("{\"k\\u00FCy\\u20ac\":\"\"}",
                      "0 d0 i0 k:\n4 d1 i0 kk\xC3\xBCy\xE2\x82\xAC:k\xC3\xBCy\xE2\x82\xAC\n5 d1 i0 kk\xC3\xBCy\xE2\x82\xAC:\n1 d0 i0 k:\n");
    // Empty strings and string chunks: an empty final chunk must still yield exactly one record.
    expect_all_splits("[\"\",\"x\"]", "2 d0 i0 k:\n5 d1 i0 k:\n5 d1 i1 k:x\n3 d0 i0 k:\n");
}

static void test_navigation(void) {
    // Keys/indices as seen by the callback: START at the parent's depth, END after the pop.
    expect_all_splits("{\"a\":[1,{\"b\":true},null],\"c\":false}",
                      "0 d0 i0 k:\n"
                      "4 d1 i0 ka:a\n"
                      "2 d1 i0 ka:\n"
                      "6 d2 i0 k:1\n"
                      "0 d2 i1 k:\n"
                      "4 d3 i0 kb:b\n"
                      "7 d3 i0 kb:\n"
                      "1 d2 i1 k:\n"
                      "9 d2 i2 k:\n"
                      "3 d1 i0 ka:\n"
                      "4 d1 i1 kc:c\n"
                      "8 d1 i1 kc:\n"
                      "1 d0 i0 k:\n");
    // Whitespace everywhere, top-level scalars, trailing garbage after completion is ignored.
    expect_all_splits(" \n\t{ \"a\" : [ 1 , 2 ] , \"b\" : { } } \r\n",
                      "0 d0 i0 k:\n4 d1 i0 ka:a\n2 d1 i0 ka:\n6 d2 i0 k:1\n6 d2 i1 k:2\n3 d1 i0 ka:\n"
                      "4 d1 i1 kb:b\n0 d1 i1 kb:\n1 d1 i1 kb:\n1 d0 i0 k:\n");
    expect_all_splits("42", "6 d0 i0 k:42\n");
    expect_all_splits("\"s\"", "5 d0 i0 k:s\n");
    expect_all_splits("true", "7 d0 i0 k:\n");
    expect_all_splits("[] trailing junk", "2 d0 i0 k:\n3 d0 i0 k:\n");
    expect_all_splits("-1.5e+2 x", "6 d0 i0 k:-1.5e+2\n");
    // Number terminated by a chunk boundary must not split into two numbers.
    expect_all_splits("[1757548800123,2]", "2 d0 i0 k:\n6 d1 i0 k:1757548800123\n6 d1 i1 k:2\n3 d0 i0 k:\n");
}

static void test_key_truncation(void) {
    const char *text = "{\"abcdefghijklmnopqrstuvwxyz0123\":1}";
    rec_t r;
    CHECK(run(text, strlen(text), strlen(text), &r));
    const char *want = "0 d0 i0 k:\n4 d1 i0 kabcdefghijklmnopqrstuvwx:abcdefghijklmnopqrstuvwx\n"
                       "6 d1 i0 kabcdefghijklmnopqrstuvwx:1\n1 d0 i0 k:\n";
    CHECK(r.len == strlen(want) && memcmp(r.buf, want, r.len) == 0);
    free(r.buf);
}

static void test_depth(void) {
    char deep[8 * JSON_MAX_DEPTH + 16];
    // JSON_MAX_DEPTH nested arrays is fine; one more is an error.
    memset(deep, '[', JSON_MAX_DEPTH); memset(deep + JSON_MAX_DEPTH, ']', JSON_MAX_DEPTH); deep[2 * JSON_MAX_DEPTH] = 0;
    expect_ok(deep);
    memset(deep, '[', JSON_MAX_DEPTH + 1); memset(deep + JSON_MAX_DEPTH + 1, ']', JSON_MAX_DEPTH + 1); deep[2 * JSON_MAX_DEPTH + 2] = 0;
    expect_error(deep);
    // Depth is counted per container, not per value; mixed nesting counts too.
    memset(deep, 0, sizeof deep);
    for (int i = 0; i < JSON_MAX_DEPTH + 1; i++) strcat(deep, "{\"a\":");
    strcat(deep, "1");
    expect_error(deep);
}

static void test_malformed(void) {
    const char *bad[] = {
        "", " ", "{", "[", "{\"a\":}", "[1,]", "{\"a\":1,}", "[1 2]", "{\"a\" 1}", "{\"a\":1 \"b\":2}",
        "{a:1}", "{\"a\"}", "tru", "nul", "fals", "[tru]", "[truee]", "01", "1.", ".5", "+1", "-", "-.5",
        "1e", "1e+", "1.e5", "\"abc", "\"\\x\"", "\"\\u12G4\"", "\"\\u12\"", "\"a\tb\"", "\"a\nb\"",
        "]", "}", "[}", "{]", ",", ":",
        "12345678901234567890123456789012",   // > JSON_NUM_MAX digits
        "[\"a\",]", "nan", "Infinity", "'a'", "[1,,2]", "{\"a\":1,,\"b\":2}", "{,}", "[,]",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) expect_error(bad[i]);
    // Anything after a complete top-level value is ignored (json.h), so these are valid: the
    // junk arrives after completion. "truee" is "true" followed by junk for the same reason.
    expect_ok("[1]]");
    expect_ok("{\"a\":1}}");
    expect_ok("1,");
    expect_ok("\"a\":1");
    expect_ok("truee");
    expect_ok("0");
    expect_ok("-0");
    expect_ok("1E5");
    expect_ok("[0.5, -0.5e-3, 1e+3]");
    expect_ok("{\"\":\"\"}");
    expect_ok("[[],{},\"\",0,true,false,null]");
}

static bool abort_cb(void *ctx, const json_stream_t *js, json_event_t ev, const char *data, size_t len, bool final) {
    (void)js; (void)data; (void)len; (void)final;
    int *count = ctx;
    return ++*count < 3 || ev != JSON_EV_NUMBER;
}

static void test_callback_abort(void) {
    json_stream_t js;
    int count = 0;
    json_stream_init(&js, abort_cb, &count);
    const char *text = "[1,2,3,4]";
    CHECK(!json_stream_feed(&js, text, strlen(text)));
    CHECK(js.error);
    CHECK(!json_stream_finish(&js));
    CHECK(!json_stream_feed(&js, "]", 1));   // stays failed
}

static void test_numbers(void) {
    int32_t v; float f;
#define TOINT(s, want) do { CHECK(json_number_to_int(s, strlen(s), &v)); CHECK(v == (want)); if (v != (want)) printf("  %s -> %d\n", s, v); } while (0)
#define TOFIX(s, bits, want) do { CHECK(json_number_to_fixed(s, strlen(s), bits, &v)); CHECK(v == (want)); if (v != (want)) printf("  %s @%d -> %d\n", s, bits, v); } while (0)
    TOINT("1", 1); TOINT("0", 0); TOINT("-0", 0); TOINT("-0.5", -1); TOINT("0.5", 1); TOINT("0.49", 0);
    TOINT("1e3", 1000); TOINT("2.5e-1", 0); TOINT("2.5e-1", 0); TOINT("7.5e-1", 1); TOINT("1E2", 100);
    TOINT("1757548800123", INT32_MAX); TOINT("-1757548800123", INT32_MIN);
    TOINT("2147483647", INT32_MAX); TOINT("2147483648", INT32_MAX); TOINT("-2147483648", INT32_MIN);
    TOINT("-2147483649", INT32_MIN); TOINT("1e99", INT32_MAX); TOINT("-1e99", INT32_MIN); TOINT("1e-99", 0);
    TOINT("123456789012345678901234567890", INT32_MAX);
    TOINT("0.000000000000000000000000000001", 0);
    TOINT("1234.5", 1235); TOINT("-1234.5", -1235); TOINT("1234.4999", 1234);
    TOFIX("0.0625", 3, 1);      // 0.5 px8: tie rounds away from zero
    TOFIX("-0.0625", 3, -1);
    TOFIX("0.0624", 3, 0); TOFIX("0.0626", 3, 1); TOFIX("0.1875", 3, 2); TOFIX("0.125", 3, 1);
    TOFIX("1", 3, 8); TOFIX("1.5", 3, 12); TOFIX("640.25", 3, 5122); TOFIX("1e3", 3, 8000);
    TOFIX("2.5e-1", 3, 2); TOFIX("1.23e2", 3, 984); TOFIX("12300e-2", 3, 984);
    TOFIX("268435455", 3, 2147483640); TOFIX("268435456", 3, INT32_MAX); TOFIX("-268435456", 3, INT32_MIN);
    TOFIX("0.5", 16, 32768); TOFIX("0.000015258789", 16, 1);   // 2^-16 = 0.0000152587890625
    TOFIX("1", 30, 1 << 30); TOFIX("2", 30, INT32_MAX);
    // Long mantissas with large frac_bits: the mantissa must be trimmed, not overflowed.
    TOFIX("1.234567890123456789", 30, 1325607178); TOFIX("0.999999999999999999", 30, 1 << 30);
    TOFIX("123456789.123456789", 3, 987654313); TOFIX("123456789.123456789", 30, INT32_MAX);
    TOFIX("1757548800123", 3, INT32_MAX);
    CHECK(!json_number_to_fixed("1", 1, 31, &v));
    CHECK(!json_number_to_fixed("1", 1, -1, &v));
    const char *bad[] = { ".5", "1.", "+1", "", "-", "01", "1e", "abc", "1 ", " 1", "0x10", "1..2" };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        if (json_number_to_int(bad[i], strlen(bad[i]), &v)) { failures++; printf("FAIL to_int accepted '%s'\n", bad[i]); }
        if (json_number_to_float(bad[i], strlen(bad[i]), &f)) { failures++; printf("FAIL to_float accepted '%s'\n", bad[i]); }
    }
    CHECK(json_number_to_float("-0.5", 4, &f) && f == -0.5f);
    CHECK(json_number_to_float("2.5e-1", 6, &f) && f == 0.25f);
    CHECK(json_number_to_float("1e3", 3, &f) && f == 1000.0f);
    CHECK(json_number_to_float("1757548800123", 13, &f) && f > 1.7575e12f && f < 1.7576e12f);
    CHECK(json_number_to_float("640.25", 6, &f) && f == 640.25f);
    CHECK(json_number_to_float("1e50", 4, &f) && f > 3.0e38f);   // saturated, finite
    CHECK(json_number_to_float("1e-50", 5, &f) && f >= 0.0f && f < 1e-40f);
}

int main(void) {
    test_fixture_chunking("test/fixtures/generic-game.json");
    test_fixture_chunking("test/fixtures/gc-us.json");
    test_fixture_chunking("test/fixtures/gc-europe.json");
    test_escapes();
    test_navigation();
    test_key_truncation();
    test_depth();
    test_malformed();
    test_callback_abort();
    test_numbers();
    if (failures) { printf("test_json: %d failure(s)\n", failures); return 1; }
    printf("test_json: OK\n");
    return 0;
}
