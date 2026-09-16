// PiCowBell HSTX DVI connection test for a Pico W (RP2040). No video, no Wi-Fi.
//
// Once a second on the USB serial console:
//
// 1. VSYS voltage. On the PiCowBell, VSYS feeds the HDMI connector's +5V pin (the sink powers its
//    EDID EEPROM and hot-plug detect from it). On the Pico W, VSYS/3 is on ADC3 (GPIO29), which
//    only connects to the divider while GPIO25 (the wireless chip select) is held high.
// 2. DDC probe, bit-banged. GPIO4 (SDA) / GPIO5 (SCL) go through BSS138 level shifters to the
//    HDMI connector's DDC pins. Every HDMI/DVI sink answers at 0x50 with a 128-byte EDID. The bus
//    is driven open-drain by hand at ~500 Hz so each step is observable: an ACK from 0x50 proves
//    Pico -> joints -> PiCowBell -> socket -> cable -> monitor. The RP2040's own I2C block
//    reported a timeout with and without a cable, so it is not used.
// 3. PiCowBell pull-ups on GPIO4/5 (10k to 3V3 on the board) against the Pico's pull-down.
// 4. TMDS termination on GPIO12-19: a sink holds each line at 3.3 V through 50 ohms, which beats
//    the Pico's weak pull-down (the board has 220 ohm series resistors and no coupling caps).
//
// Nothing drives a TMDS pin low, so a connected monitor's termination never sinks current.
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/adc.h"

#define SDA 4
#define SCL 5
#define HALF_US 1000

static const struct { uint8_t gpio; const char *name; } pins[] = {
    { 12, "D0+ (blue)" }, { 13, "D0- (blue)" },
    { 14, "CK+"        }, { 15, "CK-"        },
    { 16, "D2+ (red)"  }, { 17, "D2- (red)"  },
    { 18, "D1+ (green)"}, { 19, "D1- (green)"},
};
#define NPINS (sizeof pins / sizeof pins[0])

// ---- open-drain bit-bang I2C: "high" = release (board pull-up), "low" = drive low ----
static void rel(uint g) { gpio_set_dir(g, GPIO_IN); }
static void low(uint g) { gpio_put(g, 0); gpio_set_dir(g, GPIO_OUT); }
static bool scl_high_wait(void) {   // release SCL and wait for it to rise (clock stretching)
    rel(SCL);
    for (int i = 0; i < 100; i++) { if (gpio_get(SCL)) return true; sleep_us(100); }
    return false;
}
static int stuck;   // set when SCL never rises
static void i2c_start(void) { rel(SDA); if (!scl_high_wait()) stuck = 1; sleep_us(HALF_US); low(SDA); sleep_us(HALF_US); low(SCL); sleep_us(HALF_US); }
static void i2c_stop(void) { low(SDA); sleep_us(HALF_US); if (!scl_high_wait()) stuck = 1; sleep_us(HALF_US); rel(SDA); sleep_us(HALF_US); }
static bool i2c_bit_write(bool b) {
    if (b) rel(SDA); else low(SDA);
    sleep_us(HALF_US);
    if (!scl_high_wait()) stuck = 1;
    sleep_us(HALF_US);
    low(SCL);
    return true;
}
static bool i2c_bit_read(void) {
    rel(SDA);
    sleep_us(HALF_US);
    if (!scl_high_wait()) stuck = 1;
    bool v = gpio_get(SDA);
    sleep_us(HALF_US);
    low(SCL);
    return v;
}
static bool i2c_byte_write(uint8_t v) {   // returns true on ACK
    for (int i = 7; i >= 0; i--) i2c_bit_write((v >> i) & 1);
    return !i2c_bit_read();
}
static uint8_t i2c_byte_read(bool ack) {
    uint8_t v = 0;
    for (int i = 0; i < 8; i++) v = (uint8_t)(v << 1 | i2c_bit_read());
    i2c_bit_write(!ack);
    return v;
}

static void print_edid(const uint8_t *e) {
    static const uint8_t hdr[8] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
    uint8_t sum = 0;
    for (int i = 0; i < 128; i++) sum += e[i];
    char mfg[4] = { (char)('A' - 1 + ((e[8] >> 2) & 0x1F)), (char)('A' - 1 + (((e[8] & 3) << 3) | (e[9] >> 5))), (char)('A' - 1 + (e[9] & 0x1F)), 0 };
    printf("EDID: header %s, checksum %s, manufacturer %s\n", memcmp(e, hdr, 8) == 0 ? "OK" : "BAD", sum == 0 ? "OK" : "BAD", mfg);
    for (int d = 54; d <= 108; d += 18) {
        if (e[d] == 0 && e[d + 1] == 0 && e[d + 3] == 0xFC) {
            char name[14] = {0};
            for (int i = 0; i < 13 && e[d + 5 + i] != 0x0A; i++) name[i] = (char)e[d + 5 + i];
            printf("EDID: monitor name \"%s\"\n", name);
        } else if (e[d] == 0 && e[d + 1] == 0 && e[d + 3] == 0xFD) {
            printf("EDID: range limits: vertical %u-%u Hz, horizontal %u-%u kHz\n", e[d + 5], e[d + 6], e[d + 7], e[d + 8]);
        }
    }
}

