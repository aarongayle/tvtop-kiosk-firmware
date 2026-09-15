#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct { uint8_t r, g, b, a; } rgba8_t;

// Parses "#rgb", "#rgba", "#rrggbb", "#rrggbbaa" (case-insensitive) and the CSS colour names
// the games use (at least: black white red green blue yellow gray grey orange purple pink brown
// cyan magenta transparent). Returns false for anything else — "none", "url(#…)", garbage — which
// callers treat as "draw nothing", matching the browser. Leading/trailing spaces are tolerated.
bool colour_parse(const char *s, size_t len, rgba8_t *out);

static inline uint32_t rgba8_to_rgb(rgba8_t c) { return ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | c.b; }
