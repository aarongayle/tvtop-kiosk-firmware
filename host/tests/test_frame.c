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

// ---- placed 'u' ops and group defs ------------------------------------------------------------

static uint8_t g_img2[OUT_MAX_W * OUT_MAX_H];

// Decodes a frame written with ' for " (readability) at w×h, renders it into g_img, and returns
// the decode status. `st` (optional) gets the render stats.
static frame_status_t run(geom_store_t *geom, const char *text, uint16_t w, uint16_t h, render_stats_t *st) {
    static char buf[8192];
    size_t n = strlen(text);
    if (n >= sizeof buf) return FD_ERR_JSON;
    for (size_t i = 0; i <= n; i++) buf[i] = text[i] == '\'' ? '"' : text[i];
    frame_status_t fs = decode_chunked(buf, n, geom, 3, w, h);
    if (fs != FD_OK) return fs;
    raster_t r;
    raster_init(&r, g_scratch, g_rscratch, w, h, &g_pal);
    memset(g_img, 0, sizeof g_img);
    render_stats_t rs = {0};
    frame_render(&g_frame, &g_pal, geom, &r, NULL, 0, sink, NULL, &rs);
    if (st) *st = rs;
    return fs;
}

static int img_diff(uint16_t w, uint16_t h) {
    int n = 0;
    for (int y = 0; y < h; y++) n += memcmp(g_img + (size_t)y * OUT_MAX_W, g_img2 + (size_t)y * OUT_MAX_W, w) != 0;
    return n;
}

static uint32_t px(int x, int y) { return palette_rgb(&g_pal, g_img[(size_t)y * OUT_MAX_W + x]); }

#define RED palette_quantize_rgb(0xff0000)
#define BLUE palette_quantize_rgb(0x0000ff)
#define WHITE palette_quantize_rgb(0xffffff)

// Every static set below is new (its own id), so each frame carries its defs.
#define DEFS_AT_ORIGIN "'0':['path','M0 0 L30 0 L30 20 Z M4 4 L12 4 L12 10 L4 10 Z',0,0,1,1],'1':['circle',10,10,8]"

