#if KIOSK_HSTX
// Pico 2 W video modes. Every mode is a standard 60 Hz timing, so a monitor's picture-in-picture
// handles it like any other source.
#include <string.h>
#include "hardware/vreg.h"
#include "board.h"

static const video_mode_info_t modes[VIDEO_MODE_COUNT] = {
    // CEA-861 VIC 4, 1650 x 750 totals, positive syncs. 372 MHz / 5 = 74.4 MHz pixels, 60.12 Hz: the
    // overclock PicoHDMI runs 720p60 at, at the regulator's 1.30 V ceiling.
    [VIDEO_720P60] = { "720p60", 1280, 720, 372000, VREG_VOLTAGE_1_30, 1, { 110, 40, 220, 1280, 5, 5, 20, 720, true, true } },
    // qHD, the Pico W's mode, for sinks that refuse 720p60: 372 MHz / (5 * 2) = 37.2 MHz pixels.
    [VIDEO_960X540P60] = { "960x540p60", 960, 540, 372000, VREG_VOLTAGE_1_30, 2, { 16, 32, 96, 960, 2, 6, 15, 540, true, true } },
    // VGA: 252 MHz / (5 * 2) = 25.2 MHz pixels, negative syncs. Past the RP2350's 150 MHz rating, so 1.20 V.
    [VIDEO_480P60] = { "480p60", 640, 480, 252000, VREG_VOLTAGE_1_20, 2, { 16, 96, 48, 640, 10, 2, 33, 480, false, false } },
    // 1080p at the 720p60 pixel clock: CEA VIC 34 / 33 / 32 differ only in horizontal blanking
    // (2200 / 2640 / 2750 total pixels, 1125 lines). 30 Hz is 33.8 kHz, inside a 30 kHz monitor;
    // 25 and 24 Hz are 28.2 and 27.1 kHz, for TVs. 60.12 Hz's clock gives 30.06 / 25.05 / 24.05 Hz.
    [VIDEO_1080P30] = { "1080p30", 1920, 1080, 372000, VREG_VOLTAGE_1_30, 1, { 88, 44, 148, 1920, 4, 5, 36, 1080, true, true } },
    [VIDEO_1080P25] = { "1080p25", 1920, 1080, 372000, VREG_VOLTAGE_1_30, 1, { 528, 44, 148, 1920, 4, 5, 36, 1080, true, true } },
    [VIDEO_1080P24] = { "1080p24", 1920, 1080, 372000, VREG_VOLTAGE_1_30, 1, { 638, 44, 148, 1920, 4, 5, 36, 1080, true, true } },
};

// The DVI clock lane radiates harmonics of the pixel clock, and the PiCowBell sits under the Wi-Fi
// antenna (see docs/HARDWARE.md). Per 2.4 GHz channel, the exact PLL clock whose nearest harmonic is
// furthest from the channel, keeping 720p60 between 59 and 61 Hz; index 0 is channel 1.
static const uint32_t clock_720p60_by_channel[14] = { 372000, 372000, 372000, 372000, 372000, 372000, 375000, 375000, 369000, 369000, 369000, 369000, 372000, 372000 };
// 960x540 has the same pixel clock per system clock as the Pico W's, so its table carries over.
static const uint32_t clock_960x540_by_channel[14] = { 368000, 369000, 364000, 354000, 366000, 372000, 351000, 368000, 369000, 364000, 354000, 360000, 372000, 368000 };

uint32_t video_mode_clock_khz(video_mode_t m, uint8_t wifi_channel) {
    const video_mode_info_t *info = video_mode_info(m);
    if (wifi_channel >= 1 && wifi_channel <= 14) {
        // The 1080p modes share 720p60's pixel clock, so its channel table applies to them too.
        if (m == VIDEO_720P60 || m == VIDEO_1080P30 || m == VIDEO_1080P25 || m == VIDEO_1080P24) return clock_720p60_by_channel[wifi_channel - 1];
        if (m == VIDEO_960X540P60) return clock_960x540_by_channel[wifi_channel - 1];
    }
    return info->sys_clk_khz;
}

const video_mode_info_t *video_mode_info(video_mode_t m) {
    if ((unsigned)m >= VIDEO_MODE_COUNT) m = KIOSK_DEFAULT_VIDEO_MODE;
    return &modes[m];
}

video_mode_t video_mode_from_name(const char *name) {
    if (name)
        for (unsigned i = 0; i < VIDEO_MODE_COUNT; i++)
            if (strcmp(name, modes[i].name) == 0) return (video_mode_t)i;
    return KIOSK_DEFAULT_VIDEO_MODE;
}