static bool read_with(uint gpio, bool pull_up) {
    gpio_set_dir(gpio, GPIO_IN);
    if (pull_up) gpio_pull_up(gpio); else gpio_pull_down(gpio);
    sleep_ms(5);
    return gpio_get(gpio);
}

int main(void) {
    stdio_init_all();
    for (unsigned i = 0; i < NPINS; i++) gpio_init(pins[i].gpio);
    gpio_init(SDA); gpio_init(SCL);
    gpio_init(25); gpio_set_dir(25, GPIO_OUT);
    adc_init(); adc_gpio_init(29);
    for (unsigned pass = 1;; pass++) {
        printf("\n==== PiCowBell connection test v3, pass %u ====\n", pass);

        gpio_put(25, 1); sleep_ms(2);
        adc_select_input(3);
        uint32_t raw = 0;
        for (int i = 0; i < 16; i++) raw += adc_read();
        gpio_put(25, 0);
        unsigned mv = (unsigned)((raw / 16) * 3300u * 3u / 4096u);
        printf("VSYS (feeds HDMI +5V): %u.%02u V (%s)\n", mv / 1000, (mv % 1000) / 10, mv > 4300 ? "OK" : "LOW - the monitor may not see a source");

        gpio_disable_pulls(SDA); gpio_disable_pulls(SCL);
        gpio_pull_down(SDA); gpio_pull_down(SCL); sleep_ms(20);
        bool sda_pu = gpio_get(SDA), scl_pu = gpio_get(SCL);
        gpio_disable_pulls(SDA); gpio_disable_pulls(SCL);
        printf("PiCowBell pull-ups on GPIO4/5: SDA %s, SCL %s\n", sda_pu ? "present" : "MISSING", scl_pu ? "present" : "MISSING");

        stuck = 0;
        i2c_start();
        bool ack_w = i2c_byte_write(0xA0);       // 0x50, write
        bool ack_off = ack_w && i2c_byte_write(0x00);
        uint8_t edid[128];
        bool got = false;
        if (ack_off) {
            i2c_start();                          // repeated start
            if (i2c_byte_write(0xA1)) {           // 0x50, read
                for (int i = 0; i < 128; i++) edid[i] = i2c_byte_read(i < 127);
                got = true;
            }
        }
        i2c_stop();
        rel(SDA); rel(SCL);
        if (stuck) printf("DDC: SCL never went high - the clock line is held low somewhere\n");
        printf("DDC probe at 0x50: %s\n", got ? "monitor ANSWERED - socket, cable and monitor are connected" : ack_w ? "address acknowledged but read failed" : "no acknowledge - no monitor seen on DDC");
        if (got) {
            print_edid(edid);
            printf("EDIDHEX0 ");
            for (int i = 0; i < 128; i++) printf("%02x", edid[i]);
            printf("\n");
            if (edid[126] > 0) {
                uint8_t ext[128];
                stuck = 0;
                i2c_start();
                bool ok = i2c_byte_write(0xA0) && i2c_byte_write(0x80);
                if (ok) { i2c_start(); ok = i2c_byte_write(0xA1); }
                if (ok) for (int i = 0; i < 128; i++) ext[i] = i2c_byte_read(i < 127);
                i2c_stop(); rel(SDA); rel(SCL);
                if (ok) { printf("EDIDHEX1 "); for (int i = 0; i < 128; i++) printf("%02x", ext[i]); printf("\n"); }
                else printf("EDID extension block: read failed\n");
            }
        }

        int reach = 0;
        printf("TMDS termination (pull-down reads, GPIO 12-19): ");
        for (unsigned i = 0; i < NPINS; i++) {
            bool pd = read_with(pins[i].gpio, false);
            if (pd) reach++;
            printf("%u:%s ", pins[i].gpio, pd ? "HIGH" : "low");
            gpio_disable_pulls(pins[i].gpio);
        }
        printf("\nsummary: vsys=%umV pullups=%s ddc=%s tmds_terminated=%d/8\n", mv, sda_pu && scl_pu ? "yes" : "no", got ? "answered" : ack_w ? "ack-only" : "none", reach);
        sleep_ms(1000);
    }
}
