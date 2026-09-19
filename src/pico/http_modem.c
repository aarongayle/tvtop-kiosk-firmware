// http_client.h over the ESP32 modem link. The modem does DNS, TCP, TLS and HTTP/1.1; this file
// is the shim that turns one http_get() into an H_HTTP_GET and feeds the answer back through the
// same http_sink_t the lwIP client uses, so kiosk_loop.c cannot tell the two apart.
//
// Flow control is the interesting part. The modem can pull from the network far faster than core 0
// can consume — a frame arrives, the loop renders for a hundred milliseconds, and nothing is
// draining the link. So the modem may only send body bytes it has credit for. Credit is granted up
// front in the GET and topped up as bytes reach the sink, and the window is kept below the receive
// ring so that even a modem that spends its whole credit at once cannot lap the DMA.
#include <stdio.h>
#include <string.h>

#include "http_client.h"
#include "modem_link.h"
#include "modem_io.h"

#include "pico/stdlib.h"
#include "pico/time.h"

// 8 KB in flight against a 16 KB ring. Two full windows still fit, so a top-up that crosses in the
// air with a burst cannot overrun.
#define HTTP_WINDOW 8192u
#define CREDIT_REFILL (HTTP_WINDOW / 2u)

struct http_client {
    const http_sink_t *sink;
    void *ctx;
    uint8_t id;              // rolls; replies for any other id are stale and dropped
    bool active;
    bool got_status;
    int status;
    uint32_t deadline;
    uint32_t timeout_ms;
    uint32_t credit_unacked;   // bytes delivered to the sink but not yet granted back
};

static struct http_client client;

http_client_t *http_client_get(void) { return &client; }

// The roots live on the modem (ESP-IDF's bundle, pinned at build time in modem/main/httpc.c):
// certificates the RP2350 never sees are certificates it never has to store or parse.
void http_client_init(http_client_t *c, const char *ca_pem, size_t ca_len) {
    (void)ca_pem; (void)ca_len;
    memset(c, 0, sizeof *c);
}

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

bool http_url_split(const char *url, bool *https, char *host, size_t hostcap, uint16_t *port, char *path, size_t pathcap) {
    if (!url) return false;
    bool tls;
    const char *p;
    if (strncmp(url, "http://", 7) == 0) { tls = false; p = url + 7; }
    else if (strncmp(url, "https://", 8) == 0) { tls = true; p = url + 8; }
    else return false;
    const char *hend = p;
    while (*hend && *hend != '/' && *hend != ':') hend++;
    size_t hn = (size_t)(hend - p);
    if (!hn || hn >= hostcap) return false;
    memcpy(host, p, hn);
    host[hn] = 0;
    uint16_t pt = tls ? 443 : 80;
    if (*hend == ':') {
        uint32_t v = 0;
        const char *q = hend + 1;
        if (*q < '0' || *q > '9') return false;
        for (; *q >= '0' && *q <= '9'; q++) { v = v * 10 + (uint32_t)(*q - '0'); if (v > 65535) return false; }
        pt = (uint16_t)v;
        hend = q;
    }
    if (*hend && *hend != '/') return false;
    const char *pp = *hend ? hend : "/";
    size_t pn = strlen(pp);
    if (pn >= pathcap) return false;
    memcpy(path, pp, pn + 1);
    if (https) *https = tls;
    if (port) *port = pt;
    return true;
}

static void finish(struct http_client *c, int err, int status) {
    if (!c->active) return;
    const http_sink_t *s = c->sink;
    void *ctx = c->ctx;
    c->active = false;
    c->sink = NULL;
    if (s && s->on_complete) s->on_complete(ctx, err, status);
}

int http_get(http_client_t *c, const char *url, const http_sink_t *sink, void *ctx, uint32_t timeout_ms) {
    if (c->active) return HTTP_ERR_BUSY;
    bool https;
    char host[128], path[KIOSK_MAX_URL];
    uint16_t port;
    if (!http_url_split(url, &https, host, sizeof host, &port, path, sizeof path)) return HTTP_ERR_URL;
    if (!modem_link_ready()) return HTTP_ERR_CONNECT;

    size_t url_len = strlen(url);
    if (url_len > MODEM_MAX_PAYLOAD - 12u) return HTTP_ERR_URL;

    uint8_t hdr[11];
    c->id++;
    hdr[0] = c->id;
    uint32_t t = timeout_ms, credit = HTTP_WINDOW;
    uint16_t ul = (uint16_t)url_len;
    memcpy(hdr + 1, &t, 4);
    memcpy(hdr + 5, &credit, 4);
    memcpy(hdr + 9, &ul, 2);
    if (!modem_link_send2(H_HTTP_GET, hdr, sizeof hdr, url, ul)) return HTTP_ERR_BUSY;

    c->sink = sink;
    c->ctx = ctx;
    c->active = true;
    c->got_status = false;
    c->status = 0;
    c->credit_unacked = 0;
    c->timeout_ms = timeout_ms;
    // The modem runs the authoritative timer; this one only covers a modem that has stopped
    // answering at all, so it is deliberately slack.
    c->deadline = now_ms() + timeout_ms + 5000u;
    return HTTP_OK;
}