static void test_placed(geom_store_t *geom) {
    // Both forms parse; the placed one keeps x, y (device px8) and s.
    CHECK(run(geom, "{'v':3,'bg':'#ffffff','static':{'id':'p1','defs':{" DEFS_AT_ORIGIN "},'paints':[['#ff0000',null,0]]},"
                    "'ops':[['u','0',0],['u','0',0,100,50,1500],['u','0',0,'x',5,6],['u','0',0,100,50],['u','0',0,1,2,true],"
                    "['u','0',0,100,50,0],['u','0',0,100,50,-5],['u','0',0,100,50,32768],['u','0',0,100.5,50.25,32767]]}", 1280, 720, NULL) == FD_OK);
    CHECK(g_frame.nops == 6 && g_frame.stats_dropped_ops == 3);   // s = 0, -5 and 32768 dropped
    CHECK(g_frame.ops[0].aux == 0 && g_frame.ops[0].v[0] == 0 && g_frame.ops[0].v[1] == 0);
    CHECK(g_frame.ops[1].aux == 1 && g_frame.ops[1].v[2] == 800 && g_frame.ops[1].v[3] == 400 && g_frame.ops[1].v[4] == 1500);
    CHECK(g_frame.ops[2].aux == 0 && g_frame.ops[3].aux == 0);   // a non-number, or only five elements: the plain form
    CHECK(g_frame.ops[5].aux == 1 && g_frame.ops[5].v[2] == 804 && g_frame.ops[5].v[3] == 402 && g_frame.ops[5].v[4] == 32767);

    // A placed draw matches the same shape defined at its placed position and size: at 1920×1080
    // (k = 1.5, no offset), at 640×480 (k = 0.5 and a 60 px letterbox, which must not be scaled
    // along with the vertices) and at 1280×720. Strokes scale with s, so the "defined" frame uses a
    // paint twice as wide.
    static const uint16_t modes[3][2] = { { 1920, 1080 }, { 640, 480 }, { 1280, 720 } };
    for (int m = 0; m < 3; m++) {
        uint16_t w = modes[m][0], h = modes[m][1];
        CHECK(run(geom, "{'v':3,'bg':'#ffffff','static':{'id':'p2','defs':{" DEFS_AT_ORIGIN "},"
                        "'paints':[['#ff0000','#0000ff',2],['#0000ff',null,0]]},"
                        "'ops':[['u','0',0,100,50,2000],['u','1',1,400,300,2000],['u','1',0,700,300,1500]]}", w, h, NULL) == FD_OK);
        memcpy(g_img2, g_img, sizeof g_img);
        CHECK(run(geom, "{'v':3,'bg':'#ffffff','static':{'id':'p3','defs':{"
                        "'0':['path','M0 0 L30 0 L30 20 Z M4 4 L12 4 L12 10 L4 10 Z',100,50,2,2],'1':['circle',420,320,16],'2':['circle',715,315,12]},"
                        "'paints':[['#ff0000','#0000ff',4],['#0000ff',null,0],['#ff0000','#0000ff',3]]},"
                        "'ops':[['u','0',0],['u','1',1],['u','2',2]]}", w, h, NULL) == FD_OK);
        CHECK_MSG(img_diff(w, h) == 0, "%ux%u: %d rows differ", w, h, img_diff(w, h));
    }
    // Where it lands, at 640×480: canvas 100..160 × 50..90 is device 50..80 × 60 + (25..45).
    CHECK(run(geom, "{'v':3,'bg':'#ffffff','static':{'id':'p4','defs':{'0':['poly','0,0 30,0 30,20 0,20',0,0,1,1]},'paints':[['#ff0000',null,0]]},"
                    "'ops':[['u','0',0,100,50,2000]]}", 640, 480, NULL) == FD_OK);
    CHECK(g_frame.oy8 == 60 * PX8_ONE && g_frame.ox8 == 0);
    CHECK(px(65, 84) == WHITE && px(65, 85) == RED && px(65, 104) == RED && px(65, 105) == WHITE);
    CHECK(px(49, 90) == WHITE && px(50, 90) == RED && px(79, 90) == RED && px(80, 90) == WHITE);

    // Band rejection uses the placed box: the shape at y 600..640 (1280×720, 16-line bands) is
    // drawn in exactly the three bands it touches, 592..640.
    render_stats_t st;
    CHECK(run(geom, "{'v':3,'bg':'#ffffff','static':{'id':'p5','defs':{" DEFS_AT_ORIGIN "},'paints':[['#ff0000',null,0]]},"
                    "'ops':[['u','0',0,10,600,2000]]}", 1280, 720, &st) == FD_OK);
    CHECK_MSG(st.ops_drawn == 3 && st.ops_skipped == 720 / 16 - 3, "drawn %u skipped %u", st.ops_drawn, st.ops_skipped);
    CHECK(px(20, 601) == RED && px(20, 599) == WHITE);
    // A placement whose box leaves the int16 px8 range is dropped at draw time, not clamped.
    CHECK(run(geom, "{'v':3,'bg':'#ffffff','static':{'id':'p6','defs':{'0':['poly','0,0 200,0 200,100 0,100',0,0,1,1]},'paints':[['#ff0000',null,0]]},"
                    "'ops':[['u','0',0,0,0,32767],['u','0',0,200,0,20000]]}", 1280, 720, &st) == FD_OK);
    CHECK(g_frame.nops == 2 && st.ops_drawn == 0 && px(10, 10) == WHITE);   // 6553 px wide; 4000 px wide from x = 200 ends past 4095
    CHECK(run(geom, "{'v':3,'bg':'#ffffff','static':{'id':'p7','defs':{'0':['poly','0,0 200,0 200,100 0,100',0,0,1,1]},'paints':[['#ff0000',null,0]]},"
                    "'ops':[['u','0',0,0,0,20000]]}", 1280, 720, &st) == FD_OK);
    CHECK(st.ops_drawn == 720 / 16 && px(10, 10) == RED && px(1279, 719) == RED);   // 4000 x 2000 px still fits
}

