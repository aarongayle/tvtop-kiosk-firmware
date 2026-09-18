#include "link.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define TAG "link"
#define UART_PORT ((uart_port_t)MODEM_LINK_UART)
#define RX_DRIVER_BUF 4096
#define TX_DRIVER_BUF 4096

static SemaphoreHandle_t tx_mux;
static link_handler_t handler;

// ---- log relay ----------------------------------------------------------------------------------
//
// ESP_LOG can fire from any task, including from inside link_send while it holds the transmit
// mutex (the UART driver logs on error). Lines therefore go into a queue and are sent by the
// receive task, which never logs from inside a send. A full queue drops lines rather than blocking
// whoever was logging: losing a log line is always better than stalling the radio.
#define LOG_LINE_MAX 160
#define LOG_QUEUE_LEN 16
typedef struct { uint8_t level; uint8_t len; char text[LOG_LINE_MAX]; } log_line_t;
static QueueHandle_t log_q;
static int (*prev_vprintf)(const char *, va_list);

static int log_vprintf(const char *fmt, va_list ap) {
    if (!log_q) return prev_vprintf ? prev_vprintf(fmt, ap) : 0;
    log_line_t line;
    int n = vsnprintf(line.text, sizeof line.text, fmt, ap);
    if (n <= 0) return n;
    if (n > (int)sizeof line.text - 1) n = (int)sizeof line.text - 1;
    // ESP_LOG prefixes a colour escape and a level letter: "\033[0;32mI (123) tag: ...". Take the
    // level from it and strip the escape, so the kiosk console prints something readable.
    char *p = line.text;
    int len = n;
    if (p[0] == '\033') {
        char *m = memchr(p, 'm', (size_t)len);
        if (m) { len -= (int)(m + 1 - p); p = m + 1; }
    }
    uint8_t level = MODEM_LOG_INFO;
    if (len > 0) {
        switch (p[0]) {
        case 'E': level = MODEM_LOG_ERROR; break;
        case 'W': level = MODEM_LOG_WARN; break;
        case 'D': case 'V': level = MODEM_LOG_DEBUG; break;
        default: break;
        }
    }
    while (len > 0 && (p[len - 1] == '\n' || p[len - 1] == '\r')) len--;
    if (len >= 4 && memcmp(p + len - 4, "\033[0m", 4) == 0) len -= 4;   // the colour reset
    while (len > 0 && (p[len - 1] == '\n' || p[len - 1] == '\r')) len--;
    if (len <= 0) return n;
    line.level = level;
    line.len = (uint8_t)(len > 255 ? 255 : len);
    memmove(line.text, p, (size_t)line.len);
    xQueueSend(log_q, &line, 0);   // never blocks: a dropped line beats a stalled task
    return n;
}

static void log_drain(void) {
    log_line_t line;
    while (log_q && xQueueReceive(log_q, &line, 0) == pdTRUE) {
        uint8_t lvl = line.level;
        link_send2(M_LOG, &lvl, 1, line.text, line.len);
    }
}

void link_log_redirect(void) {
    log_q = xQueueCreate(LOG_QUEUE_LEN, sizeof(log_line_t));
    prev_vprintf = esp_log_set_vprintf(log_vprintf);
}

// ---- transmit -----------------------------------------------------------------------------------

bool link_send2(uint8_t type, const void *a, uint16_t alen, const void *b, uint16_t blen) {
    uint16_t len = (uint16_t)(alen + blen);
    if (len > MODEM_MAX_PAYLOAD) return false;
    static uint8_t frame[MODEM_FRAME_MAX];   // guarded by tx_mux
    if (xSemaphoreTake(tx_mux, pdMS_TO_TICKS(2000)) != pdTRUE) return false;
    frame[0] = MODEM_SYNC0;
    frame[1] = MODEM_SYNC1;
    frame[2] = type;
    frame[3] = 0;
    frame[4] = (uint8_t)(len & 0xff);
    frame[5] = (uint8_t)(len >> 8);
    if (alen) memcpy(frame + MODEM_HDR_BYTES, a, alen);
    if (blen) memcpy(frame + MODEM_HDR_BYTES + alen, b, blen);
    uint16_t crc = modem_crc16(MODEM_CRC_INIT, frame + 2, (size_t)(4 + len));
    frame[MODEM_HDR_BYTES + len] = (uint8_t)(crc & 0xff);
    frame[MODEM_HDR_BYTES + len + 1] = (uint8_t)(crc >> 8);
    int written = uart_write_bytes(UART_PORT, frame, MODEM_HDR_BYTES + len + MODEM_CRC_BYTES);
    xSemaphoreGive(tx_mux);
    return written == (int)(MODEM_HDR_BYTES + len + MODEM_CRC_BYTES);
}

