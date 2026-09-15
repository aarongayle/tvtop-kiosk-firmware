// path.c tests: every command incl. relative forms and implicit repetition, number syntax, arcs
// against the analytic circle/ellipse, S/T reflection, chunked feeding, recorder round trip,
// polygon points, degenerate input, flattening tolerance, zero-scale transforms.
#include "path.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_MSG(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef struct { int32_t x, y; bool start; } vtx_t;
typedef struct { vtx_t *v; size_t n, cap; } vlist_t;

static void vl_cb(void *ctx, int32_t x8, int32_t y8, bool start) {
    vlist_t *l = ctx;
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 64; l->v = realloc(l->v, l->cap * sizeof *l->v); }
    l->v[l->n].x = x8; l->v[l->n].y = y8; l->v[l->n].start = start; l->n++;
}
static void vl_free(vlist_t *l) { free(l->v); memset(l, 0, sizeof *l); }

static const affine_t IDENT = { 1, 1, 0, 0 };

// Flattens `d` fed in `chunk`-byte pieces (0 = whole). Returns finish() && all feeds.
static bool flatten(const char *d, const affine_t *t, float tol, size_t chunk, bool points, vlist_t *out) {
    memset(out, 0, sizeof *out);
    path_flattener_t f;
    path_flattener_init(&f, t, tol, vl_cb, out);
    size_t len = strlen(d);
    bool ok = true;
    if (chunk == 0) chunk = len ? len : 1;
    for (size_t i = 0; i < len && ok; i += chunk) {
        size_t n = len - i < chunk ? len - i : chunk;
        ok = points ? path_flattener_feed_points(&f, d + i, n) : path_flattener_feed(&f, d + i, n);
    }
    ok = path_flattener_finish(&f) && ok;
    CHECK(f.nverts == out->n);
    return ok;
}

// Records `d` in `chunk`-byte pieces, then replays through a flattener.
static bool record_replay(const char *d, const affine_t *t, float tol, size_t chunk, bool points, size_t cap, vlist_t *out, bool *overflow) {
    memset(out, 0, sizeof *out);
    uint8_t *buf = malloc(cap ? cap : 1);
    path_recorder_t r;
    path_recorder_init(&r, buf, cap);
    size_t len = strlen(d);
    bool ok = true;
    if (chunk == 0) chunk = len ? len : 1;
    for (size_t i = 0; i < len && ok; i += chunk) {
        size_t n = len - i < chunk ? len - i : chunk;
        ok = points ? path_recorder_feed_points(&r, d + i, n) : path_recorder_feed(&r, d + i, n);
    }
    ok = path_recorder_finish(&r) && ok;
    *overflow = r.overflow;
    if (ok) {
        path_flattener_t f;
        path_flattener_init(&f, t, tol, vl_cb, out);
        ok = path_flattener_replay(&f, r.buf, r.len);
    }
    free(buf);
    return ok;
}

static bool same_list(const vlist_t *a, const vlist_t *b) {
    if (a->n != b->n) return false;
    for (size_t i = 0; i < a->n; i++)
        if (a->v[i].x != b->v[i].x || a->v[i].y != b->v[i].y || a->v[i].start != b->v[i].start) return false;
    return true;
}

static void dump(const char *name, const vlist_t *l) {
    printf("  %s (%zu):", name, l->n);
    for (size_t i = 0; i < l->n && i < 12; i++) printf(" %s(%d,%d)", l->v[i].start ? "*" : "", l->v[i].x, l->v[i].y);
    printf("\n");
}

// Expects exactly the given px8 vertices.
static void expect(const char *d, const affine_t *t, const vtx_t *exp, size_t n) {
    vlist_t l;
    bool ok = flatten(d, t, 0.2f, 0, false, &l);
    CHECK_MSG(ok, "%s", d);
    bool same = l.n == n;
    for (size_t i = 0; same && i < n; i++) same = l.v[i].x == exp[i].x && l.v[i].y == exp[i].y && l.v[i].start == exp[i].start;
    CHECK_MSG(same, "%s", d);
    if (!same) dump("got", &l);
    vl_free(&l);
}

