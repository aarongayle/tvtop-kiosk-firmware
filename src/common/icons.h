// The ten protocol icons, as 24×24 SVG path data copied from SceneCanvas.js (the reference).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *name;
    const char *d;          // SVG path data in a 24×24 box
    bool stroke;            // true: stroke width 2, round caps/joins, no fill
    bool evenodd;           // fill rule
} icon_def_t;

#define ICON_COUNT 10
extern const icon_def_t icon_defs[ICON_COUNT];
int icon_lookup(const char *name, size_t len);   // index or -1
