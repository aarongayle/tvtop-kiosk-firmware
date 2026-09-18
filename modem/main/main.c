// TV-Top Kiosk modem firmware.
//
// The RP2350 does video and nothing else; this chip is its network card. Everything it does is in
// answer to a message on the UART (src/common/modem_proto.h): join this network, run the
// provisioning AP, fetch this URL, write these bytes to that portal connection. It keeps no
// settings of its own — no NVS credentials, no server URL — so a modem that reboots comes back
// blank and the kiosk simply tells it again.
//
// The one thing it owns that the RP2350 cannot is TLS: an RP2040 could never afford mbedTLS beside
// a framebuffer, and on v3 there is no second radio to fall back to. HTTPS to the kiosk server
// terminates here.
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "httpc.h"
#include "link.h"
#include "net.h"
#include "portal.h"

#define TAG "modem"

static void on_message(uint8_t type, const uint8_t *p, uint16_t len) {
    switch (type) {
    case H_HELLO:
        // The host has (re)started. Tell it who we are and what the radio is doing, so it does not
        // have to wait for the next state change to find out.
        link_send_hello();
        net_send_state();
        return;

    case H_PING:
        link_send(M_PONG, NULL, 0);
        return;

    case H_WIFI_CONNECT: {
        if (!len) return;
        uint8_t sl = p[0];
        if (1u + sl + 1u > len) return;
        uint8_t pl = p[1 + sl];
        if (2u + sl + pl > len) return;
        char ssid[33], pass[65];
        if (sl > 32 || pl > 64) return;
        memcpy(ssid, p + 1, sl); ssid[sl] = 0;
        memcpy(pass, p + 2 + sl, pl); pass[pl] = 0;
        portal_enable(false);
        net_connect(ssid, pass);
        return;
    }

    case H_WIFI_STOP:
        net_stop();
        return;

    case H_AP_START: {
        if (!len) return;
        uint8_t sl = p[0];
        if (1u + sl > len || sl > 31) return;
        char ssid[32];
        memcpy(ssid, p + 1, sl); ssid[sl] = 0;
        net_ap_start(ssid);
        portal_enable(true);
        return;
    }

    case H_AP_STOP:
        portal_enable(false);
        net_ap_stop();
        return;

    case H_SCAN:
        net_scan();
        return;

    case H_HTTP_GET: {
        if (len < 11) return;
        uint8_t id = p[0];
        uint32_t timeout, credit;
        uint16_t ul;
        memcpy(&timeout, p + 1, 4);
        memcpy(&credit, p + 5, 4);
        memcpy(&ul, p + 9, 2);
        if (11u + ul > len) return;
        httpc_get(id, timeout, credit, (const char *)p + 11, ul);
        return;
    }

    case H_HTTP_CREDIT: {
        if (len < 5) return;
        uint32_t extra;
        memcpy(&extra, p + 1, 4);
        httpc_credit(p[0], extra);
        return;
    }

    case H_HTTP_CANCEL:
        httpc_cancel(p[0]);
        return;

    case H_SOCK_DATA:
        if (len < 2) return;
        portal_write(p[0], p + 1, (uint16_t)(len - 1));
        return;

    case H_SOCK_CLOSE:
        if (len < 2) return;
        portal_close(p[0], p[1] != 0);
        return;

    case H_LED:
        return;   // v3 puts the status LED on the RP2354A; nothing to do here

    default:
        ESP_LOGW(TAG, "unknown message 0x%02x (%u bytes)", type, len);
        return;
    }
}

void app_main(void) {
    link_init(on_message);
    link_log_redirect();
    ESP_LOGI(TAG, "TV-Top Kiosk modem " MODEM_FW_VERSION " starting");

    net_init();
    httpc_init();
    portal_init();

    // The kiosk may already be up and waiting (it holds the modem in reset only briefly at boot),
    // or it may be a few seconds behind us. Announce ourselves a handful of times; after that a
    // host that arrives late says H_HELLO and gets an answer.
    for (int i = 0; i < 10; i++) {
        link_send_hello();
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    vTaskDelete(NULL);
}
