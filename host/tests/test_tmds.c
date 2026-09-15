// tmds.c tests: the balanced set is re-derived from the encoder, balanced symbols are
// disparity-independent, decode inverts encode for every byte at several running disparities,
// the nearest-level table is checked against a brute-force search, and the control symbols
// match the DVI 1.0 constants.
#include "tmds.h"
#include <stdio.h>
#include <stdlib.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static int popcount(unsigned x) { int n = 0; for (; x; x &= x - 1) n++; return n; }

static const uint8_t expected_levels[TMDS_BALANCED_COUNT] = {
    16, 17, 34, 35, 38, 39, 44, 45, 54, 55, 56, 57, 68, 69, 74, 75, 84, 85, 90, 91, 104, 105, 108,
    109, 118, 119, 136, 137, 146, 147, 150, 151, 164, 165, 170, 171, 180, 181, 186, 187, 198, 199,
    200, 201, 210, 211, 216, 217, 220, 221, 238, 239,
};

// A byte is balanced iff encoding it from zero disparity leaves the disparity at zero: the
// encoder adds N1-N0 of q_m's low byte, which is zero exactly when it has four ones.
static void test_balanced_set(void) {
    uint8_t derived[256];
    int n = 0;
    for (int v = 0; v < 256; v++) {
        int d = 0;
        uint16_t sym = tmds_encode_byte((uint8_t)v, &d);
        if (d == 0) {
            derived[n++] = (uint8_t)v;
            CHECK(popcount(sym) == 5);   // zero-disparity symbol: five ones in ten bits
        }
    }
    CHECK(n == TMDS_BALANCED_COUNT);
    for (int i = 0; i < TMDS_BALANCED_COUNT && i < n; i++) {
        if (derived[i] != tmds_balanced_levels[i] || tmds_balanced_levels[i] != expected_levels[i]) {
            failures++;
            printf("FAIL level %d: derived %u, table %u, expected %u\n", i, derived[i], tmds_balanced_levels[i], expected_levels[i]);
        }
    }
    for (int i = 1; i < TMDS_BALANCED_COUNT; i++) CHECK(tmds_balanced_levels[i - 1] < tmds_balanced_levels[i]);
    // Every balanced symbol has exactly four ones in its low 8 bits after undoing the inversion,
    // i.e. balanced values are all even/odd pairs (v, v^1) -- the property PicoDVI exploits.
    for (int i = 0; i < TMDS_BALANCED_COUNT; i += 2) CHECK((tmds_balanced_levels[i] ^ 1) == tmds_balanced_levels[i + 1]);
}

static void test_is_and_nearest(void) {
    for (int v = 0; v < 256; v++) {
        bool inset = false;
        for (int i = 0; i < TMDS_BALANCED_COUNT; i++) if (expected_levels[i] == v) inset = true;
        CHECK(tmds_is_balanced((uint8_t)v) == inset);

        uint8_t near = tmds_nearest_balanced((uint8_t)v);
        CHECK(tmds_is_balanced(near));
        // Brute force: minimal distance, ties to the larger level.
        int best = -1, best_d = 1000;
        for (int i = 0; i < TMDS_BALANCED_COUNT; i++) {
            int d = abs((int)expected_levels[i] - v);
            if (d < best_d || (d == best_d && expected_levels[i] > best)) { best_d = d; best = expected_levels[i]; }
        }
        if (near != best) { failures++; printf("FAIL nearest(%d) = %u, want %d\n", v, near, best); }
        // Interior gaps are at most 17 wide (17..34, 221..238) so the error is at most 8 there;
        // the ends are unreachable below 16 / above 239 and can be off by up to 16.
        CHECK(abs((int)near - v) <= 16);
        if (v >= 8 && v <= 247) CHECK(abs((int)near - v) <= 8);
        if (inset) CHECK(near == v);
    }
    CHECK(tmds_nearest_balanced(0) == 16);
    CHECK(tmds_nearest_balanced(255) == 239);
    CHECK(tmds_nearest_balanced(25) == 17);    // 25-17 = 8 < 34-25 = 9
    CHECK(tmds_nearest_balanced(26) == 34);    // 8 either way: tie rounds up
    CHECK(tmds_nearest_balanced(229) == 221);
    CHECK(tmds_nearest_balanced(230) == 238);  // tie (9 vs 8 -> 238 is nearer anyway)
    CHECK(tmds_nearest_balanced(36) == 35);    // 1 vs 2
    CHECK(tmds_nearest_balanced(37) == 38);    // 2 vs 1
}