static void expect_equal(const char *a, const char *b) {
    vlist_t la, lb;
    bool oka = flatten(a, &IDENT, 0.2f, 0, false, &la), okb = flatten(b, &IDENT, 0.2f, 0, false, &lb);
    CHECK_MSG(oka && okb, "%s | %s", a, b);
    CHECK_MSG(same_list(&la, &lb), "%s != %s", a, b);
    if (!same_list(&la, &lb)) { dump("a", &la); dump("b", &lb); }
    vl_free(&la); vl_free(&lb);
}

// ---------------------------------------------------------------------------------------------

static void test_basic_commands(void) {
    {   // M L l H h V v Z with a continuation after Z
        const vtx_t e[] = { {80,80,1},{160,80,0},{160,160,0},{80,160,0},{40,160,0},{40,40,0},{40,24,0}, {80,80,1},{120,120,0} };
        expect("M10 10 L20 10 l0 10 H10 h-5 V5 v-2 Z L15 15", &IDENT, e, 9);
    }
    {   // relative m at start is absolute; m after Z is relative to the subpath start
        const vtx_t e[] = { {8,8,1},{24,24,0}, {16,16,1},{32,16,0} };
        expect("m1 1 l2 2 z m1 1 h2", &IDENT, e, 4);
    }
    expect_equal("M1 1 2 2 3 3", "M1 1 L2 2 L3 3");
    expect_equal("m1 1 2 2 3 3", "M1 1 L3 3 L6 6");
    expect_equal("M1 1L2 2 3 3 4 4", "M1 1 L2 2 L3 3 L4 4");
    expect_equal("M 1689.5 177.4 1703.2 188.4 1710 190", "M1689.5,177.4L1703.2,188.4L1710,190");
    expect_equal("M0 0 h5 5 5", "M0 0 L5 0 L10 0 L15 0");
    expect_equal("M0 0 C1 1 2 2 3 3 4 4 5 5 6 6", "M0 0 C1 1 2 2 3 3 C4 4 5 5 6 6");
    {   // empty subpaths emit nothing; only the last M matters
        const vtx_t e[] = { {24,24,1},{32,32,0} };
        expect("M1 1 M2 2 M3 3 L4 4", &IDENT, e, 2);
    }
    {   // two contours
        const vtx_t e[] = { {0,0,1},{8,0,0},{40,40,1},{48,40,0} };
        expect("M0 0 L1 0 M5 5 L6 5", &IDENT, e, 4);
    }
    {   // separators: commas, newlines, tabs, none where unambiguous
        const vtx_t e[] = { {8,16,1},{24,32,0},{-40,-48,0} };
        expect("M1,2\n\tL3\r\n4,-5-6", &IDENT, e, 3);
    }
}

static void test_numbers(void) {
    { const vtx_t e[] = { {12,2,1},{-16,-24,0} }; expect("M1.5.3L-2-3", &IDENT, e, 2); }         // 1.5 .3 ; -2 -3
    { const vtx_t e[] = { {0,2400,1},{24000,-8,0} }; expect("M1e-3 3E2 L3e+3-1", &IDENT, e, 2); }   // 0.001 300 3000 -1
    { const vtx_t e[] = { {4,-4,1},{2,-2,0} }; expect("M.5-.5L.25-.25", &IDENT, e, 2); }
    { const vtx_t e[] = { {1,-1,1},{0,0,0} }; expect("M0.0625-0.0625L0.05 0.04", &IDENT, e, 2); }   // px8 rounding
    { const vtx_t e[] = { {8,8,1},{16,16,0} }; expect("M+1+1L+2,+2", &IDENT, e, 2); }
    {   // long digit strings do not overflow; value is clamped, never NaN
        vlist_t l;
        CHECK(flatten("M123456789012345678901234567890 1e999 L1e-999 0", &IDENT, 0.2f, 0, false, &l));
        CHECK(l.n == 2 && l.v[0].x == 100000000 && l.v[0].y == 100000000 && l.v[1].x == 0);
        vl_free(&l);
    }
}

