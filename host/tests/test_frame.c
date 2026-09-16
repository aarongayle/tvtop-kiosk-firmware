// Decodes every fixture (real server output) in small chunks, checks the op/def/paint counts
// against the JSON, renders each frame, and spot-checks pixels.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include "frame.h"
#include "font.h"
#include "json.h"
#include "linepool.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_MSG(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static frame_t g_frame;
static frame_decoder_t g_dec;
static palette_t g_pal;
static uint8_t g_scratch[KIOSK_SCRATCH_BYTES];
static uint8_t g_rscratch[RASTER_SCRATCH_BYTES];
static uint8_t g_img[OUT_MAX_W * OUT_MAX_H];
static uint16_t g_seen[OUT_MAX_H];

// The pixel probes below are written for the 1280x720 canvas at scale 1. The host build's OUT_MAX
// is larger (it covers the Pico 2 W's 1080p), so decode at a fixed size rather than the maximum.
#define TEST_W 1280
#define TEST_H 720

typedef struct { uint32_t ops, defs, paints; bool has_static; } counts_t;

static bool count_cb(void *ctx, const json_stream_t *js, json_event_t ev, const char *data, size_t len, bool final) {
    counts_t *c = ctx; (void)data; (void)len; (void)final;
    if (ev == JSON_EV_ARR_START && js->depth == 2 && js->stack[1] == 'a' && !strcmp(js->key[0], "ops")) c->ops++;
    if (ev == JSON_EV_ARR_START && js->depth == 3 && js->stack[2] == 'o' && !strcmp(js->key[0], "static") && !strcmp(js->key[1], "defs")) c->defs++;
    if (ev == JSON_EV_ARR_START && js->depth == 3 && js->stack[2] == 'a' && !strcmp(js->key[0], "static") && !strcmp(js->key[1], "paints")) c->paints++;
    if (ev == JSON_EV_OBJ_START && js->depth == 1 && !strcmp(js->key[0], "static")) c->has_static = true;
    return true;
}

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
    fclose(f); buf[n] = 0; *len = (size_t)n;
    return buf;
}

static void sink(void *ctx, uint16_t y, const uint8_t *px, uint16_t width) {
    (void)ctx;
    if (y < OUT_MAX_H) { memcpy(g_img + (size_t)y * OUT_MAX_W, px, width); g_seen[y]++; }
}

static frame_status_t decode_chunked(const char *text, size_t len, geom_store_t *geom, size_t chunk, uint16_t w, uint16_t h) {
    frame_decoder_init(&g_dec, &g_frame, &g_pal, geom, g_scratch, sizeof g_scratch, w, h);
    for (size_t off = 0; off < len; off += chunk) {
        size_t n = len - off < chunk ? len - off : chunk;
        if (!frame_decoder_feed(&g_dec, text + off, n)) break;
    }
    return frame_decoder_finish(&g_dec);
}

static void render_all(const geom_store_t *geom) {
    raster_t r;
    raster_init(&r, g_scratch, g_rscratch, g_frame.w, g_frame.h, &g_pal);
    memset(g_seen, 0, sizeof g_seen);
    render_stats_t st = {0};
    frame_render(&g_frame, &g_pal, geom, &r, NULL, 0, sink, NULL, &st);
    for (int y = 0; y < g_frame.h; y++) CHECK_MSG(g_seen[y] == 1, "line %d delivered %u times", y, g_seen[y]);
}

static int fixture_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

