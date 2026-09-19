// cyw43 bring-up, station mode and the provisioning access point.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum { WIFI_DOWN, WIFI_CONNECTING, WIFI_UP, WIFI_FAILED, WIFI_AP } wifi_state_t;

// Initialises the cyw43 arch (threadsafe background). Must be called after scanout_setup_clocks:
// it derives the PIO SPI clock divider from the actual sys clock. Returns false on failure.
bool net_wifi_init(void);
void net_wifi_connect(const char *ssid, const char *password);   // async; poll net_wifi_state
wifi_state_t net_wifi_state(void);
const char *net_wifi_ip(char *buf, size_t cap);   // dotted quad or "" when not up
int net_wifi_rssi(void);
int net_wifi_channel(void);
// SoftAP for provisioning (open network). Returns the SSID used ("TVTOP-xxxx", from the unique id).
// keep_station asks the radio to go on trying the station credentials while the AP is up, so a
// kiosk that has lost its network offers a way in without giving up on finding it again. Only the
// modem backend can do both at once; see net_wifi_ap_is_concurrent.
const char *net_wifi_start_ap(bool keep_station);
// True when the AP and a station join can run together. The ESP modem manages it; the cyw43 build
// drops the station while its AP is up (a join in flight keeps the radio hopping channels), so the
// caller has to alternate instead of relying on both.
bool net_wifi_ap_is_concurrent(void);
void net_wifi_stop_ap(void);
void net_wifi_led(bool on);
// Pico W 3V3 regulator mode: true forces PWM (quieter rail), false restores power-save.
void net_wifi_smps_pwm(bool pwm);
// --------------- Is this network actually online? ---------------
//
// A kiosk can join a hotel or airport network perfectly and still reach nothing, because a portal
// wants someone to click "I agree" in a browser — and the kiosk has no browser, and the
// authorisation is per-MAC so a phone cannot do it on its behalf. The kiosk's own polling cannot
// tell that apart from the server being down: those requests are HTTPS, and a portal cannot
// intercept TLS, so all it can do is break the handshake. Only a plain-HTTP probe distinguishes
// them, and only the radio's side of the link can run one.
typedef enum {
    NET_CHECK_IDLE = 0,       // nothing asked for
    NET_CHECK_PENDING,
    NET_CHECK_ONLINE,
    NET_CHECK_CAPTIVE,        // something is answering on the network's behalf: a sign-in page
    NET_CHECK_NO_DNS,
    NET_CHECK_NO_ROUTE,
    NET_CHECK_UNSUPPORTED,    // this backend cannot probe (cyw43); callers keep their old behaviour
} net_check_t;

void net_wifi_check_start(void);
net_check_t net_wifi_check_result(void);

// Call from the main loop (drives reconnect attempts / state transitions).
void net_wifi_poll(void);