static void test_transform(void) {
    affine_t t = { 2, -1, 1.5f, 0 };
    const vtx_t e[] = { {28,-8,1},{44,-24,0} };
    expect("M1 1 L2 3", &t, e, 2);
    affine_t z = { 0, 0, 100, 50 };
    vlist_t l;
    CHECK(flatten("M1 1 C 5 5 9 9 20 1 Q 30 30 40 0 A 5 5 0 1 1 10 10 L1e9 1e9 Z", &z, 0.2f, 0, false, &l));
    CHECK(l.n >= 4);
    for (size_t i = 0; i < l.n; i++) CHECK(l.v[i].x == 800 && l.v[i].y == 400);
    vl_free(&l);
    affine_t nan_t = { NAN, 1, INFINITY, 0 };
    CHECK(flatten("M1 1 L2 2", &nan_t, 0.2f, 0, false, &l));
    CHECK(l.n == 2 && l.v[1].y == 16 && l.v[0].x <= 100000000 && l.v[0].x >= -100000000);   // finite, no crash
    vl_free(&l);
}

// Distance from p to the segment ab.
static double seg_dist(double px, double py, double ax, double ay, double bx, double by) {
    double dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
    double t = l2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / l2 : 0;
    if (t < 0) t = 0; if (t > 1) t = 1;
    double qx = ax + t * dx - px, qy = ay + t * dy - py;
    return sqrt(qx * qx + qy * qy);
}

// Max distance of dense samples of the cubic (px) from the emitted polyline (px8 → px).
static double cubic_deviation(const vlist_t *l, const double p[8]) {
    double worst = 0;
    for (int s = 0; s <= 2000; s++) {
        double t = s / 2000.0, u = 1 - t;
        double x = u*u*u*p[0] + 3*u*u*t*p[2] + 3*u*t*t*p[4] + t*t*t*p[6];
        double y = u*u*u*p[1] + 3*u*u*t*p[3] + 3*u*t*t*p[5] + t*t*t*p[7];
        double best = 1e9;
        for (size_t i = 1; i < l->n; i++) {
            double d = seg_dist(x, y, l->v[i-1].x / 8.0, l->v[i-1].y / 8.0, l->v[i].x / 8.0, l->v[i].y / 8.0);
            if (d < best) best = d;
        }
        if (best > worst) worst = best;
    }
    return worst;
}

static void test_curves(void) {
    vlist_t l;
    // Cubic: endpoints exact, intermediate vertices inside the hull, tolerance respected.
    const double p[8] = { 0, 0, 0, 10, 10, 10, 10, 0 };
    float tols[] = { 0.2f, 0.05f, 1.0f };
    for (int k = 0; k < 3; k++) {
        CHECK(flatten("M0 0 C 0 10 10 10 10 0", &IDENT, tols[k], 0, false, &l));
        CHECK(l.n >= 3 && l.v[0].start && l.v[0].x == 0 && l.v[0].y == 0 && l.v[l.n-1].x == 80 && l.v[l.n-1].y == 0);
        for (size_t i = 1; i < l.n; i++) CHECK(!l.v[i].start && l.v[i].y >= 0 && l.v[i].y <= 61 && l.v[i].x >= 0 && l.v[i].x <= 80);
        double dev = cubic_deviation(&l, p);
        CHECK_MSG(dev <= tols[k] + 0.13, "tol %g dev %g n %zu", tols[k], dev, l.n);
        vl_free(&l);
    }
    // A bigger curve at scale, through a transform (the tolerance is in device px).
    affine_t t = { 3, 3, 100, 100 };
    const double pb[8] = { 100, 100, 100 + 3*50, 100 - 3*80, 100 + 3*120, 100 + 3*90, 100 + 3*200, 100 };
    CHECK(flatten("M0 0 C 50 -80 120 90 200 0", &t, 0.2f, 0, false, &l));
    CHECK_MSG(cubic_deviation(&l, pb) <= 0.2 + 0.13, "dev %g n %zu", cubic_deviation(&l, pb), l.n);
    CHECK(l.n < 80);
    vl_free(&l);
    // A straight "curve" is one segment.
    CHECK(flatten("M0 0 C 1 0 2 0 3 0", &IDENT, 0.2f, 0, false, &l));
    CHECK(l.n == 2 && l.v[1].x == 24);
    vl_free(&l);
    // Quadratic == its degree-elevated cubic.
    expect_equal("M0 0 Q 5 10 10 0", "M0 0 C 3.3333333 6.6666667 6.6666667 6.6666667 10 0");
    // Relative curve forms.
    expect_equal("M10 10 c 0 10 10 10 10 0", "M10 10 C 10 20 20 20 20 10");
    expect_equal("M10 10 q 5 10 10 0", "M10 10 Q 15 20 20 10");
    // S/T reflection rules.
    expect_equal("M0 0 C 0 10 10 10 10 0 S 20 -10 20 0", "M0 0 C 0 10 10 10 10 0 C 10 -10 20 -10 20 0");
    expect_equal("M0 0 C 0 10 10 10 10 0 s 10 -10 10 0", "M0 0 C 0 10 10 10 10 0 C 10 -10 20 -10 20 0");
    expect_equal("M0 0 L10 0 S 20 10 20 0", "M0 0 L10 0 C 10 0 20 10 20 0");          // no reflection after L
    expect_equal("M0 0 Q 5 10 10 0 S 20 10 20 0", "M0 0 Q 5 10 10 0 C 10 0 20 10 20 0"); // nor after Q
    expect_equal("M0 0 Q 5 10 10 0 T 20 0", "M0 0 Q 5 10 10 0 Q 15 -10 20 0");
    expect_equal("M0 0 Q 5 10 10 0 t 10 0 10 0", "M0 0 Q 5 10 10 0 Q 15 -10 20 0 Q 25 10 30 0");
    expect_equal("M0 0 L10 0 T 20 0", "M0 0 L10 0 Q 10 0 20 0");                      // no reflection after L
    expect_equal("M0 0 C 0 10 10 10 10 0 T 20 0", "M0 0 C 0 10 10 10 10 0 Q 10 0 20 0"); // nor after C
    expect_equal("M0 0 S 10 10 10 0", "M0 0 C 0 0 10 10 10 0");                        // first command
}

