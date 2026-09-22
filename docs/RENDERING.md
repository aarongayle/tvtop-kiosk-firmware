# Rendering

The renderer draws protocol-v3 frames exactly as `helpers/Kiosk/SceneCanvas.js` does in the
browser. This documents the interpretation, the defaults, and the tolerances.

## Scaling

The wire canvas is 1280×720. `k = min(out_w/1280, out_h/720)` (16.16 fixed point) and the unused
strip of a non-16:9 mode is split evenly (letterboxing). Every coordinate, size, radius and stroke
width is scaled by `k` at decode time and stored in 1/8-pixel units; text sizes become integer
pixel sizes. At 720p `k = 1`.

## Ops

| Op | Interpretation |
|---|---|
| `["r", x, y, w, h, fill, radius?]` | filled rectangle; `radius` defaults to 0 and is clamped to half the shorter side |
| `["l", x1, y1, x2, y2, color, width?]` | stroke with butt caps; width defaults to 2; a zero-length line draws nothing; widths under 1 px draw as 1 px |
| `["c", cx, cy, r, fill]` | filled circle |
| `["t", x, y, text, size, color, align?, weight?]` | one line of text; `y` is the alphabetic baseline; align 0/1/2 = left/centre/right; weight 1 = bold |
| `["b", x, y, size, modules, base64, color?]` | 1-bit bitmap scaled to a square; rows byte-padded, MSB leftmost; cells tile without seams; clear cells are transparent; colour defaults to black |
| `["i", x, y, size, name, color]` | one of the ten icons (24-unit paths from SceneCanvas.js); unknown names draw nothing |
| `["k", x, y, w, h]` / `["k"]` | clip rectangle for the following ops / clear it |
| `["u", id, paint]` | cached geometry `id` with `paints[paint] = [fill, stroke, width]`; nonzero fill, then the stroke centred on the edge |
| `["u", id, paint, x, y, s]` | the same definition placed: every point `p` of it maps to `(x + p.x·s/1000, y + p.y·s/1000)`; see [Placed geometry](#placed-geometry) |

Ops with an unparseable colour are dropped (a paint side that is unparseable is "none"), which is
what the browser does with `url(#…)` fills.

## Colours and anti-aliasing

Every colour is quantised per component to the nearest of the 52 TMDS-balanced values (error ≤ 8
of 255) and stored in a 256-entry palette that persists across frames. Glyph edges and alpha
fills blend against what is already on the band through the palette (cached mixes), so text is
anti-aliased at 4 levels. Icons are rendered with the same anti-aliased filler. Map polygons and
rectangles are point-sampled (pixel centres), matching the crispness of a 1:1 SVG raster.

## Text

Sizes up to 26 px use pre-rendered Roboto bitmaps (2-bit coverage, nearest available size); larger
text is rendered from the glyph outlines at the exact size. Missing glyphs (emoji) advance by the
space width and draw nothing. No kerning.

## Static sets

`static.defs` stream straight into the flash geometry cache as they arrive: each path is recorded
in a compact command form (≤ 20 KB), and when its transform arrives it is flattened (0.2 px
tolerance) into device-space vertices written behind a 16-byte trailer. Records are therefore
limited only by the region (956 KB). The header and index are written last, so a power cut during
a transfer leaves either the previous complete set or none. The header carries a layout version
(4 since group records); a set cached by older firmware, which would have dropped its groups, does
not open, so the kiosk fetches it again.

The kinds are `path`, `poly`, `circle` and `group`. A group is `["group", [[member, paint], …]]`:
the ids of other definitions in the same set, each with its own paint index, drawn in order. Members
are resolved when the group is drawn, so they may come before or after it in `defs`. A member that
does not parse is dropped while decoding; one that names an absent id, another group (including
itself) or a paint the frame does not have is skipped when drawing. At most 32 members
(`GEOM_GROUP_MAX`) are kept; the rest are dropped. A `u` op naming a group draws every member with
the member's paint and ignores its own paint index, in either form.

## Placed geometry

`["u", id, paint, x, y, s]` draws a definition at a new position and size, so one copy in the static
set (an emoji, authored at the canvas origin) can appear anywhere. `x` and `y` are canvas
coordinates like any other op's; `s` is an integer scale in thousandths. Point `p` of the definition,
after its own `dx dy sx sy`, lands at `(x + p.x·s/1000, y + p.y·s/1000)`; a circle's radius and the
paint's stroke width are multiplied by `s/1000`. It is the placed form only when all three are
numbers; with fewer than six elements, or anything else in those places, it is the plain form.
`s ≤ 0` or `s > 32767` drops the op.

Stored vertices are device px8, so the renderer applies the placement in device space:
`p' = (x, y)dev + (p − origin)·s/1000`, where `origin` is the device position of the canvas origin
(the letterbox offset), which must not be scaled with the vertex. The scale is held as `s·1024/1000`
(1000 is exactly 1024), which keeps every product in 32 bits.

The rasteriser maps each vertex as it reads it (`raster_fill_poly_xf`, `raster_stroke_poly_xf`),
so there is no scratch buffer and no limit on the size of a placed definition: no RAM, and a
32-bit multiply, add and clamp per coordinate. The edge walk for placed geometry is a separate loop,
so unplaced geometry runs exactly the code it ran before. Band culling uses the placed bounding box
(the mapping is monotonic, so it is the mapped record box). A group culls each member against the
band by its own box, which rejects every band the union of the boxes would and needs no extra pass
over the members. On the host, a 1080p frame of 85 placed five-path groups renders in the same time
as the same picture with a separate definition per copy.

A placement whose box leaves ±4095 px (the int16 px8 range of stored geometry) is not drawn at all.
Clamping its vertices would bend edges that can still be on screen.

Curves are flattened once, when the definition is decoded, at its defined size (0.2 px). Placed at
`s`, the chord error grows to `0.2·s/1000` px: at 3× (an emoji of ~160 px at 1080p) outlines show
slight facets, and by 6× they are plainly polygons. To place something much larger than defined,
define it at the larger size and place it with `s < 1000`.

## Error statuses

| Status | Meaning | Kiosk behaviour |
|---|---|---|
| `FD_ERR_JSON` | malformed body | keep the last frame, back off |
| `FD_ERR_VERSION` | `v` is not 3 | keep the last frame, overlay the warning badge |
| `FD_ERR_STATIC_MISSING` | frame names a static id the cache lacks | keep the last frame, restart from `/v1/config` |
| `FD_ERR_STATIC_STORE` | the cache could not store the set | keep the last frame, back off |
| `FD_ERR_TOO_MANY_OPS` | more than `KIOSK_MAX_OPS` ops | draw what fits |

## Host tools

```
host/build/render_frame test/fixtures/gc-europe.json --stats -o out.png   # decode + render + numbers
tools/render-references.sh                                                # browser-equivalent references (resvg)
host/build/imgdiff out.png host/out/ref/gc-europe.png diff.png            # compare
```

`--stats` prints the line-pool bytes the frame needs on the device (the number that sizes
`KIOSK_LINEPOOL_BYTES`), spans, edge visits and timings.
