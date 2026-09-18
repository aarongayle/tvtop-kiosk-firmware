// One HTTP/1.1 request at a time, on its own task because esp_http_client is blocking.
//
// The body is only read when the host has credit outstanding. That is what keeps a 300 KB static
// set from drowning an RP2350 that has stopped draining the UART to render a frame: the modem
// simply stops reading the socket, TCP's own window closes, and the server waits. No buffering
// here, no dropped bytes there.
//
// The socket timeout is deliberately short (SOCKET_SLICE_MS) and the real deadline is counted in
// this loop, so a cancel or a credit top-up is noticed within a few seconds even in the middle of
// the kiosk server's 25-second long poll, where the connection is legitimately silent.
#include "httpc.h"

#include <errno.h>
#include <string.h>
#include <time.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "link.h"
#include "net.h"

#define TAG "http"
#define SOCKET_SLICE_MS 3000
#define CONNECT_TIMEOUT_MS 10000
#define MAX_REDIRECTS 3
#define BODY_CHUNK (MODEM_MAX_PAYLOAD - 1)
#define URL_MAX 256

typedef struct {
    uint8_t id;
    uint32_t timeout_ms;
    uint32_t credit;
    char url[URL_MAX];
} req_t;

static QueueHandle_t req_q;
static SemaphoreHandle_t credit_sem;      // given whenever credit grows or a cancel lands
static volatile uint32_t credit;
static volatile uint8_t active_id;
static volatile bool active;
static volatile bool cancelled;

static void done(uint8_t id, int16_t err, uint16_t status) {
    uint8_t buf[5];
    buf[0] = id;
    memcpy(buf + 1, &err, 2);
    memcpy(buf + 3, &status, 2);
    link_send(M_HTTP_DONE, buf, sizeof buf);
}