static void check_on_circle(const vlist_t *l, double cx, double cy, double r, float tol) {
    for (size_t i = 0; i < l->n; i++) {
        double dx = l->v[i].x / 8.0 - cx, dy = l->v[i].y / 8.0 - cy, d = sqrt(dx * dx + dy * dy);
        CHECK_MSG(fabs(d - r) <= tol + 0.13, "vertex %zu at distance %g (r %g)", i, d, r);
    }
}

static void test_arcs(void) {
    vlist_t l;
    // Semicircle, sweep 1: passes through +y (SVG positive-angle direction).
    CHECK(flatten("M 10 0 A 10 10 0 0 1 -10 0", &IDENT, 0.2f, 0, false, &l));
    CHECK(l.n >= 6 && l.v[0].start && l.v[l.n-1].x == -80 && l.v[l.n-1].y == 0);
    check_on_circle(&l, 0, 0, 10, 0.2f);
    for (size_t i = 0; i < l.n; i++) CHECK(l.v[i].y >= -1);
    { double mx = 0; for (size_t i = 0; i < l.n; i++) if (l.v[i].y > mx) mx = l.v[i].y; CHECK(mx >= 78); }
    vl_free(&l);
    // sweep 0: through -y.
    CHECK(flatten("M 10 0 A 10 10 0 0 0 -10 0", &IDENT, 0.2f, 0, false, &l));
    check_on_circle(&l, 0, 0, 10, 0.2f);
    for (size_t i = 0; i < l.n; i++) CHECK(l.v[i].y <= 1);
    vl_free(&l);
    // Quarter arc small vs large: the large one goes round the far side.
    CHECK(flatten("M10 0 A10 10 0 0 1 0 10", &IDENT, 0.2f, 0, false, &l));
    check_on_circle(&l, 0, 0, 10, 0.2f);
    for (size_t i = 0; i < l.n; i++) CHECK(l.v[i].x >= -1 && l.v[i].y >= -1);
    size_t nsmall = l.n;
    vl_free(&l);
    // Large + sweep from (10,0) to (0,10) is the 270° arc in the positive direction, which the
    // two-centre construction places on the circle centred at (10,10), through (20,10) and (10,20).
    CHECK(flatten("M10 0 A10 10 0 1 1 0 10", &IDENT, 0.2f, 0, false, &l));
    check_on_circle(&l, 10, 10, 10, 0.2f);
    { bool far = false; for (size_t i = 0; i < l.n; i++) if (l.v[i].x > 150) far = true; CHECK(far); }
    CHECK(l.n > nsmall);
    vl_free(&l);
    // Flags without separators, and relative form.
    expect_equal("M10 0A10 10 0 01-10 0", "M10 0 A 10 10 0 0 1 -10 0");
    expect_equal("M10 0A10,10,0,1,1,0,10", "M10 0 A 10 10 0 1 1 0 10");
    expect_equal("M10 0 a10 10 0 0 1 -20 0", "M10 0 A 10 10 0 0 1 -10 0");
    expect_equal("M10 0 a10 10 0 0 1 -20 0 a10 10 0 0 1 20 0", "M10 0 A 10 10 0 0 1 -10 0 A 10 10 0 0 1 10 0");
    // Radii too small are scaled up (F.6.6): centre is the chord midpoint, r = 5.
    CHECK(flatten("M0 0 A 1 1 0 0 1 10 0", &IDENT, 0.2f, 0, false, &l));
    check_on_circle(&l, 5, 0, 5, 0.2f);
    CHECK(l.n >= 5);
    vl_free(&l);
    // Zero radius → straight line; zero-length arc omitted.
    { const vtx_t e[] = { {0,0,1},{80,0,0} }; expect("M0 0 A 0 5 0 0 1 10 0", &IDENT, e, 2); }
    { const vtx_t e[] = { {0,0,1},{80,0,0} }; expect("M0 0 L10 0 A 5 5 0 0 1 10 0", &IDENT, e, 2); }
    // Ellipse rx=10, ry=5 and the same ellipse via a 90° rotation of rx=5, ry=10.
    const char *ell[] = { "M 10 0 A 10 5 0 0 1 -10 0", "M 10 0 A 5 10 90 0 1 -10 0", "M 10 0 A 5 10 -90 1 0 -10 0" };
    for (int k = 0; k < 3; k++) {
        CHECK(flatten(ell[k], &IDENT, 0.2f, 0, false, &l));
        CHECK(l.n >= 6);
        for (size_t i = 0; i < l.n; i++) {
            double x = l.v[i].x / 8.0, y = l.v[i].y / 8.0;
            double e = x * x / 100 + y * y / 25;
            CHECK_MSG(fabs(sqrt(e) - 1) < 0.08, "%s vertex %zu (%g,%g) e=%g", ell[k], i, x, y, e);
        }
        vl_free(&l);
    }
    // Near-full circle (two arcs) closes on the analytic circle.
    CHECK(flatten("M 0 -20 A 20 20 0 1 1 0 20 A 20 20 0 1 1 0 -20", &IDENT, 0.2f, 0, false, &l));
    check_on_circle(&l, 0, 0, 20, 0.2f);
    CHECK(l.n >= 20);
    vl_free(&l);
}

