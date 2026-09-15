// imgdiff a.png b.png [diff.png]: mean absolute error per channel and the share of pixels whose
// largest channel difference exceeds 32, raw and after a 3x3 box blur (to discount anti-aliasing).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

static void blur(const unsigned char *in, unsigned char *out, int w, int h) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            for (int c = 0; c < 3; c++) {
                int sum = 0, n = 0;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        int xx = x + dx, yy = y + dy;
                        if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
                        sum += in[(yy * w + xx) * 3 + c]; n++;
                    }
                out[(y * w + x) * 3 + c] = (unsigned char)(sum / n);
            }
}

static void compare(const unsigned char *a, const unsigned char *b, int w, int h, const char *label, unsigned char *diff) {
    double mae[3] = {0, 0, 0}; long bad = 0;
    for (long i = 0; i < (long)w * h; i++) {
        int m = 0;
        for (int c = 0; c < 3; c++) { int d = abs(a[i * 3 + c] - b[i * 3 + c]); mae[c] += d; if (d > m) m = d; }
        if (m > 32) bad++;
        if (diff) { unsigned char v = (unsigned char)(m > 255 ? 255 : m); diff[i * 3] = diff[i * 3 + 1] = diff[i * 3 + 2] = v; }
    }
    printf("%s: mae r=%.2f g=%.2f b=%.2f  bad(>32)=%.3f%%\n", label, mae[0] / (w * h), mae[1] / (w * h), mae[2] / (w * h), 100.0 * bad / ((double)w * h));
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: imgdiff a.png b.png [diff.png]\n"); return 2; }
    int wa, ha, wb, hb, n;
    unsigned char *a = stbi_load(argv[1], &wa, &ha, &n, 3);
    unsigned char *b = stbi_load(argv[2], &wb, &hb, &n, 3);
    if (!a || !b) { fprintf(stderr, "cannot load images\n"); return 1; }
    if (wa != wb || ha != hb) { fprintf(stderr, "size mismatch %dx%d vs %dx%d\n", wa, ha, wb, hb); return 1; }
    unsigned char *diff = argc > 3 ? malloc((size_t)wa * ha * 3) : NULL;
    compare(a, b, wa, ha, "raw", diff);
    unsigned char *ba = malloc((size_t)wa * ha * 3), *bb = malloc((size_t)wa * ha * 3);
    blur(a, ba, wa, ha); blur(b, bb, wa, ha);
    compare(ba, bb, wa, ha, "blurred", NULL);
    if (diff) stbi_write_png(argv[3], wa, ha, 3, diff, wa * 3);
    return 0;
}