int main(void) {
    CHECK(font_init(font_blob, font_blob_size));
    geom_store_t *geom = geom_ram_create(2u << 20);
    palette_init(&g_pal);

    // A frame is scaled from the canvas it declares: 1920×1080 drawn at 1920×1080 is 1:1 and at
    // 960×540 exactly half; a frame without w/h keeps the 1280×720 default.
    {
        static const char big[] = "{\"v\":3,\"w\":1920,\"h\":1080,\"bg\":\"#FFFFFF\",\"ops\":[[\"r\",300,150,90,60,\"#FF0000\",0]]}";
        static const char old[] = "{\"v\":3,\"bg\":\"#FFFFFF\",\"ops\":[[\"r\",300,150,90,60,\"#FF0000\",0]]}";
        CHECK(decode_chunked(big, sizeof big - 1, geom, 5, 1920, 1080) == FD_OK && g_frame.nops == 1);
        CHECK(g_frame.ops[0].v[0] == 300 * PX8_ONE && g_frame.ops[0].v[2] == 90 * PX8_ONE);
        CHECK(decode_chunked(big, sizeof big - 1, geom, 5, 960, 540) == FD_OK && g_frame.nops == 1);
        CHECK(g_frame.ops[0].v[0] == 150 * PX8_ONE && g_frame.ops[0].v[3] == 30 * PX8_ONE);
        CHECK(decode_chunked(old, sizeof old - 1, geom, 5, 1920, 1080) == FD_OK && g_frame.nops == 1);
        CHECK(g_frame.ops[0].v[0] == 450 * PX8_ONE && g_frame.ops[0].v[2] == 135 * PX8_ONE);
    }

    DIR *d = opendir("test/fixtures");
    CHECK(d != NULL);
    if (!d) return 1;
    char *names[64]; int nnames = 0;
    struct dirent *e;
    while ((e = readdir(d)) && nnames < 64) if (strstr(e->d_name, ".json")) names[nnames++] = strdup(e->d_name);
    closedir(d);
    qsort(names, (size_t)nnames, sizeof names[0], fixture_cmp);
    CHECK(nnames >= 20);
    for (int i = 0; i < nnames; i++) {
        char path[256];
        snprintf(path, sizeof path, "test/fixtures/%s", names[i]);
        size_t len;
        char *text = read_file(path, &len);
        CHECK(text != NULL);
        if (!text) continue;
        counts_t c = {0};
        json_stream_t js;
        json_stream_init(&js, count_cb, &c);
        CHECK(json_stream_feed(&js, text, len) && json_stream_finish(&js));

        frame_status_t st = decode_chunked(text, len, geom, 7, TEST_W, TEST_H);
        CHECK_MSG(st == FD_OK, "%s: status %d", names[i], (int)st);
        CHECK_MSG(g_frame.stats_dropped_ops == 0, "%s: dropped %u ops", names[i], g_frame.stats_dropped_ops);
        CHECK_MSG(g_frame.nops == c.ops, "%s: nops %u vs json %u", names[i], g_frame.nops, c.ops);
        CHECK_MSG(g_frame.npaints == c.paints, "%s: paints %u vs json %u", names[i], g_frame.npaints, c.paints);
        if (c.has_static) {
            CHECK_MSG(g_frame.static_id[0] != 0 && geom_store_is_open(geom), "%s: static not open", names[i]);
            CHECK_MSG(geom_store_count(geom) == c.defs, "%s: defs %u vs json %u", names[i], geom_store_count(geom), c.defs);
        }
        render_all(geom);

        // Decoding the same frame again in bigger chunks must give the same op table.
        static op_t first[KIOSK_MAX_OPS];
        uint16_t nfirst = g_frame.nops;
        memcpy(first, g_frame.ops, (size_t)nfirst * sizeof(op_t));
        st = decode_chunked(text, len, geom, 1460, TEST_W, TEST_H);
        CHECK(st == FD_OK && g_frame.nops == nfirst && memcmp(first, g_frame.ops, (size_t)nfirst * sizeof(op_t)) == 0);

        if (!strcmp(names[i], "pairing.json")) {
            render_all(geom);
            CHECK(palette_rgb(&g_pal, g_img[0]) == palette_quantize_rgb(0x0d1b2a));
            CHECK(g_img[0] == g_img[(size_t)719 * OUT_MAX_W + 1279]);
        }
        if (!strcmp(names[i], "generic-game.json")) {
            render_all(geom);
            // First player row: ["r",72,136,688,56,fill,10] — a pixel well inside has that fill.
            int found = 0;
            for (uint16_t k = 0; k < g_frame.nops; k++) {
                const op_t *op = &g_frame.ops[k];
                if (op->kind == OP_RECT && op->v[0] == 72 * 8 && op->v[1] == 136 * 8) {
                    found = 1;
                    CHECK(g_img[(size_t)160 * OUT_MAX_W + 400] == op->cidx);
                    break;
                }
            }
            CHECK(found);
            // The QR bitmap draws both black and white cells inside its square (x 910..1130, y 170..390).
            int black = 0, white = 0;
            for (int y = 170; y < 390; y += 3)
                for (int x = 910; x < 1130; x += 3) {
                    uint32_t rgb = palette_rgb(&g_pal, g_img[(size_t)y * OUT_MAX_W + x]);
                    if (rgb == palette_quantize_rgb(0x000000)) black++;
                    if (rgb == palette_quantize_rgb(0xffffff)) white++;
                }
            CHECK_MSG(black > 500 && white > 500, "qr black=%d white=%d", black, white);
        }
        if (!strcmp(names[i], "gc-us.json")) {
            render_all(geom);
            bool used[256] = {0}; int distinct = 0;
            for (int y = 100; y < 700; y += 2)
                for (int x = 300; x < 1260; x += 2) if (!used[g_img[(size_t)y * OUT_MAX_W + x]]) { used[g_img[(size_t)y * OUT_MAX_W + x]] = true; distinct++; }
            CHECK_MSG(distinct > 5, "map area distinct=%d", distinct);
        }
        free(text);
    }
    // Letterboxed 640×480: decodes and renders, offset rows are background.
    {
        size_t len; char *text = read_file("test/fixtures/generic-game.json", &len);
        CHECK(decode_chunked(text, len, geom, 500, 640, 480) == FD_OK);
        raster_t r;
        raster_init(&r, g_scratch, g_rscratch, 640, 480, &g_pal);
        memset(g_seen, 0, sizeof g_seen);
        render_stats_t st = {0};
        frame_render(&g_frame, &g_pal, geom, &r, NULL, 0, sink, NULL, &st);
        for (int y = 0; y < 480; y++) CHECK(g_seen[y] == 1);
        free(text);
    }
    // Garbage and truncation never crash and report an error.
    {
        size_t len; char *text = read_file("test/fixtures/gc-us.json", &len);
        for (size_t cut = 1; cut < len; cut += len / 37) {
            frame_status_t st = decode_chunked(text, cut, geom, 1000, TEST_W, TEST_H);
            CHECK(st != FD_OK);
        }
        char *bad = malloc(len);
        memcpy(bad, text, len);
        for (size_t k = 0; k < len; k += 97) { bad[k] ^= 0x55; decode_chunked(bad, len, geom, 1000, TEST_W, TEST_H); bad[k] ^= 0x55; }
        free(bad); free(text);
        CHECK(decode_chunked("{\"v\":2,\"ops\":[]}", 16, geom, 100, TEST_W, TEST_H) == FD_ERR_VERSION);
        CHECK(decode_chunked("{\"v\":3,\"ops\":[],\"static\":{\"id\":\"s-nope\"}}", 41, geom, 5, TEST_W, TEST_H) == FD_ERR_STATIC_MISSING);
    }
    printf(failures ? "test_frame: %d failure(s)\n" : "test_frame: OK\n", failures);
    return failures ? 1 : 0;
}