static const char *COMPLEX =
    "M12.17,4.71c.85-1.43,2-2.57,3.43-3.43,1.43-.85,3.02-1.28,4.77-1.28h9.22c.71,0,1.32.25,1.83.76.51.51.76,1.12.76,1.83"
    "v3.5l-2.4,1.7a3,3,0,0,1-1.5,2.5s1,2,2,3q1,2,3,3t2,2,3,3S40,10,30,20Q1e1,2E1 15.5.5T1,1V9H2.5z"
    "M 1689.5 177.4 1703.2 188.4 1710 190 A 30 20 45 1 0 1650 150 Z m-5-5 10 0 0 10 z";

static void test_chunking_and_recorder(void) {
    vlist_t whole, chunked, replayed;
    bool overflow;
    CHECK(flatten(COMPLEX, &IDENT, 0.2f, 0, false, &whole));
    CHECK(whole.n > 40);
    size_t chunks[] = { 1, 2, 3, 7, 13 };
    for (int k = 0; k < 5; k++) {
        CHECK(flatten(COMPLEX, &IDENT, 0.2f, chunks[k], false, &chunked));
        CHECK_MSG(same_list(&whole, &chunked), "chunk %zu", chunks[k]);
        vl_free(&chunked);
        CHECK(record_replay(COMPLEX, &IDENT, 0.2f, chunks[k], false, 4096, &replayed, &overflow));
        CHECK(!overflow);
        CHECK_MSG(same_list(&whole, &replayed), "replay chunk %zu", chunks[k]);
        if (!same_list(&whole, &replayed)) { dump("direct", &whole); dump("replay", &replayed); }
        vl_free(&replayed);
    }
    // Through a transform too (the recording is source-space).
    affine_t t = { 0.37f, 0.37f, 411.25f, 12.5f };
    vlist_t a, b;
    CHECK(flatten(COMPLEX, &t, 0.2f, 0, false, &a));
    CHECK(record_replay(COMPLEX, &t, 0.2f, 5, false, 4096, &b, &overflow));
    CHECK(same_list(&a, &b));
    vl_free(&a); vl_free(&b);
    // Numbers that must survive the compact encoding exactly: big, fractional, exponent, negative zero.
    const char *nums = "M 32767.9 -32768 L 1234567.89 0.01 L 99999999 1e5 L -0 12.345678 L 2147483647 -2147483648 L 65535.5 -65535.5";
    CHECK(flatten(nums, &IDENT, 0.2f, 0, false, &a));
    CHECK(record_replay(nums, &IDENT, 0.2f, 0, false, 4096, &b, &overflow));
    CHECK(same_list(&a, &b));
    vl_free(&a); vl_free(&b);
    // Overflow: a too-small buffer sets overflow and finish() fails.
    CHECK(!record_replay(COMPLEX, &IDENT, 0.2f, 0, false, 32, &b, &overflow));
    CHECK(overflow);
    vl_free(&b);
    CHECK(!record_replay("M0 0", &IDENT, 0.2f, 0, false, 0, &b, &overflow));
    CHECK(overflow);
    vl_free(&b);
    // Trailing bare letter: recorded then backed out; replay matches the direct feed.
    CHECK(flatten("M0 0 L5 5 L", &IDENT, 0.2f, 0, false, &a));
    CHECK(a.n == 2);
    CHECK(record_replay("M0 0 L5 5 L", &IDENT, 0.2f, 0, false, 64, &b, &overflow));
    CHECK(same_list(&a, &b));
    vl_free(&a); vl_free(&b);
    // Recorder rejects what the flattener rejects.
    CHECK(!record_replay("M0 0 L5", &IDENT, 0.2f, 0, false, 64, &b, &overflow)); vl_free(&b);
    CHECK(!record_replay("M0 0 X5 5", &IDENT, 0.2f, 0, false, 64, &b, &overflow)); vl_free(&b);
    // Replay rejects truncated/garbage recordings without crashing.
    {
        path_flattener_t f;
        path_flattener_init(&f, &IDENT, 0.2f, vl_cb, &b);
        memset(&b, 0, sizeof b);
        const uint8_t bad1[] = { 'L', 0xF0, 1 };             // int16 cut short
        const uint8_t bad2[] = { 'C', 5, 5, 5 };              // too few args
        const uint8_t bad3[] = { 'X', 5, 5 };                 // unknown command
        const uint8_t bad4[] = { 'M', 0xF9, 0, 0, 0, 0 };     // unknown encoding
        CHECK(!path_flattener_replay(&f, bad1, sizeof bad1));
        CHECK(!path_flattener_replay(&f, bad2, sizeof bad2));
        CHECK(!path_flattener_replay(&f, bad3, sizeof bad3));
        CHECK(!path_flattener_replay(&f, bad4, sizeof bad4));
        CHECK(path_flattener_replay(&f, NULL, 0));
        vl_free(&b);
    }
}

