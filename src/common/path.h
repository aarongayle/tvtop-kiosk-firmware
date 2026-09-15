// SVG path data ("d") and polygon "points" → flattened device-space vertices.
//
// Two front ends share one scanner:
//  * path_recorder_t  — streams path text into a compact command buffer (used for static defs,
//    whose transform arrives *after* the path string), replayed later through a flattener.
//  * path_flattener_t — streams path text (or replays a recording) through an affine transform
//    and curve flattening, emitting px8 vertices with contour starts.
// Supports M m L l H h V v C c S s Q q T t A a Z z, implicit repeated commands, and the usual
// number syntax ("1.5.3", "-2-3", exponents). Arcs become cubics. Curves are flattened to
// KIOSK_PATH_TOLERANCE in device pixels.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "kiosk_config.h"

typedef struct { float sx, sy, dx, dy; } affine_t;   // x' = dx + x*sx, y' = dy + y*sy

// contour_start is true for the first vertex of each subpath (M). Every subpath is implicitly
// closed for filling; strokes use `closed` from the Z command (see path_flattener_t.last_closed).
typedef void (*path_vertex_cb)(void *ctx, int32_t x8, int32_t y8, bool contour_start);

typedef struct {
    // scanner state
    uint8_t cmd;            // current command letter, 0 = none
    uint8_t argi;           // args collected for current command
    float args[8];
    char num[24]; uint8_t numlen;
    bool in_num, num_has_dot, num_has_exp, num_prev_exp;
    bool error;
} path_scanner_t;

typedef struct {
    path_scanner_t sc;
    uint8_t *buf; size_t cap, len;   // compact command stream
    bool overflow;
} path_recorder_t;

void path_recorder_init(path_recorder_t *r, uint8_t *buf, size_t cap);
bool path_recorder_feed(path_recorder_t *r, const char *text, size_t len);          // path "d"
bool path_recorder_feed_points(path_recorder_t *r, const char *text, size_t len);   // polygon "points"
bool path_recorder_finish(path_recorder_t *r);   // flushes; false on overflow/syntax error

typedef struct {
    path_scanner_t sc;
    affine_t t;
    float tol;                       // device px
    path_vertex_cb cb; void *ctx;
    float cx, cy;                    // current point (source space)
    float sx0, sy0;                  // subpath start
    float lcx, lcy; uint8_t last_cmd; // last control point for S/T reflection
    bool open;                       // a subpath has vertices
    bool polygon_mode;               // points-attribute semantics
    uint32_t nverts;
} path_flattener_t;

void path_flattener_init(path_flattener_t *f, const affine_t *t, float tolerance_px, path_vertex_cb cb, void *ctx);
bool path_flattener_feed(path_flattener_t *f, const char *text, size_t len);
bool path_flattener_feed_points(path_flattener_t *f, const char *text, size_t len);
bool path_flattener_finish(path_flattener_t *f);
// Replays a recording (from path_recorder_t) through the flattener.
bool path_flattener_replay(path_flattener_t *f, const uint8_t *rec, size_t len);

// Convenience for small paths in memory (icons): parse+flatten in one go.
bool path_flatten_string(const char *d, const affine_t *t, float tol, path_vertex_cb cb, void *ctx);