bool link_send(uint8_t type, const void *payload, uint16_t len) {
    return link_send2(type, payload, len, NULL, 0);
}

void link_send_hello(void) {
    uint8_t buf[9 + 16];
    buf[0] = MODEM_PROTO_VERSION;
#if CONFIG_IDF_TARGET_ESP32C2
    buf[1] = MODEM_CHIP_ESP32C2;
#elif CONFIG_IDF_TARGET_ESP32C3
    buf[1] = MODEM_CHIP_ESP32C3;
#else
    buf[1] = MODEM_CHIP_UNKNOWN;
#endif
    esp_read_mac(buf + 2, ESP_MAC_WIFI_STA);
    const char *fw = MODEM_FW_VERSION;
    uint8_t n = (uint8_t)strlen(fw);
    if (n > 16) n = 16;
    buf[8] = n;
    memcpy(buf + 9, fw, n);
    link_send(M_HELLO, buf, (uint16_t)(9 + n));
}

// ---- receive ------------------------------------------------------------------------------------

static void rx_task(void *arg) {
    (void)arg;
    enum { S_SYNC0, S_SYNC1, S_HEAD, S_PAYLOAD, S_CRC } st = S_SYNC0;
    uint8_t head[4], payload[MODEM_MAX_PAYLOAD];
    uint16_t fill = 0, plen = 0, crc_want = 0, crc = 0;
    uint8_t chunk[256];

    for (;;) {
        int n = uart_read_bytes(UART_PORT, chunk, sizeof chunk, pdMS_TO_TICKS(20));
        for (int i = 0; i < n; i++) {
            uint8_t b = chunk[i];
            switch (st) {
            case S_SYNC0: if (b == MODEM_SYNC0) st = S_SYNC1; break;
            case S_SYNC1:
                if (b == MODEM_SYNC1) { st = S_HEAD; fill = 0; }
                else if (b != MODEM_SYNC0) st = S_SYNC0;
                break;
            case S_HEAD:
                head[fill++] = b;
                if (fill < 4) break;
                plen = (uint16_t)(head[2] | (head[3] << 8));
                if (plen > MODEM_MAX_PAYLOAD) { st = S_SYNC0; break; }
                crc = modem_crc16(MODEM_CRC_INIT, head, 4);
                fill = 0;
                st = plen ? S_PAYLOAD : S_CRC;
                break;
            case S_PAYLOAD:
                payload[fill++] = b;
                if (fill < plen) break;
                crc = modem_crc16(crc, payload, plen);
                fill = 0;
                st = S_CRC;
                break;
            case S_CRC:
                if (fill == 0) { crc_want = b; fill = 1; break; }
                crc_want = (uint16_t)(crc_want | (b << 8));
                st = S_SYNC0;
                fill = 0;
                if (crc_want == crc && handler) handler(head[0], payload, plen);
                break;
            }
        }
        log_drain();
    }
}

void link_init(link_handler_t h) {
    handler = h;
    tx_mux = xSemaphoreCreateMutex();

    uart_config_t cfg = {
        .baud_rate = MODEM_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, RX_DRIVER_BUF, TX_DRIVER_BUF, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, MODEM_LINK_TX_PIN, MODEM_LINK_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    // The kiosk can sit inside a render for 100 ms at a time; nothing here should give up on it.
    xTaskCreate(rx_task, "link_rx", 4096, NULL, 12, NULL);
    ESP_LOGI(TAG, "link up on UART%d (tx %d, rx %d) at %u baud",
             MODEM_LINK_UART, MODEM_LINK_TX_PIN, MODEM_LINK_RX_PIN, (unsigned)MODEM_BAUD);
}
