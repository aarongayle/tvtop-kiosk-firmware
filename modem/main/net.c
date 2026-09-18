// Wi-Fi station and SoftAP, plus SNTP.
//
// The join retry lives here rather than on the kiosk: the RP2350 asks once and is told what
// happened, which keeps the protocol free of timers and means a modem restart cannot lose a
// reconnect that was already under way. Backoff matches the cyw43 build's (2 s doubling to 60 s)
// so the two backends behave the same from the kiosk's side.
//
// TLS needs a clock. There is no RTC here, so certificate validity dates would all fail against a
// 1970 system time; SNTP is started as soon as the station has an address and HTTPS waits for it.
#include "net.h"

#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "nvs_flash.h"

#include "link.h"

#define TAG "net"
#define BACKOFF_MIN_MS 2000u
#define BACKOFF_MAX_MS 60000u

static esp_netif_t *sta_netif, *ap_netif;
static modem_wifi_state_t state = MODEM_WIFI_DOWN;
static char want_ssid[33], want_pass[65];
static bool have_creds;
static uint32_t backoff_ms = BACKOFF_MIN_MS;
static TimerHandle_t retry_timer;
static bool ap_on;
static bool sntp_started;

bool net_sta_up(void) { return state == MODEM_WIFI_UP; }

void net_send_state(void) {
    uint8_t buf[7];
    esp_netif_ip_info_t ip = {0};
    if (state == MODEM_WIFI_UP && sta_netif) esp_netif_get_ip_info(sta_netif, &ip);
    wifi_ap_record_t ap;
    int8_t rssi = 0;
    uint8_t chan = 0;
    if (state == MODEM_WIFI_UP && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        rssi = ap.rssi;
        chan = ap.primary;
    }
    buf[0] = (uint8_t)state;
    memcpy(buf + 1, &ip.ip.addr, 4);
    buf[5] = (uint8_t)rssi;
    buf[6] = chan;
    link_send(M_WIFI_STATE, buf, sizeof buf);
}

static void set_state(modem_wifi_state_t s) {
    if (state == s && s != MODEM_WIFI_UP) return;
    state = s;
    net_send_state();
}

static void retry_cb(TimerHandle_t t) {
    (void)t;
    if (!have_creds) return;
    ESP_LOGI(TAG, "retrying join to \"%s\"", want_ssid);
    set_state(MODEM_WIFI_CONNECTING);
    esp_wifi_connect();
}

static void schedule_retry(void) {
    set_state(MODEM_WIFI_FAILED);
    if (!retry_timer) retry_timer = xTimerCreate("wifi_retry", pdMS_TO_TICKS(backoff_ms), pdFALSE, NULL, retry_cb);
    xTimerChangePeriod(retry_timer, pdMS_TO_TICKS(backoff_ms), 0);
    xTimerStart(retry_timer, 0);
    backoff_ms = backoff_ms * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : backoff_ms * 2;
}

