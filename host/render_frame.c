// Host renderer: decodes a protocol-v3 frame exactly as the device does and writes a PNG plus the
// numbers that size the device's memory (line-pool bytes, spans, edge visits).
//
//   render_frame <frame.json> [--static other.json] [-o out.png] [--width W --height H]
//                [--chunk N] [--stats] [--overlay]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "frame.h"
#include "font.h"
#include "linepool.h"
#include "icons.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

static frame_t g_frame;
static frame_decoder_t g_dec;
static palette_t g_pal;
static uint8_t g_scratch[KIOSK_SCRATCH_BYTES];
static uint8_t g_raster_scratch[RASTER_SCRATCH_BYTES];

typedef struct {
    uint8_t *img; uint16_t w, h;
    uint32_t pool_bytes, max_line, max_line_y, lines;
    uint8_t rle[LINE_MAX_BYTES];
} sink_t;

static void sink(void *ctx, uint16_t y, const uint8_t *px, uint16_t width) {
    sink_t *s = ctx;
    if (y < s->h) memcpy(s->img + (size_t)y * s->w, px, width < s->w ? width : s->w);
    uint16_t n = rle_encode_line(px, width, s->rle, sizeof s->rle);
    s->pool_bytes += n;
    if (n > s->max_line) { s->max_line = n; s->max_line_y = y; }
    s->lines++;
}

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { perror(path); fclose(f); free(buf); return NULL; }
    fclose(f);
    buf[n] = 0;
    *len = (size_t)n;
    return buf;
}

static double now_ms(void) { return (double)clock() * 1000.0 / CLOCKS_PER_SEC; }

static const char *status_name(frame_status_t s) {
    switch (s) {
        case FD_OK: return "ok";
        case FD_ERR_JSON: return "json";
        case FD_ERR_VERSION: return "version";
        case FD_ERR_STATIC_MISSING: return "static-missing";
        case FD_ERR_STATIC_STORE: return "static-store";
        case FD_ERR_TOO_MANY_OPS: return "too-many-ops";
    }
    return "?";
}

static frame_status_t decode(const char *path, geom_store_t *geom, uint16_t w, uint16_t h, size_t chunk, double *ms) {
    size_t len;
    char *text = read_file(path, &len);
    if (!text) return FD_ERR_JSON;
    double t0 = now_ms();
    frame_decoder_init(&g_dec, &g_frame, &g_pal, geom, g_scratch, sizeof g_scratch, w, h);
    for (size_t off = 0; off < len; off += chunk) {
        size_t n = len - off < chunk ? len - off : chunk;
        if (!frame_decoder_feed(&g_dec, text + off, n)) break;
    }
    frame_status_t st = frame_decoder_finish(&g_dec);
    *ms = now_ms() - t0;
    free(text);
    return st;
}

int main(int argc, char **argv) {
    const char *in = NULL, *out = NULL, *stat = NULL;
    uint16_t w = OUT_MAX_W, h = OUT_MAX_H;
    size_t chunk = 1000;
    bool stats = false, overlay = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--static") && i + 1 < argc) stat = argv[++i];
        else if (!strcmp(argv[i], "--width") && i + 1 < argc) w = (uint16_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--height") && i + 1 < argc) h = (uint16_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--chunk") && i + 1 < argc) chunk = (size_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--stats")) stats = true;
        else if (!strcmp(argv[i], "--overlay")) overlay = true;
        else if (argv[i][0] == '-') { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
        else in = argv[i];
    }
    if (!in || w == 0 || h == 0 || w > OUT_MAX_W || h > OUT_MAX_H || chunk == 0) {
        fprintf(stderr, "usage: render_frame <frame.json> [--static other.json] [-o out.png] [--width W --height H] [--chunk N] [--stats] [--overlay]\n");
        return 2;
    }
    if (!font_init(font_blob, font_blob_size)) { fprintf(stderr, "font blob invalid\n"); return 1; }
    geom_store_t *geom = geom_ram_create(2u << 20);
    palette_init(&g_pal);

    double ms;
    if (stat) {
        frame_status_t st = decode(stat, geom, w, h, chunk, &ms);
        if (st != FD_OK) { fprintf(stderr, "static preload %s: %s\n", stat, status_name(st)); return 1; }
    }
    frame_status_t st = decode(in, geom, w, h, chunk, &ms);
    double decode_ms = ms;
    if (st != FD_OK) { fprintf(stderr, "%s: decode failed: %s\n", in, status_name(st)); return 1; }

    sink_t s = { .w = w, .h = h };
    s.img = calloc((size_t)w * h, 1);
    raster_t r;
    raster_init(&r, g_scratch, g_raster_scratch, w, h, &g_pal);

    op_t extra[2]; uint16_t nextra = 0;
    if (overlay) {
        uint8_t dark = palette_add(&g_pal, 0x101820), ink = palette_add(&g_pal, 0xf8f4ec);
        if (frame_make_overlay_rect(&extra[nextra], w, h, 1180, 640, 80, 60, 12, dark, 200)) nextra++;
        if (frame_make_overlay_icon(&extra[nextra], w, h, 1198, 646, 44, icon_lookup("wifi", 4), ink)) nextra++;
    }
    render_stats_t rs = {0};
    double t0 = now_ms();
    frame_render(&g_frame, &g_pal, geom, &r, extra, nextra, sink, &s, &rs);
    double render_ms = now_ms() - t0;

    if (out) {
        uint8_t *rgb = malloc((size_t)w * h * 3);
        for (size_t i = 0; i < (size_t)w * h; i++) {
            uint32_t c = palette_rgb(&g_pal, s.img[i]);
            rgb[i * 3] = (uint8_t)(c >> 16); rgb[i * 3 + 1] = (uint8_t)(c >> 8); rgb[i * 3 + 2] = (uint8_t)c;
        }
        if (!stbi_write_png(out, w, h, 3, rgb, w * 3)) { fprintf(stderr, "cannot write %s\n", out); return 1; }
        free(rgb);
    }
    if (stats) {
        bool used[PALETTE_SIZE] = {0}; unsigned distinct = 0;
        for (size_t i = 0; i < (size_t)w * h; i++) if (!used[s.img[i]]) { used[s.img[i]] = true; distinct++; }
        printf("%s: status=%s ops=%u dropped=%u/%u palette=%u distinct=%u paints=%u static=%s defs=%u "
               "pool=%u max_line=%u@y%u spans=%u edges=%u decode=%.1fms render=%.1fms lines=%u\n",
               in, status_name(st), g_frame.nops, g_frame.stats_dropped_ops, g_frame.stats_dropped_bytes,
               g_pal.count, distinct, g_frame.npaints, g_frame.static_id[0] ? g_frame.static_id : "-",
               geom_store_is_open(geom) ? geom_store_count(geom) : 0,
               s.pool_bytes, s.max_line, s.max_line_y, rs.spans, rs.edge_visits, decode_ms, render_ms, s.lines);
    }
    return 0;
}
