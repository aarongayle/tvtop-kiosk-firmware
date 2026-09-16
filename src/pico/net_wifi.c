// cyw43 bring-up, station mode with reconnect/backoff, and the provisioning SoftAP.
//
// Threading: pico_cyw43_arch_lwip_threadsafe_background. The cyw43 driver API (connect, scan,
// rssi, link status) takes its own lock internally; only direct lwIP calls (netif_*, and the
// vendored DHCP/DNS servers, which create UDP pcbs) must be bracketed with
// cyw43_arch_lwip_begin/end. Everything in this file runs in main context on core 0.
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "net_wifi.h"

#include "pico/cyw43_arch.h"
#include "pico/cyw43_driver.h"
#include "pico/time.h"
#include "pico/unique_id.h"
#include "hardware/clocks.h"
#include "lwip/ip_addr.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"

#include "dhcpserver.h"
#include "dnsserver.h"

// A join that sits in JOIN/NOIP this long without the driver reporting failure is treated as
// failed and re-issued (seen with APs that accept the association but never DHCP-answer).
#define JOIN_TIMEOUT_MS 30000u
#define BACKOFF_MIN_S 2u
#define BACKOFF_MAX_S 60u

static bool initted;
static wifi_state_t state = WIFI_DOWN;
static char sta_ssid[33];
static char sta_pass[65];
static bool have_creds;
static uint32_t join_started_ms;
static uint32_t retry_at_ms;
static uint32_t backoff_s = BACKOFF_MIN_S;

static bool ap_on;
static char ap_ssid[16];
static char hostname[24];
static dhcp_server_t dhcp;
static dns_server_t dns;

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

static void unique_suffix(char out[5]) {
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    // The last two bytes of the 64-bit flash id are the ones that differ between boards from the
    // same batch.
    snprintf(out, 5, "%02X%02X", id.id[PICO_UNIQUE_BOARD_ID_SIZE_BYTES - 2], id.id[PICO_UNIQUE_BOARD_ID_SIZE_BYTES - 1]);
}

bool net_wifi_init(void) {
    if (initted) return true;

    // The cyw43 SPI is bit-banged by a PIO program that spends 2 PIO cycles per bit, so the SPI
    // clock is clk_sys / (2 * div). A stock 125 MHz Pico W uses div 2 → 31.25 MHz; the chip is
    // not specified above that. We run clk_sys at up to 372 MHz (the TMDS bit clock), so pick the
    // smallest integer divider that keeps the SPI at or below 31.25 MHz: ceil(clk_sys / 62.5 MHz).
    uint32_t hz = clock_get_hz(clk_sys);
    // Keep the PIO clock at the stock ~62.5 MHz. The SDK's default SPI program (spi_gap01_sample0)
    // samples for that speed: halving the divider's output made the chip fail to start at all.
    uint32_t div = (hz + 62500000u - 1u) / 62500000u;
    if (div < 2) div = 2;
    cyw43_set_pio_clkdiv_int_frac8(div, 0);

    if (cyw43_arch_init_with_country(CYW43_COUNTRY_WORLDWIDE) != 0) {
        printf("wifi: cyw43 init failed\n");
        return false;
    }
    initted = true;

    char suffix[5];
    unique_suffix(suffix);
    snprintf(ap_ssid, sizeof ap_ssid, "TVTOP-%s", suffix);
    for (char *c = suffix; *c; c++) *c = (char)(*c >= 'A' && *c <= 'F' ? *c + 32 : *c);
    snprintf(hostname, sizeof hostname, "tvtop-kiosk-%s", suffix);

    // enable_sta_mode does netif_add and then stamps CYW43_HOST_NAME on the netif, so our name
    // must go on afterwards. That is early enough: DHCP only sends DISCOVER once the link is up,
    // and lwIP reads the hostname pointer when it builds each message.
    cyw43_arch_enable_sta_mode();
#if LWIP_NETIF_HOSTNAME
    cyw43_arch_lwip_begin();
    netif_set_hostname(&cyw43_state.netif[CYW43_ITF_STA], hostname);
    cyw43_arch_lwip_end();
#endif
    // Mains-powered device holding a long-poll open: radio power saving only adds latency and
    // the occasional dropped beacon-interval frame.
    cyw43_wifi_pm(&cyw43_state, CYW43_NONE_PM);

    state = WIFI_DOWN;
    return true;
}

static void schedule_retry(const char *why) {
    printf("wifi: %s; retry in %u s\n", why, (unsigned)backoff_s);
    state = WIFI_FAILED;
    retry_at_ms = now_ms() + backoff_s * 1000u;
    backoff_s = backoff_s * 2 > BACKOFF_MAX_S ? BACKOFF_MAX_S : backoff_s * 2;
}

static void start_join(void) {
    uint32_t auth = sta_pass[0] ? CYW43_AUTH_WPA2_AES_PSK : CYW43_AUTH_OPEN;
    int err = cyw43_arch_wifi_connect_async(sta_ssid, sta_pass[0] ? sta_pass : NULL, auth);
    join_started_ms = now_ms();
    if (err != 0) {
        schedule_retry("connect_async failed");
        return;
    }
    state = WIFI_CONNECTING;
}