static void start_sntp(void) {
    if (sntp_started) return;
    sntp_started = true;
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base;
    switch (id) {
    case WIFI_EVENT_STA_START:
        if (have_creds) esp_wifi_connect();
        break;
    case WIFI_EVENT_STA_DISCONNECTED: {
        wifi_event_sta_disconnected_t *d = data;
        if (!have_creds) { set_state(MODEM_WIFI_DOWN); break; }
        ESP_LOGW(TAG, "disconnected from \"%s\" (reason %d)", want_ssid, d->reason);
        schedule_retry();
        break;
    }
    case WIFI_EVENT_SCAN_DONE: {
        uint16_t n = 0;
        esp_wifi_scan_get_ap_num(&n);
        if (n > 32) n = 32;
        wifi_ap_record_t *recs = n ? calloc(n, sizeof *recs) : NULL;
        if (recs && esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK) {
            for (uint16_t i = 0; i < n; i++) {
                uint8_t buf[4 + 32];
                int16_t rssi = recs[i].rssi;
                uint8_t sl = (uint8_t)strnlen((char *)recs[i].ssid, 32);
                if (!sl) continue;                      // hidden network: nothing to show
                memcpy(buf, &rssi, 2);
                buf[2] = recs[i].authmode == WIFI_AUTH_OPEN;
                buf[3] = sl;
                memcpy(buf + 4, recs[i].ssid, sl);
                link_send(M_SCAN_RESULT, buf, (uint16_t)(4 + sl));
            }
        }
        free(recs);
        link_send(M_SCAN_DONE, NULL, 0);
        break;
    }
    default:
        break;
    }
}

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base;
    if (id != IP_EVENT_STA_GOT_IP) return;
    ip_event_got_ip_t *e = data;
    ESP_LOGI(TAG, "joined \"%s\", ip " IPSTR, want_ssid, IP2STR(&e->ip_info.ip));
    backoff_ms = BACKOFF_MIN_MS;
    start_sntp();
    set_state(MODEM_WIFI_UP);
}

void net_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip, NULL, NULL));
    // The kiosk is mains-powered and holds a long poll open: power saving only adds latency.
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
}

void net_connect(const char *ssid, const char *pass) {
    snprintf(want_ssid, sizeof want_ssid, "%s", ssid);
    snprintf(want_pass, sizeof want_pass, "%s", pass ? pass : "");
    have_creds = true;
    backoff_ms = BACKOFF_MIN_MS;
    if (retry_timer) xTimerStop(retry_timer, 0);
    if (ap_on) net_ap_stop();

    // esp_wifi's fields are fixed-size and need no terminator: a 32-character SSID fills ssid[]
    // exactly, which is why this is a memcpy and not an snprintf.
    wifi_config_t wc = {0};
    memcpy(wc.sta.ssid, want_ssid, strnlen(want_ssid, sizeof wc.sta.ssid));
    memcpy(wc.sta.password, want_pass, strnlen(want_pass, sizeof wc.sta.password));
    wc.sta.threshold.authmode = want_pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_LOGI(TAG, "joining \"%s\"", want_ssid);
    set_state(MODEM_WIFI_CONNECTING);
    esp_wifi_disconnect();
    esp_wifi_connect();
}

void net_stop(void) {
    have_creds = false;
    if (retry_timer) xTimerStop(retry_timer, 0);
    esp_wifi_disconnect();
    set_state(MODEM_WIFI_DOWN);
}

void net_ap_start(const char *ssid) {
    if (!ap_netif) ap_netif = esp_netif_create_default_wifi_ap();
    have_creds = false;
    if (retry_timer) xTimerStop(retry_timer, 0);

    wifi_config_t wc = {0};
    size_t n = strnlen(ssid, sizeof wc.ap.ssid);
    memcpy(wc.ap.ssid, ssid, n);
    wc.ap.ssid_len = (uint8_t)n;
    wc.ap.authmode = WIFI_AUTH_OPEN;
    wc.ap.max_connection = 4;
    wc.ap.channel = 1;
    // APSTA, not AP: the portal's network list comes from a scan, and scanning needs the station
    // interface to exist. esp-netif's DHCP server on the AP hands out 192.168.4.x by default.
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ap_on = true;
    ESP_LOGI(TAG, "provisioning AP \"%s\" up", ssid);
    set_state(MODEM_WIFI_AP);
}

void net_ap_stop(void) {
    if (!ap_on) return;
    ap_on = false;
    esp_wifi_set_mode(WIFI_MODE_STA);
    set_state(have_creds ? MODEM_WIFI_CONNECTING : MODEM_WIFI_DOWN);
}

void net_scan(void) {
    wifi_scan_config_t sc = {0};
    sc.show_hidden = false;
    esp_err_t err = esp_wifi_scan_start(&sc, false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan start failed: %s", esp_err_to_name(err));
        link_send(M_SCAN_DONE, NULL, 0);
    }
}