static void test_groups(geom_store_t *geom) {
    // Group "0" is defined before its members. Members: 1 red rect, 2 blue circle over its right
    // half, then ones that must be skipped — an absent id, a group, itself, a paint the frame does
    // not have — then 2 again. The malformed members after that are dropped while decoding.
    const char *defs = "'0':['group',[['1',0],['2',1],['zz',0],['3',0],['0',0],['1',99],['2',1],['x!',0],[5],'no',[['1'],0],{'a':1}]],"
                       "'1':['poly','0,0 40,0 40,20 0,20',0,0,1,1],'2':['circle',40,10,10],"
                       "'3':['group',[['1',1]]],'4':['group',[['2',1],['1',0]]]";
    char frame[2048];
    snprintf(frame, sizeof frame, "{'v':3,'bg':'#ffffff','static':{'id':'g1','defs':{%s},'paints':[['#ff0000',null,0],['#0000ff',null,0]]},"
                                  "'ops':[['u','0',0],['u','0',5,200,100,1500],['u','4',0,0,200,1000]]}", defs);
    CHECK(run(geom, frame, 1280, 720, NULL) == FD_OK);
    CHECK(g_frame.nops == 3 && g_frame.stats_dropped_defs == 0 && geom_store_count(geom) == 5);
    const geom_rec_t *g0 = geom_store_get(geom, 0);
    CHECK(g0 && g0->kind == GEOM_GROUP && g0->count == 7);
    if (g0) CHECK(geom_rec_members(g0)[0] == 1 && geom_rec_members(g0)[1] == 0 && geom_rec_members(g0)[2] == 2 && geom_rec_members(g0)[3] == 1
                  && geom_rec_members(g0)[4] == 35 * 36 + 35 && geom_rec_members(g0)[11] == 99);
    // Member order: in "0" the circle is drawn over the rect, in "4" under it. Skipped members
    // (the group "3" would paint the rect blue) leave no trace.
    CHECK(px(10, 10) == RED && px(35, 10) == BLUE && px(45, 10) == BLUE);
    CHECK(px(10, 210) == RED && px(35, 210) == RED && px(45, 210) == BLUE);
    memcpy(g_img2, g_img, sizeof g_img);
    // The same picture from one 'u' op per surviving member.
    snprintf(frame, sizeof frame, "{'v':3,'bg':'#ffffff','static':{'id':'g2','defs':{%s},'paints':[['#ff0000',null,0],['#0000ff',null,0]]},"
                                  "'ops':[['u','1',0],['u','2',1],['u','2',1],['u','1',0,200,100,1500],['u','2',1,200,100,1500],['u','2',1,200,100,1500],"
                                  "['u','2',1,0,200,1000],['u','1',0,0,200,1000]]}", defs);
    CHECK(run(geom, frame, 1280, 720, NULL) == FD_OK);
    CHECK_MSG(img_diff(1280, 720) == 0, "group vs members: %d rows differ", img_diff(1280, 720));
    // Drawn directly, "3" is an ordinary group; at 1080p placed and plain groups match their members too.
    snprintf(frame, sizeof frame, "{'v':3,'bg':'#ffffff','static':{'id':'g3','defs':{%s},'paints':[['#ff0000',null,0],['#0000ff',null,0]]},"
                                  "'ops':[['u','3',0],['u','0',0,600,400,3000]]}", defs);
    CHECK(run(geom, frame, 1920, 1080, NULL) == FD_OK);
    CHECK(px(15, 15) == BLUE);
    memcpy(g_img2, g_img, sizeof g_img);
    snprintf(frame, sizeof frame, "{'v':3,'bg':'#ffffff','static':{'id':'g4','defs':{%s},'paints':[['#ff0000',null,0],['#0000ff',null,0]]},"
                                  "'ops':[['u','1',1],['u','1',0,600,400,3000],['u','2',1,600,400,3000],['u','2',1,600,400,3000]]}", defs);
    CHECK(run(geom, frame, 1920, 1080, NULL) == FD_OK);
    CHECK_MSG(img_diff(1920, 1080) == 0, "1080p group vs members: %d rows differ", img_diff(1920, 1080));

    // Members past GEOM_GROUP_MAX are dropped; the group is kept.
    char big[2048];
    int n = snprintf(big, sizeof big, "{'v':3,'bg':'#ffffff','static':{'id':'g5','defs':{'1':['circle',10,10,5],'0':['group',[");
    for (int i = 0; i < GEOM_GROUP_MAX + 8; i++) n += snprintf(big + n, sizeof big - (size_t)n, "%s['1',%d]", i ? "," : "", i % 2);
    snprintf(big + n, sizeof big - (size_t)n, "]]},'paints':[['#ff0000',null,0],['#0000ff',null,0]]},'ops':[['u','0',0]]}");
    CHECK(run(geom, big, 1280, 720, NULL) == FD_OK);
    g0 = geom_store_get(geom, 0);
    CHECK(g0 && g0->count == GEOM_GROUP_MAX && px(10, 10) == BLUE);   // member 31 (paint 1) is the last drawn
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

    test_placed(geom);
    test_groups(geom);

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
