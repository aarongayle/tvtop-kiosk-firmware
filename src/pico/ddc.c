#include "ddc.h"
#include "pico/stdlib.h"

#define SDA 4
#define SCL 5
#define HALF_US 1000

static bool stuck;

static void rel(uint g) { gpio_set_dir(g, GPIO_IN); }
static void low(uint g) { gpio_put(g, 0); gpio_set_dir(g, GPIO_OUT); }
static void scl_high(void) {   // release SCL and wait for it to rise (clock stretching)
    rel(SCL);
    for (int i = 0; i < 100; i++) { if (gpio_get(SCL)) return; sleep_us(100); }
    stuck = true;
}
static void start(void) { rel(SDA); scl_high(); sleep_us(HALF_US); low(SDA); sleep_us(HALF_US); low(SCL); sleep_us(HALF_US); }
static void stop(void) { low(SDA); sleep_us(HALF_US); scl_high(); sleep_us(HALF_US); rel(SDA); sleep_us(HALF_US); }
static void bit_out(bool b) { if (b) rel(SDA); else low(SDA); sleep_us(HALF_US); scl_high(); sleep_us(HALF_US); low(SCL); }
static bool bit_in(void) { rel(SDA); sleep_us(HALF_US); scl_high(); bool v = gpio_get(SDA); sleep_us(HALF_US); low(SCL); return v; }
static bool byte_out(uint8_t v) { for (int i = 7; i >= 0; i--) bit_out((v >> i) & 1); return !bit_in(); }
static uint8_t byte_in(bool ack) { uint8_t v = 0; for (int i = 0; i < 8; i++) v = (uint8_t)(v << 1 | bit_in()); bit_out(!ack); return v; }

bool ddc_read_edid(uint8_t block, uint8_t out[128]) {
    gpio_init(SDA); gpio_init(SCL);
    gpio_disable_pulls(SDA); gpio_disable_pulls(SCL);   // the board has pull-ups
    stuck = false;
    start();
    bool ok = byte_out(0xA0) && byte_out((uint8_t)(block * 128));
    if (ok) { start(); ok = byte_out(0xA1); }
    if (ok) for (int i = 0; i < 128; i++) out[i] = byte_in(i < 127);
    stop();
    rel(SDA); rel(SCL);
    return ok && !stuck;
}
