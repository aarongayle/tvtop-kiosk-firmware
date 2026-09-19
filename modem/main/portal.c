#include "portal.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "link.h"

#define TAG "portal"
#define PORTAL_PORT 80
#define DNS_PORT 53
#define OUT_BUF 2048          // matches the host's per-connection window (provision.c)
#define IN_CHUNK (MODEM_MAX_PAYLOAD - 1)
#define PORTAL_ADDR 0x0104A8C0u   // 192.168.4.1, network byte order

typedef struct {
    int fd;                   // -1 when free
    uint8_t out[OUT_BUF];
    uint16_t out_head, out_tail;
    bool closing;             // host has finished; close once out has drained
} conn_t;

static conn_t conns[MODEM_MAX_SOCKS];
static int listen_fd = -1, dns_fd = -1;
static volatile bool want_listen;
static SemaphoreHandle_t mux;

static void conn_drop(int i, bool tell_host) {
    if (conns[i].fd < 0) return;
    close(conns[i].fd);
    conns[i].fd = -1;
    conns[i].out_head = conns[i].out_tail = 0;
    conns[i].closing = false;
    if (tell_host) {
        uint8_t id = (uint8_t)i;
        link_send(M_SOCK_CLOSE, &id, 1);
    }
}

void portal_write(uint8_t conn, const uint8_t *data, uint16_t len) {
    if (conn >= MODEM_MAX_SOCKS || !len) return;
    xSemaphoreTake(mux, portMAX_DELAY);
    conn_t *c = &conns[conn];
    if (c->fd >= 0 && (size_t)(c->out_tail + len) <= sizeof c->out) {
        memcpy(c->out + c->out_tail, data, len);
        c->out_tail = (uint16_t)(c->out_tail + len);
    }
    xSemaphoreGive(mux);
}

void portal_close(uint8_t conn, bool abort) {
    if (conn >= MODEM_MAX_SOCKS) return;
    xSemaphoreTake(mux, portMAX_DELAY);
    conn_t *c = &conns[conn];
    if (c->fd >= 0) {
        // An orderly close waits for whatever is still queued: the host asks to close as soon as
        // it has handed over the last byte of the response, and the phone must see all of it.
        if (abort || c->out_head == c->out_tail) conn_drop((int)conn, false);
        else c->closing = true;
    }
    xSemaphoreGive(mux);
}

void portal_close_all(void) {
    xSemaphoreTake(mux, portMAX_DELAY);
    for (int i = 0; i < MODEM_MAX_SOCKS; i++) conn_drop(i, false);
    xSemaphoreGive(mux);
}

// ---- DNS ----------------------------------------------------------------------------------------

// Answers any A query with the portal's own address, which is what makes phones pop the "sign in
// to Wi-Fi" sheet. Anything that is not a single-question query is ignored.
static void dns_serve(void) {
    uint8_t buf[256];
    struct sockaddr_storage from;
    socklen_t flen = sizeof from;
    int n = recvfrom(dns_fd, buf, sizeof buf, 0, (struct sockaddr *)&from, &flen);
    if (n < 12 + 5) return;
    if (buf[2] & 0x80) return;                       // already a response
    if (buf[4] != 0 || buf[5] != 1) return;          // exactly one question

    int p = 12;
    while (p < n && buf[p]) p += buf[p] + 1;         // walk the QNAME labels
    if (p + 5 > n) return;
    int qend = p + 5;                                 // NUL + QTYPE + QCLASS
    uint16_t qtype = (uint16_t)((buf[qend - 4] << 8) | buf[qend - 3]);
    if (qtype != 1 && qtype != 28) return;            // A or AAAA only

    if (qend + 16 > (int)sizeof buf) return;
    buf[2] = 0x84;   // response, authoritative
    buf[3] = 0x00;
    buf[6] = 0; buf[7] = qtype == 1 ? 1 : 0;          // no AAAA answer: the phone falls back to A
    buf[8] = 0; buf[9] = 0;
    buf[10] = 0; buf[11] = 0;
    int o = qend;
    if (qtype == 1) {
        buf[o++] = 0xC0; buf[o++] = 0x0C;             // pointer to the question's name
        buf[o++] = 0; buf[o++] = 1;                   // A
        buf[o++] = 0; buf[o++] = 1;                   // IN
        buf[o++] = 0; buf[o++] = 0; buf[o++] = 0; buf[o++] = 30;   // TTL 30 s
        buf[o++] = 0; buf[o++] = 4;
        uint32_t addr = PORTAL_ADDR;
        memcpy(buf + o, &addr, 4);
        o += 4;
    }
    sendto(dns_fd, buf, (size_t)o, 0, (struct sockaddr *)&from, flen);
}

// ---- listener and relay -------------------------------------------------------------------------

