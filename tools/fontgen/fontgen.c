// fontgen — builds the font blob described in src/common/font.h from Roboto Regular/Bold.
//
//   cc -O2 -Ivendor/stb -Isrc/common tools/fontgen/fontgen.c -o fontgen -lm
//   ./fontgen tools/fonts/Roboto-Regular.ttf tools/fonts/Roboto-Bold.ttf assets/fonts.bin src/common/font_blob.c
//
// Bitmap faces use stbtt_ScaleForMappingEmToPixels so that "size" means CSS font-size (em = size px),
// which is what the reference SVG renderer draws; ScaleForPixelHeight would make ascent+descent =
// size and every glyph ~17% too small. Outlines are stored once per weight in font units so the
// device can rasterise any larger size exactly.
//
// Flash budget: the whole firmware must stay under 1 MB, so the bitmap set is trimmed in two ways
// (see docs/FONTS.md): only the sizes below are rasterised (the renderer snaps to the nearest one),
// and faces of LARGE_MIN px and up carry ASCII + Latin-1 letters + the few large-size punctuation
// marks the server actually emits instead of the full charset. Outlines always hold the full set.
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const int SIZES[] = { 7, 8, 9, 10, 11, 12, 13, 14, 16, 19, 22, 24, 26 };   // 7-11 px: 13-19 px board text scaled to 720x480
#define NSIZES ((int)(sizeof SIZES / sizeof SIZES[0]))
#define NWEIGHTS 2
#define LARGE_MIN 22

// Header/record sizes, byte for byte as font.h describes them.
#define HDR_BYTES 16
#define FACE_BYTES 16
#define GLYPH_BYTES 12
#define OFACE_BYTES 24
#define OGLYPH_BYTES 28

// ---- growable byte buffer with little-endian writers ----
typedef struct { uint8_t *p; size_t len, cap; } buf_t;

static void grow(buf_t *b, size_t need) {
    if (b->len + need <= b->cap) return;
    size_t cap = b->cap ? b->cap : 65536;
    while (cap < b->len + need) cap *= 2;
    b->p = realloc(b->p, cap);
    if (!b->p) { fprintf(stderr, "out of memory\n"); exit(1); }
    b->cap = cap;
}
static void put8(buf_t *b, unsigned v) { grow(b, 1); b->p[b->len++] = (uint8_t)v; }
static void put16(buf_t *b, unsigned v) { put8(b, v & 0xff); put8(b, (v >> 8) & 0xff); }
static void put32(buf_t *b, uint32_t v) { put16(b, v & 0xffff); put16(b, v >> 16); }
static void set16(buf_t *b, size_t at, unsigned v) { b->p[at] = (uint8_t)v; b->p[at + 1] = (uint8_t)(v >> 8); }
static void set32(buf_t *b, size_t at, uint32_t v) { set16(b, at, v & 0xffff); set16(b, at + 2, v >> 16); }
static void align4(buf_t *b) { while (b->len & 3) put8(b, 0); }

// ---- charset ----
// Full charset: U+0020-007E, U+00A0-00FF, U+2010-2027, U+2212. Iterated as a successor function.
static int charset_next(int cp) {
    if (cp < 0x20) return 0x20;
    if (cp < 0x7E) return cp + 1;
    if (cp < 0xA0) return 0xA0;
    if (cp < 0xFF) return cp + 1;
    if (cp < 0x2010) return 0x2010;
    if (cp < 0x2027) return cp + 1;
    if (cp < 0x2212) return 0x2212;
    return -1;
}

// Subset kept in the large bitmap faces: ASCII, Latin-1 letters (U+00C0-00FF minus × ÷) and the
// punctuation the kiosk server puts in headings and scores (middle dot, degree, dashes, bullet,
// ellipsis, minus). Everything else at those sizes measures and draws as a space.
static int in_large_charset(int cp) {
    if (cp <= 0x7E) return 1;
    if (cp >= 0xC0 && cp <= 0xFF && cp != 0xD7 && cp != 0xF7) return 1;
    switch (cp) {
    case 0xB0: case 0xB7: case 0x2013: case 0x2014: case 0x2022: case 0x2026: case 0x2212: return 1;
    default: return 0;
    }
}

// Whether cp belongs in a bitmap face of `size` px (the space is always kept, even if the font
// somehow lacked it, because missing glyphs fall back to its advance).
static int in_bitmap_face(const stbtt_fontinfo *info, int cp, int size) {
    if (cp == ' ') return 1;
    if (!stbtt_FindGlyphIndex(info, cp)) return 0;
    return size < LARGE_MIN || in_large_charset(cp);
}

