// Minimal HTTP/1.1 GET client on lwIP raw/altcp with optional TLS (KIOSK_TLS). One request at a
// time. The connection is kept alive between requests to the same origin (the kiosk server holds
// long-polls open for 25 s and allows keep-alive), and transparently reconnected when the server
// closes it. lwIP callbacks run in the cyw43 background context; body bytes are queued as pbufs
// and delivered to the sink from http_poll() in main context, so the sink may parse, render and
// write flash.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "kiosk_config.h"

enum {
    HTTP_OK = 0,
    HTTP_ERR_URL = -1,        // unparseable / unsupported scheme (https without KIOSK_TLS)
    HTTP_ERR_DNS = -2,
    HTTP_ERR_CONNECT = -3,
    HTTP_ERR_TIMEOUT = -4,    // no bytes for timeout_ms
    HTTP_ERR_CLOSED = -5,     // connection closed before the response was complete
    HTTP_ERR_PROTOCOL = -6,   // malformed response
    HTTP_ERR_ABORTED = -7,    // sink returned false / http_cancel
    HTTP_ERR_TLS = -8,
    HTTP_ERR_BUSY = -9,
    HTTP_ERR_TOO_MANY_REDIRECTS = -10,
};

typedef struct {
    void (*on_status)(void *ctx, int status);                                   // once, before body
    void (*on_header)(void *ctx, const char *name, size_t nlen, const char *value, size_t vlen);  // optional
    bool (*on_body)(void *ctx, const uint8_t *data, size_t len);                // return false to abort
    void (*on_complete)(void *ctx, int err, int status);                        // exactly once per http_get
} http_sink_t;

typedef struct http_client http_client_t;   // opaque, defined in http_client.c (one static instance)

http_client_t *http_client_get(void);
// ca_pem may be NULL (TLS then verifies nothing; a warning is logged).
void http_client_init(http_client_t *c, const char *ca_pem, size_t ca_len);
// Starts a GET. Follows up to 3 redirects (301/302/303/307/308). Returns 0 or HTTP_ERR_BUSY/URL.
int http_get(http_client_t *c, const char *url, const http_sink_t *sink, void *ctx, uint32_t timeout_ms);
void http_cancel(http_client_t *c);
bool http_busy(const http_client_t *c);
// Drives delivery and timeouts; call from the main loop as often as possible.
void http_poll(http_client_t *c);

// URL helper (shared with the kiosk loop): splits scheme/host/port/path. Buffers must hold
// KIOSK_MAX_URL. Returns false if unparseable.
bool http_url_split(const char *url, bool *https, char *host, size_t hostcap, uint16_t *port, char *path, size_t pathcap);