// mbedTLS checks certificate validity dates, and this chip boots at 1970. SNTP is started as soon
// as the station gets an address; give it a moment before the first HTTPS request rather than
// failing every one of them with a date error.
static bool wait_for_clock(uint32_t budget_ms) {
    for (uint32_t waited = 0; waited < budget_ms; waited += 200) {
        if (time(NULL) > 1700000000) return true;
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    return time(NULL) > 1700000000;
}

static int16_t run(const req_t *r, uint16_t *out_status) {
    bool https = strncmp(r->url, "https://", 8) == 0;
    if (https && !wait_for_clock(10000)) {
        ESP_LOGW(TAG, "no NTP time yet; TLS certificate dates cannot be checked");
        return MODEM_HTTP_ERR_TLS;
    }

    esp_http_client_config_t cfg = {
        .url = r->url,
        .method = HTTP_METHOD_GET,
        // Generous for DNS + TCP + the TLS handshake; dropped to SOCKET_SLICE_MS once the socket
        // is open so that reads come back often enough to notice a cancel.
        .timeout_ms = CONNECT_TIMEOUT_MS,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
        .disable_auto_redirect = true,     // followed here, so each hop is visible in the log
        .keep_alive_enable = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return MODEM_HTTP_ERR_URL;

    int16_t err = MODEM_HTTP_OK;
    uint16_t status = 0;
    uint32_t start = xTaskGetTickCount() * portTICK_PERIOD_MS;
    uint32_t deadline = start + r->timeout_ms;

    int redirects = 0;
    for (;;) {
        esp_err_t e = esp_http_client_open(c, 0);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "connect failed: %s", esp_err_to_name(e));
            err = e == ESP_ERR_HTTP_CONNECT ? MODEM_HTTP_ERR_CONNECT : MODEM_HTTP_ERR_DNS;
            goto out;
        }
        esp_http_client_set_timeout_ms(c, SOCKET_SLICE_MS);
        // The response head is the part that keeps you waiting: the kiosk server parks a long poll
        // for 25 seconds and sends nothing at all until it has a frame — not even headers. A slice
        // timeout here is therefore the normal idle case, and esp_http_client_fetch_headers reports
        // it as -ESP_ERR_HTTP_EAGAIN, which is not a failure. Only the overall deadline ends it.
        for (;;) {
            int64_t cl = esp_http_client_fetch_headers(c);
            if (cl >= 0) break;
            if (cl != -ESP_ERR_HTTP_EAGAIN) { err = MODEM_HTTP_ERR_PROTOCOL; goto out; }
            if (cancelled) { err = MODEM_HTTP_ERR_ABORTED; goto out; }
            uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
            if ((int32_t)(now - deadline) >= 0) { err = MODEM_HTTP_ERR_TIMEOUT; goto out; }
        }
        status = (uint16_t)esp_http_client_get_status_code(c);
        bool redirect = status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
        if (!redirect) break;
        if (++redirects > MAX_REDIRECTS) { err = MODEM_HTTP_ERR_TOO_MANY_REDIRECTS; goto out; }
        if (esp_http_client_set_redirection(c) != ESP_OK) { err = MODEM_HTTP_ERR_PROTOCOL; goto out; }
        esp_http_client_close(c);
        esp_http_client_set_timeout_ms(c, CONNECT_TIMEOUT_MS);
    }

    {
        uint8_t sbuf[3];
        sbuf[0] = r->id;
        memcpy(sbuf + 1, &status, 2);
        link_send(M_HTTP_STATUS, sbuf, sizeof sbuf);
    }

    // Read first, ask whether the response is complete afterwards. esp_http_client_fetch_headers()
    // reads whole segments, so for a small reply the body is already in the client's buffer *and*
    // already counted against content-length: a loop that tests
    // esp_http_client_is_complete_data_received() before its first read believes the body has been
    // handled and never fetches those buffered bytes. That is exactly what a registration reply is
    // — one 147-byte segment — and it arrived as a 200 with no body at all.
    static uint8_t chunk[1 + BODY_CHUNK];
    chunk[0] = r->id;
    for (;;) {
        if (cancelled) { err = MODEM_HTTP_ERR_ABORTED; goto out; }
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if ((int32_t)(now - deadline) >= 0) { err = MODEM_HTTP_ERR_TIMEOUT; goto out; }

        if (credit == 0) {
            // Nothing may be read until the host says it can take more. Waking on the semaphore
            // rather than polling keeps this task off the CPU while the kiosk renders.
            xSemaphoreTake(credit_sem, pdMS_TO_TICKS(200));
            continue;
        }
        uint32_t want = credit < BODY_CHUNK ? credit : BODY_CHUNK;
        int n = esp_http_client_read(c, (char *)chunk + 1, (int)want);
        if (n > 0) {
            credit -= (uint32_t)n;
            if (!link_send(M_HTTP_BODY, chunk, (uint16_t)(1 + n))) { err = MODEM_HTTP_ERR_ABORTED; goto out; }
            if (esp_http_client_is_complete_data_received(c)) break;
            continue;
        }
        if (esp_http_client_is_complete_data_received(c)) break;   // a 304 and friends: no body

        // The documented signal for "the slice expired before any data was ready", which is the
        // normal state of a parked long poll. The overall deadline above is what ends it.
        if (n == -ESP_ERR_HTTP_EAGAIN) continue;
        int se = esp_http_client_get_errno(c);
        if (se == EAGAIN || se == EWOULDBLOCK) continue;
        if (n == 0) {
            // A short read that is not a timeout means the peer closed before content-length was
            // satisfied. errno is not always conclusive here, so pause briefly rather than spin;
            // the deadline still bounds it.
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        ESP_LOGW(TAG, "read failed (errno %d)", se);
        err = MODEM_HTTP_ERR_CLOSED;
        goto out;
    }

out:
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    *out_status = status;
    return err;
}

static void http_task(void *arg) {
    (void)arg;
    req_t r;
    for (;;) {
        if (xQueueReceive(req_q, &r, portMAX_DELAY) != pdTRUE) continue;
        active_id = r.id;
        credit = r.credit;
        cancelled = false;
        active = true;
        uint16_t status = 0;
        int16_t err = run(&r, &status);
        active = false;
        done(r.id, err, status);
    }
}

void httpc_init(void) {
    req_q = xQueueCreate(1, sizeof(req_t));
    credit_sem = xSemaphoreCreateBinary();
    // 6 KB: mbedTLS itself allocates from the heap, but the TLS handshake runs on this stack.
    xTaskCreate(http_task, "http", 6144, NULL, 6, NULL);
}

void httpc_get(uint8_t id, uint32_t timeout_ms, uint32_t credit_init, const char *url, uint16_t url_len) {
    req_t r;
    if (url_len >= sizeof r.url) { done(id, MODEM_HTTP_ERR_URL, 0); return; }
    memcpy(r.url, url, url_len);
    r.url[url_len] = 0;
    r.id = id;
    r.timeout_ms = timeout_ms;
    r.credit = credit_init;
    if (!net_sta_up()) { done(id, MODEM_HTTP_ERR_CONNECT, 0); return; }
    // The host only ever has one request outstanding; a second means it gave up on the first and
    // this one has to wait for the socket, which it should not do silently.
    if (xQueueSend(req_q, &r, 0) != pdTRUE) done(id, MODEM_HTTP_ERR_BUSY, 0);
}

void httpc_credit(uint8_t id, uint32_t extra) {
    if (!active || id != active_id) return;
    credit += extra;
    xSemaphoreGive(credit_sem);
}

void httpc_cancel(uint8_t id) {
    if (!active || id != active_id) return;
    cancelled = true;
    xSemaphoreGive(credit_sem);
}

void httpc_abort_all(void) {
    cancelled = true;
    xSemaphoreGive(credit_sem);
}