static void test_points(void) {
    vlist_t a, b;
    bool overflow;
    const vtx_t e[] = { {80,80,1},{160,80,0},{160,160,0} };
    const char *forms[] = { "10,10 20,10 20,20", "10 10 20 10 20 20", "10,10,20,10,20,20", "\n10 10\n20 10\n20 20\n", "10,10 20,10 20,20 20" };
    for (int k = 0; k < 5; k++) {
        CHECK(flatten(forms[k], &IDENT, 0.2f, 0, true, &a));
        bool same = a.n == 3;
        for (size_t i = 0; same && i < 3; i++) same = a.v[i].x == e[i].x && a.v[i].y == e[i].y && a.v[i].start == e[i].start;
        CHECK_MSG(same, "points form %d", k);
        if (!same) dump("got", &a);
        for (size_t chunk = 1; chunk <= 4; chunk++) {
            CHECK(flatten(forms[k], &IDENT, 0.2f, chunk, true, &b));
            CHECK(same_list(&a, &b));
            vl_free(&b);
            CHECK(record_replay(forms[k], &IDENT, 0.2f, chunk, true, 256, &b, &overflow));
            CHECK_MSG(same_list(&a, &b), "points form %d chunk %zu", k, chunk);
            if (!same_list(&a, &b)) { dump("direct", &a); dump("replay", &b); }
            vl_free(&b);
        }
        vl_free(&a);
    }
    // Odd coordinate count with a long dangling number (5-byte encoding) is backed out too.
    CHECK(flatten("1 2 3 4 123456.78", &IDENT, 0.2f, 0, true, &a));
    CHECK(a.n == 2);
    CHECK(record_replay("1 2 3 4 123456.78", &IDENT, 0.2f, 0, true, 256, &b, &overflow));
    CHECK(same_list(&a, &b));
    vl_free(&a); vl_free(&b);
    // A single coordinate → nothing; empty → nothing; letters are errors.
    CHECK(flatten("5", &IDENT, 0.2f, 0, true, &a)); CHECK(a.n == 0); vl_free(&a);
    CHECK(flatten("", &IDENT, 0.2f, 0, true, &a)); CHECK(a.n == 0); vl_free(&a);
    CHECK(record_replay("", &IDENT, 0.2f, 0, true, 256, &b, &overflow)); CHECK(b.n == 0); vl_free(&b);
    CHECK(record_replay("5", &IDENT, 0.2f, 0, true, 256, &b, &overflow)); CHECK(b.n == 0); vl_free(&b);
    CHECK(!flatten("10,10 M20,10", &IDENT, 0.2f, 0, true, &a)); vl_free(&a);
    CHECK(!record_replay("10,10 L20,10", &IDENT, 0.2f, 0, true, 256, &b, &overflow)); vl_free(&b);
    // Exponents in points (the fixtures do not use them, but the grammar allows them).
    { CHECK(flatten("1e1,2e0 3E0 4", &IDENT, 0.2f, 0, true, &a)); CHECK(a.n == 2 && a.v[0].x == 80 && a.v[0].y == 16); vl_free(&a); }
}

