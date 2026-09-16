// Board wiring and video mode selection.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#if KIOSK_HSTX
// Pico 2 W (RP2350) with the Adafruit PiCowBell HSTX DVI (product 6363), driven by HSTX on
// GPIO12-19: D0 12/13, CK 14/15, D2 16/17, D1 18/19 (+ is the lower GPIO). The HSTX clock is the
// system clock divided by hstx_clk_div, and one pixel is five HSTX clock cycles (ten TMDS bits,
// two per cycle), so the pixel clock is sys_clk / (5 * hstx_clk_div).
#include "hstx_line.h"

typedef enum { VIDEO_720P60 = 0, VIDEO_960X540P60 = 1, VIDEO_480P60 = 2, VIDEO_1080P30 = 3, VIDEO_1080P25 = 4, VIDEO_1080P24 = 5, VIDEO_MODE_COUNT } video_mode_t;

typedef struct {
    const char *name;
    uint16_t w, h;
    uint32_t sys_clk_khz;      // default system clock; the Wi-Fi channel may pick another (board.c)
    uint8_t vreg;              // enum vreg_voltage value
    uint8_t hstx_clk_div;      // clk_hstx = clk_sys / this (1..3)
    hstx_timing_t timing;
} video_mode_info_t;

#define KIOSK_MODE_NAMES "720p60|960x540p60|480p60|1080p30|1080p25|1080p24"
#ifndef KIOSK_DEFAULT_VIDEO_MODE
#define KIOSK_DEFAULT_VIDEO_MODE VIDEO_720P60
#endif

const video_mode_info_t *video_mode_info(video_mode_t m);
video_mode_t video_mode_from_name(const char *name);
uint32_t video_mode_clock_khz(video_mode_t m, uint8_t wifi_channel);

#else

#include <stdint.h>
#include "dvi.h"   // libdvi headers must be entered through dvi.h (they include each other)
#include "dvi_serialiser.h"
#include "dvi_timing.h"

// Adafruit PiCowBell HSTX DVI (product 6363) on a Pico / Pico 2:
//   GPIO12 D0+  GPIO13 D0−   (TMDS lane 0, blue)
//   GPIO18 D1+  GPIO19 D1−   (TMDS lane 1, green)
//   GPIO16 D2+  GPIO17 D2−   (TMDS lane 2, red)
//   GPIO14 CK+  GPIO15 CK−
// Identical to libdvi's pico_sock_cfg. + is the lower GPIO of each pair, so no inversion.
#define KIOSK_DVI_CFG pico_sock_cfg

typedef enum { VIDEO_720P30 = 0, VIDEO_720P30_RB = 1, VIDEO_480P60 = 2, VIDEO_720X480P60 = 3, VIDEO_960X540P60 = 4, VIDEO_1066X600P50 = 5, VIDEO_MODE_COUNT } video_mode_t;

typedef struct {
    const char *name;
    uint16_t w, h;
    uint32_t sys_clk_khz;      // == TMDS bit clock
    uint8_t vreg;              // enum vreg_voltage value
    const struct dvi_timing *timing;
} video_mode_info_t;

#define KIOSK_MODE_NAMES "720p30|720p30rb|480p60|720x480p60|960x540p60|1066x600p50"

const video_mode_info_t *video_mode_info(video_mode_t m);
video_mode_t video_mode_from_name(const char *name);
// The system (TMDS bit) clock for a mode, chosen so that no harmonic of the DVI pixel clock lands
// inside the Wi-Fi channel. wifi_channel 0 or out of range means unknown: the mode's own clock.
uint32_t video_mode_clock_khz(video_mode_t m, uint8_t wifi_channel);   // "720p30" | "720p30rb" | "480p60" | "720x480p60" | "960x540p60" | "1066x600p50"; default 720p30

#ifndef KIOSK_DEFAULT_VIDEO_MODE
#define KIOSK_DEFAULT_VIDEO_MODE VIDEO_720P30
#endif

// Status LED: Pico W's LED is on the cyw43 (CYW43_WL_GPIO_LED_PIN), only usable after wifi init.

#endif