static int clamp_i8(int v, const char *what, int cp, int size) {
    if (v < -128 || v > 127) { fprintf(stderr, "warning: %s=%d out of int8 range for U+%04X at %d px\n", what, v, cp, size); return v < 0 ? -128 : 127; }
    return v;
}

static unsigned char *load_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *d = malloc((size_t)n);
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "read failed: %s\n", path); exit(1); }
    fclose(f);
    return d;
}

// ---- bitmap faces ----
// Appends one face's glyph table + bitmaps; writes the face record at face_rec. Returns the bytes
// this face added to the blob.
static size_t emit_face(buf_t *b, size_t face_rec, const stbtt_fontinfo *info, int size, int bold, int *nglyphs_out) {
    float scale = stbtt_ScaleForMappingEmToPixels(info, (float)size);
    int asc, desc, gap;
    stbtt_GetFontVMetrics(info, &asc, &desc, &gap);

    // Count glyphs first so the table can precede the bitmaps.
    int n = 0;
    for (int cp = charset_next(0); cp > 0; cp = charset_next(cp))
        if (in_bitmap_face(info, cp, size)) n++;

    align4(b);
    size_t start = b->len;
    size_t glyphs_off = b->len;
    grow(b, (size_t)n * GLYPH_BYTES);
    b->len += (size_t)n * GLYPH_BYTES;
    align4(b);
    size_t bits_off = b->len;

    int i = 0;
    for (int cp = charset_next(0); cp > 0; cp = charset_next(cp)) {
        if (!in_bitmap_face(info, cp, size)) continue;
        int adv, lsb, ix0, iy0, ix1, iy1;
        stbtt_GetCodepointHMetrics(info, cp, &adv, &lsb);
        stbtt_GetCodepointBitmapBox(info, cp, scale, scale, &ix0, &iy0, &ix1, &iy1);
        int w = ix1 - ix0, h = iy1 - iy0;
        if (w < 0 || h < 0 || w > 255 || h > 255) { fprintf(stderr, "glyph box %dx%d for U+%04X at %d px\n", w, h, cp, size); exit(1); }
        if (stbtt_IsGlyphEmpty(info, stbtt_FindGlyphIndex(info, cp))) w = h = 0;
        unsigned char *cov = calloc((size_t)(w ? w : 1) * (size_t)(h ? h : 1), 1);
        if (w && h) stbtt_MakeCodepointBitmap(info, cov, w, h, w, scale, scale, cp);

        int stride = (w * 2 + 7) / 8;
        uint32_t off = (uint32_t)(b->len - bits_off);
        for (int y = 0; y < h; y++) {
            for (int bx = 0; bx < stride; bx++) {
                unsigned byte = 0;
                for (int k = 0; k < 4; k++) {
                    int x = bx * 4 + k;
                    if (x >= w) break;
                    unsigned q = (cov[y * w + x] * 3 + 127) / 255;   // round(cov*3/255)
                    byte |= q << (6 - 2 * k);
                }
                put8(b, byte);
            }
        }
        free(cov);

        size_t rec = glyphs_off + (size_t)i * GLYPH_BYTES;
        set16(b, rec + 0, (unsigned)cp);
        b->p[rec + 2] = (uint8_t)w;
        b->p[rec + 3] = (uint8_t)h;
        b->p[rec + 4] = (uint8_t)(int8_t)clamp_i8(ix0, "bx", cp, size);
        b->p[rec + 5] = (uint8_t)(int8_t)clamp_i8(iy0, "by", cp, size);
        set16(b, rec + 6, (unsigned)(int)lround(adv * scale * 8));
        set32(b, rec + 8, off);
        i++;
    }

    b->p[face_rec + 0] = (uint8_t)size;
    b->p[face_rec + 1] = (uint8_t)bold;
    set16(b, face_rec + 2, (unsigned)n);
    set16(b, face_rec + 4, (unsigned)(uint16_t)(int16_t)lround(asc * scale * 8));
    set16(b, face_rec + 6, (unsigned)(uint16_t)(int16_t)lround(desc * scale * 8));
    set32(b, face_rec + 8, (uint32_t)glyphs_off);
    set32(b, face_rec + 12, (uint32_t)bits_off);
    *nglyphs_out = n;
    return b->len - start;
}

// ---- outlines ----
typedef struct { int16_t x, y; uint8_t on; } opt_t;

