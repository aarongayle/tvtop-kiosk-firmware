// net_wifi.h over the ESP32 modem link: the same API the cyw43 build exposes, but the radio, the
// IP stack and the join retries all live on the other end of the UART.
//
// The modem is stateless as far as the kiosk is concerned: everything it knows was told to it over
// the link, so if it reboots (a crash, a brownout, or the console's `modem reset`) it comes back
// with nothing. This file therefore remembers what was asked for — station credentials, or the
// provisioning AP — and replays it whenever a fresh hello arrives. Nothing above here has to care.
#include <stdio.h>
#include <string.h>

#include "net_wifi.h"
#include "modem_link.h"
#include "modem_io.h"
#include "kiosk_config.h"

#include "pico/stdlib.h"
#include "pico/time.h"
#include "pico/unique_id.h"

// The bring-up rig has no usable status LED: the Pico 2 W's is on the cyw43, which this build
// never starts. The v3 board puts one on the RP2354A itself.
#ifdef KIOSK_LED_PIN
#include "hardware/gpio.h"
#endif

static bool initted;
static wifi_state_t state = WIFI_DOWN;
static uint32_t ip_addr;
static int8_t rssi;
static uint8_t channel;

static bool want_sta;                    // credentials given: replay on a modem restart
static char sta_ssid[33];
static char sta_pass[65];
static bool want_ap;
static char ap_ssid[16];
static bool was_ready;

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

static void unique_suffix(char out[5]) {
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    snprintf(out, 5, "%02X%02X", id.id[PICO_UNIQUE_BOARD_ID_SIZE_BYTES - 2], id.id[PICO_UNIQUE_BOARD_ID_SIZE_BYTES - 1]);
}

// ---- outgoing requests --------------------------------------------------------------------------

static void send_connect(void) {
    uint8_t buf[1 + 32 + 1 + 64];
    size_t n = 0;
    uint8_t sl = (uint8_t)strlen(sta_ssid), pl = (uint8_t)strlen(sta_pass);
    buf[n++] = sl;
    memcpy(buf + n, sta_ssid, sl); n += sl;
    buf[n++] = pl;
    memcpy(buf + n, sta_pass, pl); n += pl;
    modem_link_send(H_WIFI_CONNECT, buf, (uint16_t)n);
}

static void send_ap(void) {
    uint8_t buf[1 + 16];
    uint8_t sl = (uint8_t)strlen(ap_ssid);
    buf[0] = sl;
    memcpy(buf + 1, ap_ssid, sl);
    modem_link_send(H_AP_START, buf, (uint16_t)(1 + sl));
}

// ---- incoming messages --------------------------------------------------------------------------

static void on_wifi_state(const uint8_t *p, uint16_t len) {
    if (len < 7) return;
    wifi_state_t was = state;
    state = (wifi_state_t)p[0];
    memcpy(&ip_addr, p + 1, 4);
    rssi = (int8_t)p[5];
    channel = p[6];
    if (state == was) return;
    if (state == WIFI_UP) {
        char ip[16];
        printf("wifi: up, ip %s, channel %u, rssi %d dBm\n", net_wifi_ip(ip, sizeof ip), channel, rssi);
    } else if (state == WIFI_FAILED) {
        printf("wifi: join failed; the modem is retrying\n");
    }
}

static void on_scan_result(const uint8_t *p, uint16_t len) {
    if (len < 4) return;
    int16_t r;
    memcpy(&r, p, 2);
    bool open = p[2] != 0;
    uint8_t sl = p[3];
    if (4u + sl > len) return;
    provision_scan_result(p + 4, sl, r, open);
}