#else
// Video modes: the TMDS bit clock is the system clock (one PIO cycle per bit), so each mode names
// the overclock it needs. Voltages follow PicoDVI's experience: 1.20 V is plenty for 252 MHz,
// 720p needs the regulator's maximum.
#include <string.h>
#include "hardware/vreg.h"
#include "board.h"

static const video_mode_info_t modes[VIDEO_MODE_COUNT] = {
    [VIDEO_720P30]    = { "720p30",   1280, 720, 372000, VREG_VOLTAGE_1_30, &dvi_timing_1280x720p_30hz },
    [VIDEO_720P30_RB] = { "720p30rb", 1280, 720, 319200, VREG_VOLTAGE_1_25, &dvi_timing_1280x720p_reduced_30hz },
    [VIDEO_480P60]    = { "480p60",   640,  480, 252000, VREG_VOLTAGE_1_20, &dvi_timing_640x480p_60hz },
    // CEA-861 VIC 2/3 exactly (858x525, 27 MHz, negative sync). HDMI sinks that do not list VGA
    // 640x480 almost always list this, e.g. small HDMI panels that only accept their CEA modes.
    [VIDEO_720X480P60] = { "720x480p60", 720, 480, 270000, VREG_VOLTAGE_1_20, &dvi_timing_720x480p_60hz },
    // qHD: a quarter of 1080p (and a sixteenth of 4K), so sinks scale it by whole numbers. 33.7 kHz
    // horizontal, 60 Hz vertical: inside the range of monitors that reject PicoDVI's 22 kHz 720p30.
    // Not a CEA mode, so it relies on the sink accepting in-range timings it does not list.
    [VIDEO_960X540P60] = { "960x540p60", 960, 540, 372000, VREG_VOLTAGE_1_30, &dvi_timing_960x540p_60hz },
    // The sharpest mode inside a 30 kHz / 48 Hz monitor at this overclock: 23% more pixels than
    // qHD. Not a whole-number scale to 4K, so the sink's scaler softens it slightly.
    [VIDEO_1066X600P50] = { "1066x600p50", 1066, 600, 368000, VREG_VOLTAGE_1_30, &dvi_timing_1066x600p_50hz },
};

// The DVI clock lane is a clean square wave at the pixel clock, so it radiates a comb of harmonics
// every ~37 MHz, and the PiCowBell sits right under the Pico W's antenna. At 372 MHz (37.2 MHz
// pixels) the 65th harmonic is 2418 MHz, inside Wi-Fi channel 1: the radio stops getting transmit
// credits under traffic ("[CYW43] STALL ... timeout") while silencing the DVI pins cures it. Each
// channel therefore gets the exact PLL frequency (56-62 Hz refresh, <= 372 MHz where possible)
// whose nearest harmonic is furthest from the channel centre; every entry clears it by >= 14 MHz.
// Generated for 960x540 (1104 x 563 totals); index 0 is channel 1.
static const uint32_t clock_960x540_by_channel[14] = { 368000, 369000, 364000, 354000, 366000, 372000, 351000, 368000, 369000, 364000, 354000, 360000, 372000, 368000 };

// 1066x600 (1186 x 621 totals): the same search, restricted to clocks that keep 30.5 kHz and 49 Hz
// so no channel pushes the monitor onto its range limits. Every entry clears its channel by >= 14 MHz.
static const uint32_t clock_1066x600_by_channel[14] = { 368000, 369000, 364000, 376000, 366000, 372000, 378000, 368000, 369000, 364000, 376000, 366000, 366000, 378000 };

uint32_t video_mode_clock_khz(video_mode_t m, uint8_t wifi_channel) {
    const video_mode_info_t *info = video_mode_info(m);
    if (m == VIDEO_960X540P60 && wifi_channel >= 1 && wifi_channel <= 14) return clock_960x540_by_channel[wifi_channel - 1];
    if (m == VIDEO_1066X600P50 && wifi_channel >= 1 && wifi_channel <= 14) return clock_1066x600_by_channel[wifi_channel - 1];
    return info->sys_clk_khz;
}

const video_mode_info_t *video_mode_info(video_mode_t m) {
    if ((unsigned)m >= VIDEO_MODE_COUNT) m = KIOSK_DEFAULT_VIDEO_MODE;
    return &modes[m];
}

video_mode_t video_mode_from_name(const char *name) {
    if (name)
        for (unsigned i = 0; i < VIDEO_MODE_COUNT; i++)
            if (strcmp(name, modes[i].name) == 0) return (video_mode_t)i;
    return KIOSK_DEFAULT_VIDEO_MODE;
}

#endif
