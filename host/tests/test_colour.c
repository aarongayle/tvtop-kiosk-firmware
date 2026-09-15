// colour.c tests: every accepted form, case/space tolerance, and the rejects the server is known
// to leak ("url(#…)", "none") which must map to "draw nothing".
#include "colour.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void expect(const char *s, unsigned r, unsigned g, unsigned b, unsigned a) {
    rgba8_t c = { 1, 2, 3, 4 };
    if (!colour_parse(s, strlen(s), &c)) {
        failures++;
        printf("FAIL rejected '%s'\n", s);
        return;
    }
    if (c.r != r || c.g != g || c.b != b || c.a != a) {
        failures++;
        printf("FAIL '%s' -> %02x%02x%02x%02x, want %02x%02x%02x%02x\n", s, c.r, c.g, c.b, c.a, r, g, b, a);
    }
}

static void reject(const char *s) {
    rgba8_t c = { 9, 9, 9, 9 };
    if (colour_parse(s, strlen(s), &c)) {
        failures++;
        printf("FAIL accepted '%s' -> %02x%02x%02x%02x\n", s, c.r, c.g, c.b, c.a);
    }
}

int main(void) {
    // Hex forms.
    expect("#000000", 0, 0, 0, 255);
    expect("#ffffff", 255, 255, 255, 255);
    expect("#FFFFFF", 255, 255, 255, 255);
    expect("#1a2B3c", 0x1a, 0x2b, 0x3c, 255);
    expect("#12345678", 0x12, 0x34, 0x56, 0x78);
    expect("#00000000", 0, 0, 0, 0);
    // Short forms double each nibble: f -> ff, 4 -> 44.
    expect("#fff", 255, 255, 255, 255);
    expect("#000", 0, 0, 0, 255);
    expect("#abc", 0xaa, 0xbb, 0xcc, 255);
    expect("#ABC", 0xaa, 0xbb, 0xcc, 255);
    expect("#0004", 0, 0, 0, 0x44);
    expect("#f00f", 255, 0, 0, 255);
    expect("#1234", 0x11, 0x22, 0x33, 0x44);
    // Surrounding whitespace is tolerated, including tabs and newlines.
    expect(" #fff", 255, 255, 255, 255);
    expect("#fff ", 255, 255, 255, 255);
    expect("  #123456  ", 0x12, 0x34, 0x56, 255);
    expect("\t#123456\n", 0x12, 0x34, 0x56, 255);

    // Names (the set the task requires, plus case-insensitivity and spaces).
    expect("black", 0, 0, 0, 255);
    expect("white", 255, 255, 255, 255);
    expect("red", 255, 0, 0, 255);
    expect("green", 0, 0x80, 0, 255);
    expect("blue", 0, 0, 255, 255);
    expect("yellow", 255, 255, 0, 255);
    expect("gray", 0x80, 0x80, 0x80, 255);
    expect("grey", 0x80, 0x80, 0x80, 255);
    expect("orange", 255, 0xa5, 0, 255);
    expect("purple", 0x80, 0, 0x80, 255);
    expect("pink", 255, 0xc0, 0xcb, 255);
    expect("brown", 0xa5, 0x2a, 0x2a, 255);
    expect("cyan", 0, 255, 255, 255);
    expect("magenta", 255, 0, 255, 255);
    expect("lime", 0, 255, 0, 255);
    expect("navy", 0, 0, 0x80, 255);
    expect("teal", 0, 0x80, 0x80, 255);
    expect("silver", 0xc0, 0xc0, 0xc0, 255);
    expect("maroon", 0x80, 0, 0, 255);
    expect("olive", 0x80, 0x80, 0, 255);
    expect("transparent", 0, 0, 0, 0);
    expect("RED", 255, 0, 0, 255);
    expect("Red", 255, 0, 0, 255);
    expect("TRANSPARENT", 0, 0, 0, 0);
    expect(" red ", 255, 0, 0, 255);
    expect("\n white\t", 255, 255, 255, 255);

    // Rejects: the server leaks these and the browser draws nothing for them.
    reject("url(#hatch-pattern-3)");
    reject("url(#x)");
    reject("none");
    reject("");
    reject(" ");
    reject("#");
    reject("#1");
    reject("#12");
    reject("#12345");
    reject("#1234567");
    reject("#123456789");
    reject("#ggg");
    reject("#12345g");
    reject("# fff");
    reject("#ff f");
    reject("fff");
    reject("123456");
    reject("reddish");
    reject("re");
    reject("red blue");
    reject("rgb(1,2,3)");
    reject("currentColor");
    reject("inherit");
    // A name with a NUL or a length shorter than the text must not match by accident.
    CHECK(!colour_parse("red", 2, &(rgba8_t){0}));
    CHECK(!colour_parse("red\0x", 5, &(rgba8_t){0}));
    CHECK(!colour_parse(NULL, 0, &(rgba8_t){0}));
    // Length is authoritative: a longer buffer with a valid prefix parses just the prefix.
    {
        rgba8_t c;
        CHECK(colour_parse("#fff000", 4, &c) && c.r == 255 && c.g == 255 && c.b == 255 && c.a == 255);
        CHECK(colour_parse("redx", 3, &c) && c.r == 255 && c.g == 0 && c.b == 0);
    }
    // rgba8_to_rgb packs 0x00RRGGBB.
    CHECK(rgba8_to_rgb((rgba8_t){ 0x12, 0x34, 0x56, 0x78 }) == 0x123456u);

    if (failures) { printf("test_colour: %d failure(s)\n", failures); return 1; }
    printf("test_colour: OK\n");
    return 0;
}