static void test_degenerate(void) {
    vlist_t l;
    CHECK(flatten("", &IDENT, 0.2f, 0, false, &l)); CHECK(l.n == 0); vl_free(&l);
    CHECK(flatten("   ", &IDENT, 0.2f, 0, false, &l)); CHECK(l.n == 0); vl_free(&l);
    CHECK(flatten("Z", &IDENT, 0.2f, 0, false, &l)); CHECK(l.n == 0); vl_free(&l);
    CHECK(flatten("z z Z", &IDENT, 0.2f, 0, false, &l)); CHECK(l.n == 0); vl_free(&l);
    CHECK(flatten("M", &IDENT, 0.2f, 0, false, &l)); CHECK(l.n == 0); vl_free(&l);             // bare letter tolerated
    CHECK(flatten("M0 0 L5 5 M", &IDENT, 0.2f, 0, false, &l)); CHECK(l.n == 2); vl_free(&l);
    CHECK(!flatten("M 1", &IDENT, 0.2f, 0, false, &l)); CHECK(l.n == 0); vl_free(&l);          // partial group
    CHECK(!flatten("M0 0 L 1 2 3", &IDENT, 0.2f, 0, false, &l)); CHECK(l.n == 2); vl_free(&l); // drawn up to the error
    CHECK(!flatten("1 2 3", &IDENT, 0.2f, 0, false, &l)); CHECK(l.n == 0); vl_free(&l);        // number without command
    CHECK(!flatten("garbage text", &IDENT, 0.2f, 0, false, &l)); vl_free(&l);
    CHECK(!flatten("M0 0 L5 5 X 1 1", &IDENT, 0.2f, 0, false, &l)); CHECK(l.n == 2); vl_free(&l);
    CHECK(!flatten("M0 0 Z 5 5", &IDENT, 0.2f, 0, false, &l)); vl_free(&l);
    CHECK(!flatten("M0 0 L M 1 1", &IDENT, 0.2f, 0, false, &l)); vl_free(&l);
    CHECK(!flatten("M1e 2 L3 4", &IDENT, 0.2f, 0, false, &l)); vl_free(&l);
    CHECK(flatten("L1 1", &IDENT, 0.2f, 0, false, &l)); CHECK(l.n == 2 && l.v[0].x == 0 && l.v[0].start); vl_free(&l);   // no M: starts at origin
    CHECK(flatten("M0 0 L0 0 L0 0 Z", &IDENT, 0.2f, 0, false, &l)); CHECK(l.n == 3); vl_free(&l);
    CHECK(flatten("M5 5 A 0 0 0 0 0 5 5", &IDENT, 0.2f, 0, false, &l)); CHECK(l.n == 0); vl_free(&l);
    CHECK(flatten("M0 0 C 0 0 0 0 0 0", &IDENT, 0.2f, 0, false, &l)); CHECK(l.n == 2); vl_free(&l);
    CHECK(flatten("M0 0 A 1e9 1e9 1e9 1 1 1e9 -1e9", &IDENT, 0.2f, 0, false, &l)); vl_free(&l);   // huge: finite output
    CHECK(flatten("M0 0 A 5 5 720 1 0 3 3 a 5 5 -450 0 1 -3 -3", &IDENT, 0.2f, 0, false, &l)); check_on_circle(&l, 0, 0, 0, 20.0f); vl_free(&l);
    // Feeding after an error keeps failing and stays silent.
    {
        path_flattener_t f;
        memset(&l, 0, sizeof l);
        path_flattener_init(&f, &IDENT, 0.2f, vl_cb, &l);
        CHECK(!path_flattener_feed(&f, "X", 1));   // not a command letter
        CHECK(!path_flattener_feed(&f, "M0 0 L1 1", 9));
        CHECK(!path_flattener_finish(&f));
        CHECK(l.n == 0);
        vl_free(&l);
    }
    // path_flatten_string convenience.
    memset(&l, 0, sizeof l);
    CHECK(path_flatten_string("M0 0 L8 0 L8 8 Z", &IDENT, 0.2f, vl_cb, &l));
    CHECK(l.n == 3 && l.v[2].x == 64 && l.v[2].y == 64);
    vl_free(&l);
    memset(&l, 0, sizeof l);
    CHECK(!path_flatten_string("M0 0 L8", &IDENT, 0.2f, vl_cb, &l));
    vl_free(&l);
}