void http_cancel(http_client_t *c) {
    if (!c->active) return;
    modem_link_send(H_HTTP_CANCEL, &c->id, 1);
    c->active = false;
    c->sink = NULL;
}

bool http_busy(const http_client_t *c) { return c->active; }

static void grant_flush(struct http_client *c);

void http_poll(http_client_t *c) {
    if (!c->active) return;
    // A grant that could not be queued (a full transmit ring) must be retried here. The modem
    // sends nothing without credit, so no further body arrives to drive the next grant: without
    // this retry one dropped top-up would stall the transfer until the request timed out.
    // Only the retry case: below the refill threshold grant() deliberately holds the credit back,
    // and flushing that here would put a top-up frame on the link every time round the main loop.
    if (c->credit_unacked >= CREDIT_REFILL) grant_flush(c);
    if ((int32_t)(now_ms() - c->deadline) >= 0) {
        printf("http: modem silent for %lu ms; abandoning request\n", (unsigned long)(c->timeout_ms + 5000u));
        modem_link_send(H_HTTP_CANCEL, &c->id, 1);
        finish(c, HTTP_ERR_TIMEOUT, c->status);
    }
}

// ---- link messages ------------------------------------------------------------------------------

static void grant_flush(struct http_client *c) {
    uint8_t buf[5];
    buf[0] = c->id;
    memcpy(buf + 1, &c->credit_unacked, 4);
    if (modem_link_send(H_HTTP_CREDIT, buf, sizeof buf)) c->credit_unacked = 0;
}

static void grant(struct http_client *c, uint32_t n) {
    c->credit_unacked += n;
    // Batched: a top-up per 1 KB chunk would put a frame on the link for every frame of body.
    if (c->credit_unacked >= CREDIT_REFILL) grant_flush(c);
}

void http_modem_on_message(uint8_t type, const uint8_t *p, uint16_t len) {
    struct http_client *c = &client;
    if (!len) return;
    if (!c->active || p[0] != c->id) return;   // a reply to a request we have already given up on
    c->deadline = now_ms() + c->timeout_ms + 5000u;

    switch (type) {
    case M_HTTP_STATUS: {
        if (len < 3) return;
        uint16_t st;
        memcpy(&st, p + 1, 2);
        c->status = st;
        c->got_status = true;
        if (c->sink && c->sink->on_status) c->sink->on_status(c->ctx, st);
        return;
    }
    case M_HTTP_HEADER: {
        if (len < 4) return;
        uint8_t nl = p[1];
        if (2u + nl + 2u > len) return;
        uint16_t vl;
        memcpy(&vl, p + 2 + nl, 2);
        if (4u + nl + vl > len) return;
        if (c->sink && c->sink->on_header)
            c->sink->on_header(c->ctx, (const char *)p + 2, nl, (const char *)p + 4 + nl, vl);
        return;
    }
    case M_HTTP_BODY: {
        uint16_t n = (uint16_t)(len - 1);
        if (!n) return;
        bool keep = true;
        if (c->sink && c->sink->on_body) keep = c->sink->on_body(c->ctx, p + 1, n);
        if (!keep) {
            modem_link_send(H_HTTP_CANCEL, &c->id, 1);
            finish(c, HTTP_ERR_ABORTED, c->status);
            return;
        }
        grant(c, n);
        return;
    }
    case M_HTTP_DONE: {
        if (len < 5) return;
        int16_t err;
        uint16_t st;
        memcpy(&err, p + 1, 2);
        memcpy(&st, p + 3, 2);
        finish(c, err, st ? st : c->status);
        return;
    }
    default:
        return;
    }
}

void http_modem_on_modem_restart(void) {
    struct http_client *c = &client;
    if (c->active) finish(c, HTTP_ERR_CLOSED, c->status);
}
