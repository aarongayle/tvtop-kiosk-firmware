// Minimal USB serial program: touches no GPIO, no ADC, no radio. It only proves the chip boots,
// clocks up and enumerates. BUILD_KIND says whether it runs from flash or from SRAM.
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"

int main(void) {
    stdio_init_all();
    for (unsigned n = 1;; n++) {
        printf("usb_hello (%s build): alive %u s, clk_sys %lu Hz\n", BUILD_KIND, n, (unsigned long)clock_get_hz(clk_sys));
        sleep_ms(1000);
    }
}
