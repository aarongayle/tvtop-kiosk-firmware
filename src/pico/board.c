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
};

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
