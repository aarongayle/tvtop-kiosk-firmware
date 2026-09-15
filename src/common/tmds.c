// TMDS symbol encoder/decoder (see tmds.h). tmds_encode_byte is a literal transcription of DVI 1.0
// figure 3-5, the same algorithm PicoDVI's tmds_table_gen.py implements, so symbols match libdvi's.
#include "tmds.h"
#include <assert.h>

const uint8_t tmds_balanced_levels[TMDS_BALANCED_COUNT] = {
    16, 17, 34, 35, 38, 39, 44, 45, 54, 55, 56, 57, 68, 69, 74, 75, 84, 85, 90, 91, 104, 105, 108,
    109, 118, 119, 136, 137, 146, 147, 150, 151, 164, 165, 170, 171, 180, 181, 186, 187, 198, 199,
    200, 201, 210, 211, 216, 217, 220, 221, 238, 239,
};

// Nearest balanced level for every byte (ties round up). Generated from tmds_balanced_levels; the
// test re-derives it. A table keeps palette quantisation free of searches and divisions.
static const uint8_t nearest_lut[256] = {
     16,  16,  16,  16,  16,  16,  16,  16,  16,  16,  16,  16,  16,  16,  16,  16,
     16,  17,  17,  17,  17,  17,  17,  17,  17,  17,  34,  34,  34,  34,  34,  34,
     34,  34,  34,  35,  35,  38,  38,  39,  39,  39,  44,  44,  44,  45,  45,  45,
     45,  45,  54,  54,  54,  54,  54,  55,  56,  57,  57,  57,  57,  57,  57,  68,
     68,  68,  68,  68,  68,  69,  69,  69,  74,  74,  74,  75,  75,  75,  75,  75,
     84,  84,  84,  84,  84,  85,  85,  85,  90,  90,  90,  91,  91,  91,  91,  91,
     91,  91, 104, 104, 104, 104, 104, 104, 104, 105, 105, 108, 108, 109, 109, 109,
    109, 109, 118, 118, 118, 118, 118, 119, 119, 119, 119, 119, 119, 119, 119, 119,
    136, 136, 136, 136, 136, 136, 136, 136, 136, 137, 137, 137, 137, 137, 146, 146,
    146, 146, 146, 147, 147, 150, 150, 151, 151, 151, 151, 151, 151, 151, 164, 164,
    164, 164, 164, 164, 164, 165, 165, 165, 170, 170, 170, 171, 171, 171, 171, 171,
    180, 180, 180, 180, 180, 181, 181, 181, 186, 186, 186, 187, 187, 187, 187, 187,
    187, 198, 198, 198, 198, 198, 198, 199, 200, 201, 201, 201, 201, 201, 210, 210,
    210, 210, 210, 211, 211, 211, 216, 216, 216, 217, 217, 220, 220, 221, 221, 221,
    221, 221, 221, 221, 221, 221, 238, 238, 238, 238, 238, 238, 238, 238, 238, 239,
    239, 239, 239, 239, 239, 239, 239, 239, 239, 239, 239, 239, 239, 239, 239, 239,
};

uint8_t tmds_nearest_balanced(uint8_t v) { return nearest_lut[v]; }
bool tmds_is_balanced(uint8_t v) { return nearest_lut[v] == v; }

static int popcount8(unsigned x) {
    int n = 0;
    for (; x; x &= x - 1) n++;
    return n;
}

// N1 - N0 of the low 8 bits.
static int byte_imbalance(unsigned x) { return 2 * popcount8(x & 0xff) - 8; }

uint16_t tmds_encode_byte(uint8_t d, int *disparity) {
    // Stage 1: transition minimisation. XNOR chain when the byte is "ones heavy" (bit 8 = 0),
    // XOR chain otherwise (bit 8 = 1).
    unsigned q_m = d & 1u;
    int ones = popcount8(d);
    if (ones > 4 || (ones == 4 && !(d & 1u))) {
        for (int i = 0; i < 7; i++) q_m |= (~((q_m >> i) ^ (d >> (i + 1))) & 1u) << (i + 1);
    } else {
        for (int i = 0; i < 7; i++) q_m |= (((q_m >> i) ^ (d >> (i + 1))) & 1u) << (i + 1);
        q_m |= 0x100u;
    }
    // Stage 2: DC balance. Bit 9 flags inversion of the low 8 bits.
    const unsigned inversion_mask = 0x2ffu;
    int imb = byte_imbalance(q_m);
    unsigned q_out;
    if (*disparity == 0 || imb == 0) {
        q_out = q_m ^ ((q_m & 0x100u) ? 0u : inversion_mask);
        if (q_m & 0x100u) *disparity += imb;
        else *disparity -= imb;
    } else if ((*disparity > 0) == (imb > 0)) {
        q_out = q_m ^ inversion_mask;
        *disparity += (int)((q_m & 0x100u) >> 7) - imb;
    } else {
        q_out = q_m;
        *disparity += imb - (int)((~q_m & 0x100u) >> 7);
    }
    return (uint16_t)q_out;
}

uint16_t tmds_symbol_balanced(uint8_t v) {
    assert(tmds_is_balanced(v));
    int disparity = 0;
    return tmds_encode_byte(v, &disparity);
}

uint8_t tmds_decode_symbol(uint16_t sym) {
    unsigned d = sym & 0xffu;
    if (sym & 0x200u) d ^= 0xffu;          // undo the DC-balance inversion
    unsigned out = d & 1u;
    if (sym & 0x100u) {                    // XOR chain
        for (int i = 1; i < 8; i++) out |= (((d >> i) ^ (d >> (i - 1))) & 1u) << i;
    } else {                               // XNOR chain
        for (int i = 1; i < 8; i++) out |= (~((d >> i) ^ (d >> (i - 1))) & 1u) << i;
    }
    return (uint8_t)out;
}