static void test_symbol_balanced(void) {
    for (int i = 0; i < TMDS_BALANCED_COUNT; i++) {
        uint8_t v = tmds_balanced_levels[i];
        uint16_t sym = tmds_symbol_balanced(v);
        CHECK(sym < 0x400);
        CHECK(popcount(sym) == 5);
        CHECK(tmds_decode_symbol(sym) == v);
        for (int d0 = -8; d0 <= 8; d0++) {
            int d = d0;
            uint16_t s = tmds_encode_byte(v, &d);
            if (s != sym || d != d0) {
                failures++;
                printf("FAIL value %u at disparity %d: sym %03x (balanced %03x), disparity after %d\n", v, d0, s, sym, d);
            }
        }
    }
    // A run of balanced pixels packs into identical words: two symbols per word, first in the
    // low 10 bits.
    uint16_t a = tmds_symbol_balanced(16), b = tmds_symbol_balanced(239);
    CHECK(tmds_pack2(a, b) == ((uint32_t)a | ((uint32_t)b << 10)));
    CHECK((tmds_pack2(a, b) & 0x3ff) == a);
    CHECK(((tmds_pack2(a, b) >> 10) & 0x3ff) == b);
}

static void test_roundtrip(void) {
    // Every byte, from a range of starting disparities, decodes back; the disparity stays bounded.
    static const int starts[] = { -8, -5, -3, -1, 0, 1, 3, 5, 8 };
    for (size_t k = 0; k < sizeof starts / sizeof starts[0]; k++) {
        for (int v = 0; v < 256; v++) {
            int d = starts[k];
            uint16_t sym = tmds_encode_byte((uint8_t)v, &d);
            CHECK(sym < 0x400);
            uint8_t back = tmds_decode_symbol(sym);
            if (back != v) { failures++; printf("FAIL roundtrip %d at disparity %d: sym %03x -> %u\n", v, starts[k], sym, back); }
            // The DC-balance stage keeps |disparity| within the 10-bit symbol's reach.
            CHECK(d >= -10 && d <= 10);
        }
    }
    // Sequential encoding of an arbitrary stream: the running disparity tracks the real ones/zeros
    // balance of what was sent (definition of the encoder's state), so recomputing it from the
    // symbols must agree.
    int d = 0, acc = 0;
    unsigned x = 12345;
    for (int i = 0; i < 4096; i++) {
        x = x * 1103515245u + 12345u;
        uint8_t v = (uint8_t)(x >> 16);
        uint16_t sym = tmds_encode_byte(v, &d);
        acc += 2 * popcount(sym) - 10;
        CHECK(tmds_decode_symbol(sym) == v);
    }
    CHECK(acc == d);
    // Hand-worked vectors from figure 3-5, starting at disparity 0:
    //   0x00: XOR chain gives q_m = 0x100 (bit 8 set, low byte 0), imbalance -8, not inverted
    //         because bit 8 is set -> 0x100, disparity -8.
    //   0xff: XNOR chain gives q_m = 0x0ff (bit 8 clear), imbalance +8, inverted with 0x2ff
    //         -> 0x200, disparity -8.
    d = 0; CHECK(tmds_encode_byte(0x00, &d) == 0x100); CHECK(d == -8);
    d = 0; CHECK(tmds_encode_byte(0xff, &d) == 0x200); CHECK(d == -8);
    // From a negative disparity the same bytes must swing the other way (ones-minus-zeros of
    // the symbol is what the disparity accumulates: 0x3ff is +10, 0x0ff is +6).
    d = -8; CHECK(tmds_encode_byte(0x00, &d) == 0x3ff); CHECK(d == 2);
    d = -8; CHECK(tmds_encode_byte(0xff, &d) == 0x0ff); CHECK(d == -2);
}

static void test_control_symbols(void) {
    // DVI 1.0 table 3-6, transcribed in PicoDVI's tmds_table_gen.py (ctrl_syms).
    CHECK(TMDS_CTRL_00 == 0x354u);   // 0b1101010100
    CHECK(TMDS_CTRL_01 == 0x0abu);   // 0b0010101011
    CHECK(TMDS_CTRL_10 == 0x154u);   // 0b0101010100
    CHECK(TMDS_CTRL_11 == 0x2abu);   // 0b1010101011
    // Control symbols are chosen for many transitions, which no data symbol has: seven or more.
    const uint16_t ctrl[4] = { TMDS_CTRL_00, TMDS_CTRL_01, TMDS_CTRL_10, TMDS_CTRL_11 };
    for (int i = 0; i < 4; i++) {
        int transitions = 0;
        for (int b = 0; b < 9; b++) if (((ctrl[i] >> b) ^ (ctrl[i] >> (b + 1))) & 1) transitions++;
        CHECK(transitions >= 7);
        // And no data byte encodes to a control symbol at any disparity in range.
        for (int d0 = -8; d0 <= 8; d0++) for (int v = 0; v < 256; v++) {
            int d = d0;
            CHECK(tmds_encode_byte((uint8_t)v, &d) != ctrl[i]);
        }
    }
}

int main(void) {
    test_balanced_set();
    test_is_and_nearest();
    test_symbol_balanced();
    test_roundtrip();
    test_control_symbols();
    if (failures) { printf("test_tmds: %d failure(s)\n", failures); return 1; }
    printf("test_tmds: OK\n");
    return 0;
}