static void dispatch(uint8_t type, const uint8_t *p, uint16_t len) {
    switch (type) {
    case M_WIFI_STATE: on_wifi_state(p, len); return;
    case M_SCAN_RESULT: on_scan_result(p, len); return;
    case M_SCAN_DONE: provision_scan_done(); return;
    case M_HTTP_STATUS: case M_HTTP_HEADER: case M_HTTP_BODY: case M_HTTP_DONE:
        http_modem_on_message(type, p, len); return;
    case M_SOCK_OPEN: case M_SOCK_DATA: case M_SOCK_SENT: case M_SOCK_CLOSE:
        portal_modem_on_message(type, p, len); return;
    case M_PONG: return;
    default:
        printf("modem: unexpected message 0x%02x (%u bytes)\n", type, len);
        return;
    }
}

// ---- public API ----------------------------------------------------------------------------------

bool net_wifi_init(void) {
    if (initted) return true;
#ifdef KIOSK_LED_PIN
    gpio_init(KIOSK_LED_PIN);
    gpio_set_dir(KIOSK_LED_PIN, GPIO_OUT);
    gpio_put(KIOSK_LED_PIN, 0);
#endif
    bool ok = modem_link_init();
    modem_link_set_handler(dispatch);
    was_ready = modem_link_ready();
    initted = true;
    state = WIFI_DOWN;
    return ok;
}

void net_wifi_connect(const char *ssid, const char *password) {
    if (!initted || !ssid || !ssid[0]) return;
    snprintf(sta_ssid, sizeof sta_ssid, "%s", ssid);
    snprintf(sta_pass, sizeof sta_pass, "%s", password ? password : "");
    want_sta = true;
    want_ap = false;
    state = WIFI_CONNECTING;
    send_connect();
}

wifi_state_t net_wifi_state(void) { return state; }

const char *net_wifi_ip(char *buf, size_t cap) {
    if (state != WIFI_UP || !ip_addr) { if (cap) buf[0] = 0; return buf; }
    snprintf(buf, cap, "%lu.%lu.%lu.%lu", (unsigned long)(ip_addr & 0xff), (unsigned long)((ip_addr >> 8) & 0xff),
             (unsigned long)((ip_addr >> 16) & 0xff), (unsigned long)((ip_addr >> 24) & 0xff));
    return buf;
}

int net_wifi_rssi(void) { return rssi; }
int net_wifi_channel(void) { return channel; }

const char *net_wifi_start_ap(void) {
    char suffix[5];
    unique_suffix(suffix);
    snprintf(ap_ssid, sizeof ap_ssid, "TVTOP-%s", suffix);
    want_ap = true;
    want_sta = false;
    send_ap();
    state = WIFI_AP;
    return ap_ssid;
}

void net_wifi_stop_ap(void) {
    want_ap = false;
    modem_link_send(H_AP_STOP, NULL, 0);
    if (state == WIFI_AP) state = WIFI_DOWN;
}

void net_wifi_led(bool on) {
#ifdef KIOSK_LED_PIN
    gpio_put(KIOSK_LED_PIN, on);
#else
    (void)on;
#endif
}

// A cyw43-only knob (the Pico W's 3V3 regulator). There is no such rail to quieten here.
void net_wifi_smps_pwm(bool pwm) { (void)pwm; }

void net_wifi_poll(void) {
    if (!initted) return;
    modem_link_poll();

    bool ready = modem_link_ready();
    if (ready && !was_ready) {
        // A modem that has just (re)started knows nothing. Replay what we wanted, and let the
        // HTTP client and the portal relay abandon whatever they had in flight.
        printf("modem: link up (%s); restoring state\n", modem_link_fw());
        http_modem_on_modem_restart();
        portal_modem_on_modem_restart();
        if (want_sta) { state = WIFI_CONNECTING; send_connect(); }
        else if (want_ap) send_ap();
    } else if (!ready && was_ready) {
        state = WIFI_DOWN;
    }
    was_ready = ready;

    // A modem that stops answering is indistinguishable from one that has crashed. Ping every few
    // seconds while idle so the link's frame counters keep moving and a dead modem is visible in
    // `stats` rather than only as poll timeouts.
    static uint32_t next_ping;
    uint32_t now = now_ms();
    if (ready && (int32_t)(now - next_ping) >= 0) {
        next_ping = now + 5000u;
        modem_link_send(H_PING, NULL, 0);
    }
}
