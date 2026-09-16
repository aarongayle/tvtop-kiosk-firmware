// HSTX scanline commands: every pixel accounted for, runs compressed, sync levels right.
#include <stdio.h>
#include <string.h>
#include "hstx_line.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static const hstx_timing_t t720 = { 110, 40, 220, 1280, 5, 5, 20, 720, true, true };
static const hstx_timing_t t480 = { 16, 96, 48, 640, 10, 2, 33, 480, false, false };

// Walks a command stream and returns the number of active pixels it sends; checks raw prefix too.
static uint32_t pixels_in(const uint32_t *w, uint32_t n) {
    uint32_t px = 0, i = 6;
    while (i < n) {
        uint32_t cmd = w[i] & 0xf000u, count = w[i] & 0x0fffu;
        i++;
        if (cmd == HSTX_CMD_TMDS_REPEAT) { px += count; i += 1; }
        else if (cmd == HSTX_CMD_TMDS) { px += count; i += count; }
        else return 0xffffffffu;
    }
    return i == n ? px : 0xfffffffeu;
}

int main(void) {
    uint32_t rgb[256];
    for (int i = 0; i < 256; i++) rgb[i] = 0x010203u * (uint32_t)i;
    uint32_t prefix[6], out[4096];

    // Sync symbols follow the polarity: 720p pulses high, 640x480 pulses low.
    CHECK((hstx_sync_word(&t720, false, false) & 0x3ff) == HSTX_TMDS_CTRL_00);
    CHECK((hstx_sync_word(&t720, false, true) & 0x3ff) == HSTX_TMDS_CTRL_01);
    CHECK((hstx_sync_word(&t720, true, true) & 0x3ff) == HSTX_TMDS_CTRL_11);
    CHECK((hstx_sync_word(&t480, false, false) & 0x3ff) == HSTX_TMDS_CTRL_11);
    CHECK((hstx_sync_word(&t480, true, true) & 0x3ff) == HSTX_TMDS_CTRL_00);
    CHECK((hstx_sync_word(&t720, true, false) >> 10) == (HSTX_TMDS_CTRL_00 | HSTX_TMDS_CTRL_00 << 10));

    hstx_line_prefix(&t720, false, prefix);
    CHECK(prefix[0] == (HSTX_CMD_RAW_REPEAT | 110) && prefix[2] == (HSTX_CMD_RAW_REPEAT | 40) && prefix[4] == (HSTX_CMD_RAW_REPEAT | 220));

    // A blanking line spans the whole line: porches and sync plus the active width.
    uint32_t blank[6];
    CHECK(hstx_blank_line(&t720, true, blank) == 6);
    CHECK((blank[0] & 0xfff) + (blank[2] & 0xfff) + (blank[4] & 0xfff) == 110 + 40 + 220 + 1280);

    // 10 px of colour 3, three single pixels, 256 px of colour 2, then nothing: padded with black.
    const uint8_t spans[] = { 3, 9, 5, 0, 6, 0, 7, 0, 2, 255 };
    uint32_t n = hstx_active_line(&t720, prefix, spans, sizeof spans, rgb, out, 4096);
    CHECK(n == 6 + 2 + 4 + 2 + 2);
    CHECK(out[6] == (HSTX_CMD_TMDS_REPEAT | 10) && out[7] == rgb[3]);
    CHECK(out[8] == (HSTX_CMD_TMDS | 3) && out[9] == rgb[5] && out[10] == rgb[6] && out[11] == rgb[7]);
    CHECK(out[12] == (HSTX_CMD_TMDS_REPEAT | 256) && out[13] == rgb[2]);
    CHECK(out[14] == (HSTX_CMD_TMDS_REPEAT | (1280 - 269)) && out[15] == 0);
    CHECK(pixels_in(out, n) == 1280);

    // Spans running past the width are clipped to it.
    uint8_t wide[20];
    for (int i = 0; i < 20; i += 2) { wide[i] = (uint8_t)(i + 1); wide[i + 1] = 199; }   // 10 x 200 px
    n = hstx_active_line(&t720, prefix, wide, sizeof wide, rgb, out, 4096);
    CHECK(pixels_in(out, n) == 1280);

    // A full line of alternating single pixels: one TMDS command carrying every colour.
    static uint8_t alt[2560];
    for (int i = 0; i < 1280; i++) { alt[2 * i] = (uint8_t)(i & 1); alt[2 * i + 1] = 0; }
    n = hstx_active_line(&t720, prefix, alt, sizeof alt, rgb, out, 4096);
    CHECK(n == 6 + 1 + 1280 && out[6] == (HSTX_CMD_TMDS | 1280));
    CHECK(pixels_in(out, n) == 1280);

    // Not enough room: nothing is half written.
    CHECK(hstx_active_line(&t720, prefix, alt, sizeof alt, rgb, out, 100) == 0);
    CHECK(hstx_active_line(&t720, prefix, spans, 0, rgb, out, 4096) == 8);   // empty: all black

    // Packed runs: core 0 packs a rendered row, core 1 expands it; every pixel and colour survives,
    // read from the table the line was packed against.
    {
        static uint8_t row[1280], packed[1280 * 2 + 1];
        static uint32_t other[256];
        for (int k = 0; k < 256; k++) other[k] = rgb[k] ^ 0x00ffffffu;
        const uint32_t *const tables[2] = { rgb, other };
        for (int x = 0; x < 1280; x++) row[x] = (uint8_t)(x < 300 ? 7 : x < 301 ? 9 : x < 1000 ? 7 + (x % 2) : 3);
        for (uint8_t table = 0; table < 2; table++) {
            uint16_t bytes = hstx_pack_line(row, 1280, table, packed, sizeof packed);
            CHECK(bytes > 0 && bytes % 2 == 1 && packed[0] == table);
            // 300 px of 7 is two runs (256 + 44); 699 alternating pixels are single runs.
            CHECK(packed[1] == 7 && packed[2] == 255 && packed[3] == 7 && packed[4] == 43);
            n = hstx_active_line_packed(&t720, prefix, packed, bytes, tables, out, 4096);
            CHECK(pixels_in(out, n) == 1280);
            uint32_t x = 0, i = 6;
            int colours_ok = 1;
            while (i < n) {
                uint32_t count = out[i] & 0xfff, c = out[i + 1];
                for (uint32_t k = 0; k < count && x < 1280; k++, x++) if (c != tables[table][row[x]]) colours_ok = 0;
                i += 2;
            }
            CHECK(colours_ok && x == 1280);
            CHECK(hstx_active_line_packed(&t720, prefix, packed, bytes, tables, out, 64) == 0);  // no half line
        }
        CHECK(hstx_pack_line(row, 1280, 0, packed, 100) == 0);                            // too small: nothing
        CHECK(hstx_active_line_packed(&t720, prefix, packed, 0, tables, out, 64) == 8);   // empty: black
    }

    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("test_hstx_line: ok\n");
    return 0;
}