static void test_fixture_like(void) {
    // Shapes in the style the server emits (relative cubics with no separators), fed byte by byte
    // through the recorder as the decoder does, must match a direct flatten at the game scale.
    const char *d = "M59.2,13.23c1.25,1.52,1.83,3.27,1.76,5.26-.08,1.99-.83,3.7-2.25,5.14-1.02,1-2.19,1.67-3.5,2.02-1.32.35-2.64.35-3.96,0l-2.5-.62v-5.5h-3.14V9.59h9.22c.71,0,1.32.25,1.83.76Z";
    affine_t t = { 4.63f, 4.63f, 33.5f, 278.0f };
    vlist_t a, b;
    bool overflow;
    CHECK(flatten(d, &t, 0.2f, 0, false, &a));
    CHECK(a.n > 10 && a.v[0].start);
    for (size_t i = 1; i < a.n; i++) CHECK(!a.v[i].start);
    CHECK(record_replay(d, &t, 0.2f, 1, false, 1024, &b, &overflow));
    CHECK(same_list(&a, &b));
    vl_free(&a); vl_free(&b);
}

int main(void) {
    test_basic_commands();
    test_numbers();
    test_transform();
    test_curves();
    test_arcs();
    test_chunking_and_recorder();
    test_points();
    test_degenerate();
    test_fixture_like();
    if (failures) { printf("test_path: %d failure(s)\n", failures); return 1; }
    printf("test_path: all passed\n");
    return 0;
}