// Converts stb's shape to TrueType packed contours. Returns the point count; ends[] gets the last
// index of each contour. A contour's closing vertex that lands on its start is dropped: the
// rasteriser closes every contour itself and a duplicate point would just be a zero-length edge.
static int shape_to_contours(const stbtt_vertex *v, int nv, opt_t *pts, int maxpts, uint16_t *ends, int maxcont, int *ncont_out) {
    int np = 0, nc = 0, cstart = 0;
    for (int i = 0; i < nv; i++) {
        if (v[i].type == STBTT_vmove) {
            if (np > cstart) {   // close previous contour
                if (np - cstart >= 2 && pts[np - 1].on && pts[np - 1].x == pts[cstart].x && pts[np - 1].y == pts[cstart].y) np--;
                if (nc >= maxcont) return -1;
                ends[nc++] = (uint16_t)(np - 1);
            }
            cstart = np;
            if (np >= maxpts) return -1;
            pts[np++] = (opt_t){ v[i].x, v[i].y, 1 };
        } else if (v[i].type == STBTT_vline) {
            if (np >= maxpts) return -1;
            pts[np++] = (opt_t){ v[i].x, v[i].y, 1 };
        } else if (v[i].type == STBTT_vcurve) {
            if (np + 2 > maxpts) return -1;
            pts[np++] = (opt_t){ v[i].cx, v[i].cy, 0 };
            pts[np++] = (opt_t){ v[i].x, v[i].y, 1 };
        } else {
            // vcubic never occurs for TrueType outlines; degrade to the end point rather than abort.
            if (np >= maxpts) return -1;
            pts[np++] = (opt_t){ v[i].x, v[i].y, 1 };
        }
    }
    if (np > cstart) {
        if (np - cstart >= 2 && pts[np - 1].on && pts[np - 1].x == pts[cstart].x && pts[np - 1].y == pts[cstart].y) np--;
        if (nc >= maxcont) return -1;
        ends[nc++] = (uint16_t)(np - 1);
    }
    *ncont_out = nc;
    return np;
}

static size_t emit_outlines(buf_t *b, size_t oface_rec, const stbtt_fontinfo *info) {
    size_t start = b->len;
    int asc, desc, gap;
    stbtt_GetFontVMetrics(info, &asc, &desc, &gap);
    int upem = (int)lround(1.0 / stbtt_ScaleForMappingEmToPixels(info, 1.0f));

    int n = 0;
    for (int cp = charset_next(0); cp > 0; cp = charset_next(cp))
        if (cp == ' ' || stbtt_FindGlyphIndex(info, cp)) n++;

    align4(b);
    size_t glyphs_off = b->len;
    grow(b, (size_t)n * OGLYPH_BYTES);
    b->len += (size_t)n * OGLYPH_BYTES;

    // Three parallel streams; each glyph's offsets are relative to its stream's start.
    buf_t pts = { 0 }, flags = { 0 }, ends = { 0 };
    static opt_t P[4096];
    static uint16_t E[256];

    int i = 0;
    for (int cp = charset_next(0); cp > 0; cp = charset_next(cp)) {
        if (cp != ' ' && !stbtt_FindGlyphIndex(info, cp)) continue;
        int adv, lsb, x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetCodepointHMetrics(info, cp, &adv, &lsb);
        stbtt_GetCodepointBox(info, cp, &x0, &y0, &x1, &y1);
        stbtt_vertex *v = NULL;
        int nv = stbtt_GetCodepointShape(info, cp, &v);
        int nc = 0;
        int np = shape_to_contours(v, nv, P, 4096, E, 256, &nc);
        if (np < 0) { fprintf(stderr, "outline too complex for U+%04X\n", cp); exit(1); }
        stbtt_FreeShape(info, v);

        size_t rec = glyphs_off + (size_t)i * OGLYPH_BYTES;
        set16(b, rec + 0, (unsigned)cp);
        set16(b, rec + 2, (unsigned)adv);
        set16(b, rec + 4, (unsigned)(uint16_t)(int16_t)x0);
        set16(b, rec + 6, (unsigned)(uint16_t)(int16_t)y0);
        set16(b, rec + 8, (unsigned)(uint16_t)(int16_t)x1);
        set16(b, rec + 10, (unsigned)(uint16_t)(int16_t)y1);
        set16(b, rec + 12, (unsigned)np);
        set16(b, rec + 14, (unsigned)nc);
        set32(b, rec + 16, (uint32_t)pts.len);
        set32(b, rec + 20, (uint32_t)flags.len);
        set32(b, rec + 24, (uint32_t)ends.len);
        for (int k = 0; k < np; k++) {
            put16(&pts, (unsigned)(uint16_t)P[k].x);
            put16(&pts, (unsigned)(uint16_t)P[k].y);
            put8(&flags, P[k].on);
        }
        for (int k = 0; k < nc; k++) put16(&ends, E[k]);
        i++;
    }

    align4(b); size_t points_off = b->len; grow(b, pts.len); memcpy(b->p + b->len, pts.p, pts.len); b->len += pts.len;
    align4(b); size_t flags_off = b->len; grow(b, flags.len); memcpy(b->p + b->len, flags.p, flags.len); b->len += flags.len;
    align4(b); size_t ends_off = b->len; grow(b, ends.len); memcpy(b->p + b->len, ends.p, ends.len); b->len += ends.len;
    free(pts.p); free(flags.p); free(ends.p);

    set16(b, oface_rec + 0, (unsigned)upem);
    set16(b, oface_rec + 2, (unsigned)(uint16_t)(int16_t)asc);
    set16(b, oface_rec + 4, (unsigned)(uint16_t)(int16_t)desc);
    set16(b, oface_rec + 6, (unsigned)n);
    set32(b, oface_rec + 8, (uint32_t)glyphs_off);
    set32(b, oface_rec + 12, (uint32_t)points_off);
    set32(b, oface_rec + 16, (uint32_t)flags_off);
    set32(b, oface_rec + 20, (uint32_t)ends_off);
    return b->len - start;
}