void net_wifi_connect(const char *ssid, const char *password) {
    if (!initted || !ssid || !ssid[0]) return;
    size_t n = strlen(ssid);
    if (n >= sizeof sta_ssid) n = sizeof sta_ssid - 1;
    memcpy(sta_ssid, ssid, n);
    sta_ssid[n] = 0;
    n = password ? strlen(password) : 0;
    if (n >= sizeof sta_pass) n = sizeof sta_pass - 1;
    if (n) memcpy(sta_pass, password, n);
    sta_pass[n] = 0;
    have_creds = true;
    backoff_s = BACKOFF_MIN_S;
    if (ap_on) return;   // joined later, when the AP is torn down
    start_join();
}

wifi_state_t net_wifi_state(void) { return state; }

void net_wifi_poll(void) {
    if (!initted || ap_on) return;
    uint32_t now = now_ms();
    switch (state) {
    case WIFI_CONNECTING: {
        int st = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        if (st == CYW43_LINK_UP) {
            state = WIFI_UP;
            backoff_s = BACKOFF_MIN_S;
            char ip[16];
            printf("wifi: up, ip %s\n", net_wifi_ip(ip, sizeof ip));
            net_wifi_led(true);
        } else if (st == CYW43_LINK_BADAUTH) {
            schedule_retry("bad password");
        } else if (st == CYW43_LINK_NONET) {
            schedule_retry("network not found");
        } else if (st < 0) {
            schedule_retry("join failed");
        } else if ((uint32_t)(now - join_started_ms) > JOIN_TIMEOUT_MS) {
            schedule_retry("join timed out");
        }
        break;
    }
    case WIFI_FAILED:
        if (have_creds && (int32_t)(now - retry_at_ms) >= 0) start_join();
        break;
    case WIFI_UP: {
        int st = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        if (st != CYW43_LINK_UP) {
            net_wifi_led(false);
            backoff_s = BACKOFF_MIN_S;
            schedule_retry("link lost");
        }
        break;
    }
    case WIFI_DOWN:
    case WIFI_AP:
    default:
        break;
    }
}

const char *net_wifi_ip(char *buf, size_t cap) {
    if (cap == 0) return "";
    buf[0] = 0;
    if (!initted || state != WIFI_UP) return buf;
    cyw43_arch_lwip_begin();
    ip4addr_ntoa_r(netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA]), buf, (int)cap);
    cyw43_arch_lwip_end();
    return buf;
}

// The 2.4 GHz channel the station is on (1-14), or 0 if unknown. Channel spacing decides which DVI
// clock harmonics land inside it.
int net_wifi_channel(void) {
    uint8_t buf[12] = {0};   // channel_info_t: hw_channel, target_channel, scan_channel (LE u32)
    if (cyw43_ioctl(&cyw43_state, CYW43_IOCTL_GET_CHANNEL, sizeof buf, buf, CYW43_ITF_STA) != 0) return 0;
    return (int)(buf[0] | buf[1] << 8);
}

int net_wifi_rssi(void) {
    if (!initted || state != WIFI_UP) return 0;
    int32_t rssi = 0;
    if (cyw43_wifi_get_rssi(&cyw43_state, &rssi) != 0) return 0;
    return (int)rssi;
}

const char *net_wifi_start_ap(void) {
    if (!initted) return "";
    if (ap_on) return ap_ssid;
    // A station join in flight would keep the radio hopping channels; drop it. The STA interface
    // itself stays enabled so the portal's network scan (which runs on the STA path) works.
    if (state == WIFI_CONNECTING || state == WIFI_UP) cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
    net_wifi_led(false);

    cyw43_arch_enable_ap_mode(ap_ssid, NULL, CYW43_AUTH_OPEN);

    ip_addr_t gw, mask;
    IP_ADDR4(&gw, 192, 168, 4, 1);
    IP_ADDR4(&mask, 255, 255, 255, 0);
    cyw43_arch_lwip_begin();
    dhcp_server_init(&dhcp, &cyw43_state.netif[CYW43_ITF_AP], &gw, &mask);
    // Answers every name with 192.168.4.1, which is what makes phones pop the captive portal.
    dns_server_init(&dns, &cyw43_state.netif[CYW43_ITF_AP], &gw);
    cyw43_arch_lwip_end();

    ap_on = true;
    state = WIFI_AP;
    printf("wifi: AP %s up at 192.168.4.1\n", ap_ssid);
    return ap_ssid;
}

void net_wifi_stop_ap(void) {
    if (!initted || !ap_on) return;
    cyw43_arch_lwip_begin();
    dns_server_deinit(&dns);
    dhcp_server_deinit(&dhcp);
    cyw43_arch_lwip_end();
    cyw43_arch_disable_ap_mode();
    // Removing the AP netif clears lwIP's default netif (netif_remove does that when the removed
    // one was the default); the station must become the route again.
    cyw43_arch_lwip_begin();
    netif_set_default(&cyw43_state.netif[CYW43_ITF_STA]);
    cyw43_arch_lwip_end();
    ap_on = false;
    state = WIFI_DOWN;
    if (have_creds) {
        backoff_s = BACKOFF_MIN_S;
        start_join();
    }
}

void net_wifi_led(bool on) {
    if (!initted) return;
#ifdef CYW43_WL_GPIO_LED_PIN
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, on);
#else
    (void)on;
#endif
}

void net_wifi_smps_pwm(bool pwm) {
    if (!initted) return;
    cyw43_arch_gpio_put(CYW43_WL_GPIO_SMPS_PIN, pwm);
}
