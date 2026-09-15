// Board wiring and video mode selection.
#pragma once
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

typedef enum { VIDEO_720P30 = 0, VIDEO_720P30_RB = 1, VIDEO_480P60 = 2, VIDEO_MODE_COUNT } video_mode_t;

typedef struct {
    const char *name;
    uint16_t w, h;
    uint32_t sys_clk_khz;      // == TMDS bit clock
    uint8_t vreg;              // enum vreg_voltage value
    const struct dvi_timing *timing;
} video_mode_info_t;

const video_mode_info_t *video_mode_info(video_mode_t m);
video_mode_t video_mode_from_name(const char *name);   // "720p30" | "720p30rb" | "480p60"; default 720p30

#ifndef KIOSK_DEFAULT_VIDEO_MODE
#define KIOSK_DEFAULT_VIDEO_MODE VIDEO_720P30
#endif

// Status LED: Pico W's LED is on the cyw43 (CYW43_WL_GPIO_LED_PIN), only usable after wifi init.
