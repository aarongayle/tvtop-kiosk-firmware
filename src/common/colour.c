// CSS colour parsing (see colour.h). Anything CSS would not parse is rejected so the caller draws
// nothing, which is what the browser does with the pattern/url fills the server leaks.
#include "colour.h"

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

typedef struct { const char *name; uint32_t rgba; } named_t;

// CSS Level 4 named colours the games plausibly use (alpha 0xff except transparent).
static const named_t names[] = {
    {"aqua", 0x00ffffffu}, {"beige", 0xf5f5dcffu}, {"black", 0x000000ffu}, {"blue", 0x0000ffffu},
    {"brown", 0xa52a2affu}, {"chocolate", 0xd2691effu}, {"coral", 0xff7f50ffu}, {"crimson", 0xdc143cffu},
    {"cyan", 0x00ffffffu}, {"darkblue", 0x00008bffu}, {"darkgray", 0xa9a9a9ffu}, {"darkgreen", 0x006400ffu},
    {"darkgrey", 0xa9a9a9ffu}, {"darkorange", 0xff8c00ffu}, {"darkred", 0x8b0000ffu}, {"dimgray", 0x696969ffu},
    {"dimgrey", 0x696969ffu}, {"firebrick", 0xb22222ffu}, {"forestgreen", 0x228b22ffu}, {"fuchsia", 0xff00ffffu},
    {"gold", 0xffd700ffu}, {"goldenrod", 0xdaa520ffu}, {"gray", 0x808080ffu}, {"green", 0x008000ffu},
    {"grey", 0x808080ffu}, {"indigo", 0x4b0082ffu}, {"ivory", 0xfffff0ffu}, {"khaki", 0xf0e68cffu},
    {"lavender", 0xe6e6faffu}, {"lightblue", 0xadd8e6ffu}, {"lightgray", 0xd3d3d3ffu}, {"lightgreen", 0x90ee90ffu},
    {"lightgrey", 0xd3d3d3ffu}, {"lime", 0x00ff00ffu}, {"magenta", 0xff00ffffu}, {"maroon", 0x800000ffu},
    {"navy", 0x000080ffu}, {"olive", 0x808000ffu}, {"orange", 0xffa500ffu}, {"orchid", 0xda70d6ffu},
    {"pink", 0xffc0cbffu}, {"plum", 0xdda0ddffu}, {"purple", 0x800080ffu}, {"red", 0xff0000ffu},
    {"salmon", 0xfa8072ffu}, {"sienna", 0xa0522dffu}, {"silver", 0xc0c0c0ffu}, {"skyblue", 0x87ceebffu},
    {"slategray", 0x708090ffu}, {"slategrey", 0x708090ffu}, {"steelblue", 0x4682b4ffu}, {"tan", 0xd2b48cffu},
    {"teal", 0x008080ffu}, {"tomato", 0xff6347ffu}, {"transparent", 0x00000000u}, {"turquoise", 0x40e0d0ffu},
    {"violet", 0xee82eeffu}, {"wheat", 0xf5deb3ffu}, {"white", 0xffffffffu}, {"yellow", 0xffff00ffu},
};

static bool name_eq(const char *s, size_t len, const char *name) {
    size_t i = 0;
    for (; i < len; i++) {
        if (name[i] == 0 || lower(s[i]) != name[i]) return false;
    }
    return name[i] == 0;
}

bool colour_parse(const char *s, size_t len, rgba8_t *out) {
    if (!s) return false;
    while (len && (s[0] == ' ' || s[0] == '\t' || s[0] == '\n' || s[0] == '\r')) { s++; len--; }
    while (len && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\n' || s[len - 1] == '\r')) len--;
    if (len == 0) return false;

    if (s[0] == '#') {
        const char *h = s + 1;
        size_t n = len - 1;
        int v[8];
        if (n != 3 && n != 4 && n != 6 && n != 8) return false;
        for (size_t i = 0; i < n; i++) {
            v[i] = hexval(h[i]);
            if (v[i] < 0) return false;
        }
        if (n <= 4) {
            // Short form: each nibble is doubled, so "f" -> 0xff.
            out->r = (uint8_t)(v[0] * 17);
            out->g = (uint8_t)(v[1] * 17);
            out->b = (uint8_t)(v[2] * 17);
            out->a = n == 4 ? (uint8_t)(v[3] * 17) : 255;
        } else {
            out->r = (uint8_t)(v[0] << 4 | v[1]);
            out->g = (uint8_t)(v[2] << 4 | v[3]);
            out->b = (uint8_t)(v[4] << 4 | v[5]);
            out->a = n == 8 ? (uint8_t)(v[6] << 4 | v[7]) : 255;
        }
        return true;
    }

    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        if (name_eq(s, len, names[i].name)) {
            uint32_t c = names[i].rgba;
            out->r = (uint8_t)(c >> 24);
            out->g = (uint8_t)(c >> 16);
            out->b = (uint8_t)(c >> 8);
            out->a = (uint8_t)c;
            return true;
        }
    }
    return false;
}