static void open_sockets(void) {
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);

    listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_fd >= 0) {
        int one = 1;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        a.sin_port = htons(PORTAL_PORT);
        if (bind(listen_fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(listen_fd, 4) < 0) {
            ESP_LOGE(TAG, "listen on :%d failed", PORTAL_PORT);
            close(listen_fd);
            listen_fd = -1;
        }
    }

    dns_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (dns_fd >= 0) {
        a.sin_port = htons(DNS_PORT);
        if (bind(dns_fd, (struct sockaddr *)&a, sizeof a) < 0) {
            ESP_LOGE(TAG, "bind :%d failed", DNS_PORT);
            close(dns_fd);
            dns_fd = -1;
        }
    }
    ESP_LOGI(TAG, "portal listening on :80, DNS hijack on :53");
}

static void close_sockets(void) {
    portal_close_all();
    if (listen_fd >= 0) { close(listen_fd); listen_fd = -1; }
    if (dns_fd >= 0) { close(dns_fd); dns_fd = -1; }
}

static void do_accept(void) {
    struct sockaddr_storage peer;
    socklen_t plen = sizeof peer;
    int fd = accept(listen_fd, (struct sockaddr *)&peer, &plen);
    if (fd < 0) return;
    int slot = -1;
    xSemaphoreTake(mux, portMAX_DELAY);
    for (int i = 0; i < MODEM_MAX_SOCKS; i++) if (conns[i].fd < 0) { slot = i; break; }
    if (slot >= 0) {
        conns[slot].fd = fd;
        conns[slot].out_head = conns[slot].out_tail = 0;
    }
    xSemaphoreGive(mux);
    if (slot < 0) { close(fd); return; }
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    uint8_t id = (uint8_t)slot;
    link_send(M_SOCK_OPEN, &id, 1);
}

static void do_read(int i) {
    uint8_t buf[1 + IN_CHUNK];
    buf[0] = (uint8_t)i;
    int n = recv(conns[i].fd, buf + 1, IN_CHUNK, 0);
    if (n > 0) { link_send(M_SOCK_DATA, buf, (uint16_t)(1 + n)); return; }
    if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
        xSemaphoreTake(mux, portMAX_DELAY);
        conn_drop(i, true);
        xSemaphoreGive(mux);
    }
}

static void do_write(int i) {
    xSemaphoreTake(mux, portMAX_DELAY);
    conn_t *c = &conns[i];
    uint16_t pending = (uint16_t)(c->out_tail - c->out_head);
    int sent = pending ? send(c->fd, c->out + c->out_head, pending, 0) : 0;
    if (sent > 0) {
        c->out_head = (uint16_t)(c->out_head + sent);
        if (c->out_head == c->out_tail) {
            c->out_head = c->out_tail = 0;
            // The ack still goes out: the host has already released this connection,
            // and an extra M_SOCK_SENT for a number it no longer holds is ignored.
            if (c->closing) conn_drop(i, false);
        }
    } else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        conn_drop(i, true);
        sent = 0;
    }
    xSemaphoreGive(mux);
    if (sent > 0) {
        uint8_t buf[3];
        buf[0] = (uint8_t)i;
        uint16_t n16 = (uint16_t)sent;
        memcpy(buf + 1, &n16, 2);
        link_send(M_SOCK_SENT, buf, sizeof buf);
    }
}

static void portal_task(void *arg) {
    (void)arg;
    for (;;) {
        if (want_listen && listen_fd < 0) open_sockets();
        if (!want_listen && listen_fd >= 0) close_sockets();
        if (listen_fd < 0) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }

        fd_set rd, wr;
        FD_ZERO(&rd);
        FD_ZERO(&wr);
        int maxfd = listen_fd;
        FD_SET(listen_fd, &rd);
        if (dns_fd >= 0) { FD_SET(dns_fd, &rd); if (dns_fd > maxfd) maxfd = dns_fd; }
        for (int i = 0; i < MODEM_MAX_SOCKS; i++) {
            int fd = conns[i].fd;
            if (fd < 0) continue;
            FD_SET(fd, &rd);
            if (conns[i].out_head != conns[i].out_tail) FD_SET(fd, &wr);
            if (fd > maxfd) maxfd = fd;
        }
        struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
        if (select(maxfd + 1, &rd, &wr, NULL, &tv) <= 0) continue;

        if (FD_ISSET(listen_fd, &rd)) do_accept();
        if (dns_fd >= 0 && FD_ISSET(dns_fd, &rd)) dns_serve();
        for (int i = 0; i < MODEM_MAX_SOCKS; i++) {
            int fd = conns[i].fd;
            if (fd < 0) continue;
            if (FD_ISSET(fd, &wr)) do_write(i);
            if (conns[i].fd >= 0 && FD_ISSET(fd, &rd)) do_read(i);
        }
    }
}

void portal_init(void) {
    mux = xSemaphoreCreateMutex();
    for (int i = 0; i < MODEM_MAX_SOCKS; i++) conns[i].fd = -1;
    xTaskCreate(portal_task, "portal", 4096, NULL, 5, NULL);
}

void portal_enable(bool on) {
    want_listen = on;
}
