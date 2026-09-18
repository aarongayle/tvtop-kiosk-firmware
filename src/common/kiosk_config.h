// Every tunable in one place. Defaults are the RP2040 profile without TLS; the CMake build
// overrides per target (see CMakeLists.txt) and the host build overrides for tests.
#pragma once
#include <stdint.h>

#define KIOSK_FW_VERSION "0.2.0"   // 0.2.0: frames may declare their canvas (w/h), e.g. 1920×1080
#ifndef KIOSK_MODEL
#define KIOSK_MODEL "pico-w" // CMake sets it from PICO_BOARD (pico-w, pico2-w); this covers host builds
#endif
#define KIOSK_PROTOCOL_VERSION 3

// Wire canvas (protocol) and the largest output mode we drive.
#define CANVAS_W 1280
#define CANVAS_H 720
#ifndef KIOSK_HSTX
#define KIOSK_HSTX 0
#endif
#ifndef OUT_MAX_W
#if KIOSK_HSTX
#define OUT_MAX_W 1920   // the Pico 2 W can output 1080p at 24-30 Hz (the 720p60 bit rate)
#define OUT_MAX_H 1080
#else
#define OUT_MAX_W 1280
#define OUT_MAX_H 720
#endif
#endif

// Fixed point: device coordinates carry 3 fractional bits ("px8").
#define PX8_SHIFT 3
#define PX8_ONE (1 << PX8_SHIFT)
#define PX8_HALF (PX8_ONE / 2)
#define PX8_MAX (4095 * PX8_ONE)   // stored geometry is int16; clamp to ±4095 px

#ifndef KIOSK_BAND_LINES
#define KIOSK_BAND_LINES 8          // scanlines rasterised per band (8 bpp band buffer)
#endif
// Platform: KIOSK_HSTX selects the RP2350 HSTX video backend (Pico 2 W); KIOSK_FULL_COLOUR keeps
// palette colours exact instead of rounding them to DC-balanced TMDS levels for the RP2040 encoder.
#ifndef KIOSK_HSTX
#define KIOSK_HSTX 0
#endif
#ifndef KIOSK_FULL_COLOUR
#define KIOSK_FULL_COLOUR 0
#endif

#ifndef KIOSK_MAX_OPS
#define KIOSK_MAX_OPS 1000          // dynamic frame op table
#endif
#ifndef KIOSK_ARENA_BYTES
#define KIOSK_ARENA_BYTES 6144      // strings (text, decoded bitmaps) of the dynamic frame
#endif
#ifndef KIOSK_MAX_PAINTS
#define KIOSK_MAX_PAINTS 128
#endif
#ifndef KIOSK_MAX_DEFS
#define KIOSK_MAX_DEFS 1536         // static geometry definitions per set (ids are base-36 ordinals)
#endif
#ifndef KIOSK_SCRATCH_BYTES
#define KIOSK_SCRATCH_BYTES (28 * 1024)   // union: render scratch | static-decode staging
#endif
// Decode-time split of the scratch union: [0, KIOSK_DECODE_RECORDER_BYTES) path command recorder,
// the rest (>= GEOM_SCRATCH_MIN) for the geometry store's index + record staging.
#ifndef KIOSK_DECODE_RECORDER_BYTES
#define KIOSK_DECODE_RECORDER_BYTES (20 * 1024)   // Europe's largest territory records to ~7 KB
#endif
// Render-time split: [0, KIOSK_BAND_LINES*OUT_MAX_W) band buffer, then RASTER_SCRATCH_BYTES.
#ifndef KIOSK_LINEPOOL_BYTES
#define KIOSK_LINEPOOL_BYTES (84 * 1024)   // a full World map at 960x540 needs ~65 KB; the rest is headroom for denser maps (from ~18 KB of idle heap)
#endif
#ifndef KIOSK_MAX_CROSSINGS
#define KIOSK_MAX_CROSSINGS 128     // polygon edge crossings kept per scanline
#endif
#ifndef KIOSK_MAX_URL
#define KIOSK_MAX_URL 256
#endif
#ifndef KIOSK_FONT_BITMAP_MAX
#define KIOSK_FONT_BITMAP_MAX 26    // px; larger text is rendered from outlines
#endif
#ifndef KIOSK_PATH_TOLERANCE
#define KIOSK_PATH_TOLERANCE 0.2f   // curve flattening tolerance, device pixels
#endif
#ifndef KIOSK_GLYPH_MAX_VERTS
#define KIOSK_GLYPH_MAX_VERTS 768   // flattened outline vertices per glyph
#endif
#ifndef KIOSK_MAX_BITMAP_BYTES
#define KIOSK_MAX_BITMAP_BYTES 2048 // decoded 'b' op payload (QR up to ~120 modules)
#endif
#ifndef KIOSK_MAX_STATIC_ID
#define KIOSK_MAX_STATIC_ID 32
#endif

// Line pool encoding: each scanline is (index, run-1) byte pairs, runs of 1..256 pixels.
#if KIOSK_HSTX
// HSTX lines are a table byte and then the same pairs (hstx_line.h).
#define LINE_MAX_BYTES (OUT_MAX_W * 2 + 1)
#else
#define LINE_MAX_BYTES (OUT_MAX_W * 2)
#endif

// Flash layout (RP2040 Pico W, 2 MB). Offsets are from the start of flash.
#ifndef KIOSK_GEOM_FLASH_OFFSET
#define KIOSK_GEOM_FLASH_OFFSET 0x100000u
#endif
#ifndef KIOSK_GEOM_FLASH_SIZE
#define KIOSK_GEOM_FLASH_SIZE 0xEF000u
#endif
#ifndef KIOSK_CONFIG_FLASH_OFFSET
#define KIOSK_CONFIG_FLASH_OFFSET 0x1FF000u
#endif
// The ESP modem's firmware image, which the kiosk writes to it over the UART (modem_flash.c). It
// is not linked into the firmware: it is its own region, installed by its own UF2, so the two can
// be built and flashed independently and a modem update does not mean relinking the kiosk.
// Default 2 MB, which on the Pico 2 W's 4 MB is past the geometry cache and clear of the config
// sector. v3's RP2354A has 2 MB in total, so that build has to place it deliberately.
#ifndef KIOSK_MODEM_IMAGE_OFFSET
#define KIOSK_MODEM_IMAGE_OFFSET 0x200000u
#endif