int main(int argc, char **argv) {
    const char *reg_path = argc > 1 ? argv[1] : "tools/fonts/Roboto-Regular.ttf";
    const char *bold_path = argc > 2 ? argv[2] : "tools/fonts/Roboto-Bold.ttf";
    const char *bin_path = argc > 3 ? argv[3] : "assets/fonts.bin";
    const char *c_path = argc > 4 ? argv[4] : "src/common/font_blob.c";

    stbtt_fontinfo fonts[NWEIGHTS];
    const char *paths[NWEIGHTS] = { reg_path, bold_path };
    for (int w = 0; w < NWEIGHTS; w++) {
        unsigned char *ttf = load_file(paths[w]);
        if (!stbtt_InitFont(&fonts[w], ttf, stbtt_GetFontOffsetForIndex(ttf, 0))) { fprintf(stderr, "bad font: %s\n", paths[w]); return 1; }
    }

    buf_t b = { 0 };
    int nfaces = NSIZES * NWEIGHTS;
    // header
    put8(&b, 'T'); put8(&b, 'V'); put8(&b, 'F'); put8(&b, 'N');
    put16(&b, 1);
    put16(&b, (unsigned)nfaces);
    put32(&b, 0);   // outlines_off, patched below
    put32(&b, HDR_BYTES);
    size_t faces_off = b.len;
    grow(&b, (size_t)nfaces * FACE_BYTES);
    b.len += (size_t)nfaces * FACE_BYTES;

    size_t bitmap_total = 0;
    for (int w = 0; w < NWEIGHTS; w++) {
        for (int s = 0; s < NSIZES; s++) {
            int ng = 0;
            size_t bytes = emit_face(&b, faces_off + (size_t)(w * NSIZES + s) * FACE_BYTES, &fonts[w], SIZES[s], w, &ng);
            printf("face %2d px %-7s %3d glyphs %6zu bytes\n", SIZES[s], w ? "bold" : "regular", ng, bytes);
            bitmap_total += bytes;
        }
    }

    align4(&b);
    size_t outlines_off = b.len;
    set32(&b, 8, (uint32_t)outlines_off);
    grow(&b, NWEIGHTS * OFACE_BYTES);
    b.len += NWEIGHTS * OFACE_BYTES;
    size_t outline_total = 0;
    for (int w = 0; w < NWEIGHTS; w++) {
        size_t bytes = emit_outlines(&b, outlines_off + (size_t)w * OFACE_BYTES, &fonts[w]);
        printf("outlines %-7s %6zu bytes\n", w ? "bold" : "regular", bytes);
        outline_total += bytes;
    }
    printf("bitmap faces %zu bytes, outlines %zu bytes, headers %zu bytes, total %zu bytes (%.1f KB)\n",
           bitmap_total, outline_total, b.len - bitmap_total - outline_total, b.len, b.len / 1024.0);

    FILE *f = fopen(bin_path, "wb");
    if (!f) { perror(bin_path); return 1; }
    fwrite(b.p, 1, b.len, f);
    fclose(f);

    f = fopen(c_path, "w");
    if (!f) { perror(c_path); return 1; }
    fprintf(f, "// Generated by tools/fontgen from Roboto Regular/Bold. Do not edit; see docs/FONTS.md.\n#include <stdint.h>\n");
    fprintf(f, "const uint8_t font_blob[] = {");
    for (size_t i = 0; i < b.len; i++) {
        if (i % 16 == 0) fprintf(f, "\n ");
        fprintf(f, " 0x%02x,", b.p[i]);
    }
    fprintf(f, "\n};\nconst uint32_t font_blob_size = %zu;\n", b.len);
    fclose(f);
    return 0;
}
