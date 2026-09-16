// HTTP/1.1 GET client on lwIP altcp (plain TCP, or mbedTLS when KIOSK_TLS).
//
// Threading model (pico_cyw43_arch_lwip_threadsafe_background): lwIP callbacks run in the cyw43
// background context (a low-priority IRQ / alarm). Every lwIP call made from the main loop is
// bracketed with cyw43_arch_lwip_begin/end, which takes the same lock the background context
// holds while running callbacks, so callbacks and main-context code never interleave. The
// callbacks themselves do the minimum: the recv callback only appends the pbuf to a chain, the
// err callback only records that the pcb is gone. All parsing, all sink calls and all acking
// (altcp_recved) happen in http_poll() from main context, outside the lock, so the sink can
// decode JSON and write flash for as long as it likes while the Wi-Fi stack keeps running.
//
// Flow control: bytes are acknowledged to TCP only after the sink has consumed them, so the
// receive window is the only buffering there is. A 320 KB static set streams through TCP_WND
// bytes of pbufs.
#include "http_client.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifndef KIOSK_MODEL
#define KIOSK_MODEL "pico-w" // CMake sets it from PICO_BOARD; this covers the host test build
#endif

#ifdef HTTP_CLIENT_STANDALONE_TEST
// host/tests/test_http.c includes this file and implements a fake lwIP behind these declarations
// (same names and semantics as the real ones, see the SDK headers listed in docs/NETWORK.md).
#undef KIOSK_TLS
#define KIOSK_TLS 0
typedef int8_t err_t; typedef uint8_t u8_t; typedef uint16_t u16_t;
enum { ERR_OK = 0, ERR_MEM = -1, ERR_ARG = -16, ERR_INPROGRESS = -5, ERR_CLSD = -15, ERR_ABRT = -13, ERR_RST = -14, ERR_CONN = -11 };
enum { IPADDR_TYPE_ANY = 46 };
#define TCP_WRITE_FLAG_COPY 0x01
typedef struct { uint32_t addr; } ip_addr_t;
struct altcp_pcb;
struct pbuf { struct pbuf *next; void *payload; u16_t tot_len; u16_t len; };
typedef err_t (*altcp_recv_fn)(void *arg, struct altcp_pcb *pcb, struct pbuf *p, err_t err);
typedef void (*altcp_err_fn)(void *arg, err_t err);
typedef err_t (*altcp_connected_fn)(void *arg, struct altcp_pcb *pcb, err_t err);
typedef err_t (*altcp_poll_fn)(void *arg, struct altcp_pcb *pcb);
typedef void (*dns_found_callback)(const char *name, const ip_addr_t *ipaddr, void *arg);
struct altcp_pcb *altcp_tcp_new_ip_type(u8_t ip_type);
void altcp_arg(struct altcp_pcb *pcb, void *arg);
void altcp_recv(struct altcp_pcb *pcb, altcp_recv_fn f);
void altcp_err(struct altcp_pcb *pcb, altcp_err_fn f);
void altcp_poll(struct altcp_pcb *pcb, altcp_poll_fn f, u8_t interval);
err_t altcp_connect(struct altcp_pcb *pcb, const ip_addr_t *ip, u16_t port, altcp_connected_fn f);
err_t altcp_write(struct altcp_pcb *pcb, const void *data, u16_t len, u8_t flags);
err_t altcp_output(struct altcp_pcb *pcb);
void altcp_recved(struct altcp_pcb *pcb, u16_t len);
err_t altcp_close(struct altcp_pcb *pcb);
void altcp_abort(struct altcp_pcb *pcb);
err_t dns_gethostbyname(const char *host, ip_addr_t *addr, dns_found_callback cb, void *arg);
u8_t pbuf_free(struct pbuf *p);
void pbuf_cat(struct pbuf *h, struct pbuf *t);
void cyw43_arch_lwip_begin(void);
void cyw43_arch_lwip_end(void);
uint32_t test_now_ms(void);
#define get_absolute_time() 0
#define to_ms_since_boot(t) test_now_ms()
#else
#include "pico/cyw43_arch.h"
#include "pico/time.h"
#include "lwip/altcp.h"
#if KIOSK_TLS
#include "mbedtls/platform_time.h"
// mbedTLS wants a millisecond clock for session/handshake timing (MBEDTLS_PLATFORM_MS_TIME_ALT);
// there is no RTC, so it counts from boot. Only differences are ever used.
mbedtls_ms_time_t mbedtls_ms_time(void) { return (mbedtls_ms_time_t)to_ms_since_boot(get_absolute_time()); }
#endif
#include "lwip/altcp_tcp.h"
#include "lwip/dns.h"
#include "lwip/pbuf.h"
#include "lwip/err.h"
#include "lwip/ip_addr.h"
#include "lwip/tcp.h"
#if KIOSK_TLS
#include "lwip/altcp_tls.h"
#include "mbedtls/ssl.h"
#endif
#endif

#define HTTP_LINE_MAX 512
#define HTTP_MAX_REDIRECTS 3
#define HTTP_CONNECT_TIMEOUT_MS 15000   // DNS + TCP + TLS handshake; shorter than a long-poll timeout
#define HTTP_REQ_MAX 800                // "GET <path 255> HTTP/1.1" + Host <255:port> + fixed headers

typedef enum {
    CS_NONE = 0,     // no pcb
    CS_RESOLVING,    // dns_gethostbyname pending
    CS_CONNECTING,   // altcp_connect issued (TLS: handshake included)
    CS_OPEN,         // connected; a request may be in flight or the connection is idle
} conn_state_t;

typedef enum {
    RS_STATUS = 0,   // waiting for the status line
    RS_HEADERS,
    RS_BODY,         // Content-Length known (remaining) or unknown (until close)
    RS_CHUNK_SIZE,
    RS_CHUNK_DATA,
    RS_CHUNK_CRLF,   // the CRLF after a chunk's data
    RS_TRAILERS,     // after the 0 chunk: header lines until an empty line
    RS_DONE,
} resp_state_t;

struct http_client {
    const char *ca_pem;
    size_t ca_len;
#if KIOSK_TLS
    struct altcp_tls_config *tls_conf;
#endif
    // ---- connection (main context owns these except where noted) ----
    struct altcp_pcb *pcb;
    conn_state_t cstate;
    bool conn_https;
    char conn_host[KIOSK_MAX_URL];
    uint16_t conn_port;
    uint32_t dns_gen;                 // identifies the DNS lookup a late callback belongs to
    ip_addr_t addr;
    // Written by lwIP callbacks (background context), read by http_poll under the lwIP lock.
    struct pbuf *rx;                  // received, not yet delivered
    volatile bool rx_closed;          // peer sent FIN (recv NULL)
    volatile bool pcb_gone;           // err callback ran: pcb already freed by lwIP
    volatile err_t gone_err;
    volatile bool connected;          // connected callback ran (TLS: handshake done)
    volatile bool dns_done;
    volatile bool dns_ok;
    // ---- request ----
    bool busy;
    int deferred_err;                 // a failure detected inside http_get, reported from http_poll
    const http_sink_t *sink;
    void *ctx;
    uint32_t timeout_ms;
    uint32_t last_activity_ms;
    bool https;
    char host[KIOSK_MAX_URL];
    char path[KIOSK_MAX_URL];
    uint16_t port;
    uint8_t redirects;
    bool reused;                      // request went out on a kept-alive connection
    bool retried;                     // ...and was already re-issued once after a stale close
    bool got_status;                  // at least the status line arrived for this attempt
    // ---- response parser ----
    resp_state_t rstate;
    char line[HTTP_LINE_MAX];
    uint16_t linelen;
    bool line_overflow;
    int status;
    bool chunked;
    bool have_length;
    bool close_after;                 // Connection: close, or HTTP/1.0
    bool redirecting;                 // 3xx with a Location we will follow: sink stays silent
    uint32_t remaining;               // body bytes (have_length) or chunk bytes left
    char location[KIOSK_MAX_URL];
    bool have_location;
};

static http_client_t g_http;

static inline uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

http_client_t *http_client_get(void) { return &g_http; }

// ---------------------------------------------------------------------------------------------
// URL helper
// ---------------------------------------------------------------------------------------------

bool http_url_split(const char *url, bool *https, char *host, size_t hostcap, uint16_t *port, char *path, size_t pathcap) {
    if (!url || !https || !host || !port || !path || hostcap == 0 || pathcap == 0) return false;
    const char *p;
    if (strncmp(url, "http://", 7) == 0) { *https = false; p = url + 7; }
    else if (strncmp(url, "https://", 8) == 0) { *https = true; p = url + 8; }
    else return false;
    // authority ends at '/', '?', '#' or end of string
    const char *a_end = p;
    while (*a_end && *a_end != '/' && *a_end != '?' && *a_end != '#') a_end++;
    if (a_end == p) return false;
    // userinfo is not supported; an '@' would otherwise be sent as part of the host
    for (const char *q = p; q < a_end; q++) if (*q == '@') return false;
    // port
    const char *h_end = a_end;
    uint32_t prt = *https ? 443 : 80;
    const char *colon = NULL;
    if (*p == '[') return false;   // IPv6 literals: not supported (IPv4-only stack)
    for (const char *q = p; q < a_end; q++) if (*q == ':') colon = q;
    if (colon) {
        h_end = colon;
        const char *d = colon + 1;
        if (d == a_end) return false;
        prt = 0;
        for (; d < a_end; d++) {
            if (*d < '0' || *d > '9') return false;
            prt = prt * 10 + (uint32_t)(*d - '0');
            if (prt > 65535) return false;
        }
        if (prt == 0) return false;
    }
    size_t hlen = (size_t)(h_end - p);
    if (hlen == 0 || hlen >= hostcap) return false;
    for (size_t i = 0; i < hlen; i++) {
        char ch = p[i];
        // what a hostname may contain; anything else would corrupt the request line or SNI
        bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '.' || ch == '_';
        if (!ok) return false;
    }
    memcpy(host, p, hlen);
    host[hlen] = 0;
    *port = (uint16_t)prt;
    // path + query, without the fragment
    const char *pa = a_end;
    const char *pa_end = pa;
    while (*pa_end && *pa_end != '#') pa_end++;
    size_t plen = (size_t)(pa_end - pa);
    if (plen == 0) {
        if (pathcap < 2) return false;
        path[0] = '/'; path[1] = 0;
        return true;
    }
    size_t need = plen + (pa[0] == '/' ? 0 : 1);
    if (need >= pathcap) return false;
    size_t o = 0;
    if (pa[0] != '/') path[o++] = '/';
    for (size_t i = 0; i < plen; i++) {
        unsigned char ch = (unsigned char)pa[i];
        if (ch <= ' ' || ch >= 0x7f) return false;   // would break the request line
        path[o++] = (char)ch;
    }
    path[o] = 0;
    return true;
}

// ---------------------------------------------------------------------------------------------
// lwIP callbacks (background context). They touch only the volatile hand-off fields.
// ---------------------------------------------------------------------------------------------

static err_t cb_recv(void *arg, struct altcp_pcb *pcb, struct pbuf *p, err_t err) {
    http_client_t *c = (http_client_t *)arg;
    (void)pcb; (void)err;
    if (!c) { if (p) pbuf_free(p); return ERR_OK; }
    if (!p) { c->rx_closed = true; return ERR_OK; }
    if (c->rx) pbuf_cat(c->rx, p); else c->rx = p;
    // Deliberately no altcp_recved here: the window closes until http_poll consumes the bytes.
    return ERR_OK;
}

static void cb_err(void *arg, err_t err) {
    http_client_t *c = (http_client_t *)arg;
    if (!c) return;
    // lwIP has already freed the pcb (TLS: the wrapper too). Never touch it again.
    c->pcb = NULL;
    c->pcb_gone = true;
    c->gone_err = err;
}

static err_t cb_connected(void *arg, struct altcp_pcb *pcb, err_t err) {
    http_client_t *c = (http_client_t *)arg;
    (void)pcb;
    if (!c) return ERR_OK;
    if (err != ERR_OK) { c->pcb_gone = true; c->gone_err = err; c->pcb = NULL; return ERR_OK; }
    c->connected = true;
    return ERR_OK;
}

// Registered so that the TLS layer's own lower-level poll runs: altcp_tls_mbedtls only installs
// altcp_mbedtls_lower_poll when the application sets a poll, and that is where a decrypt that
// stalled on an empty pbuf pool is retried. Plain TCP does nothing with it.
static err_t cb_poll(void *arg, struct altcp_pcb *pcb) {
    (void)arg; (void)pcb;
    return ERR_OK;
}

static void cb_dns(const char *name, const ip_addr_t *ipaddr, void *arg) {
    http_client_t *c = &g_http;
    (void)name;
    // A lookup started for an earlier, since-cancelled request must not be mistaken for ours.
    if ((uint32_t)(uintptr_t)arg != c->dns_gen) return;
    if (ipaddr) { c->addr = *ipaddr; c->dns_ok = true; }
    else c->dns_ok = false;
    c->dns_done = true;
}

// ---------------------------------------------------------------------------------------------
// connection management (main context; caller holds no lock)
// ---------------------------------------------------------------------------------------------

static void drop_rx_locked(http_client_t *c) {
    if (c->rx) { pbuf_free(c->rx); c->rx = NULL; }
}

// Closes whatever connection exists. Safe to call in any state.
static void conn_close(http_client_t *c) {
    cyw43_arch_lwip_begin();
    if (c->pcb && !c->pcb_gone) {
        struct altcp_pcb *pcb = c->pcb;
        // Detach first: a pcb lingering in FIN_WAIT/TIME_WAIT may still fire err later.
        altcp_arg(pcb, NULL);
        altcp_recv(pcb, NULL);
        altcp_err(pcb, NULL);
        altcp_poll(pcb, NULL, 0);
        if (altcp_close(pcb) != ERR_OK) altcp_abort(pcb);
    }
    c->pcb = NULL;
    drop_rx_locked(c);
    c->dns_gen++;   // orphans any DNS callback still pending
    cyw43_arch_lwip_end();
    c->cstate = CS_NONE;
    c->pcb_gone = false;
    c->rx_closed = false;
    c->connected = false;
    c->dns_done = false;
    c->conn_host[0] = 0;
}

static void finish(http_client_t *c, int err) {
    if (!c->busy) return;
    c->busy = false;
    const http_sink_t *sink = c->sink;
    void *ctx = c->ctx;
    int status = c->got_status ? c->status : 0;
    c->sink = NULL;
    c->ctx = NULL;
    if (sink && sink->on_complete) sink->on_complete(ctx, err, status);
}

static void fail(http_client_t *c, int err) {
    conn_close(c);
    finish(c, err);
}

static void reset_response_parser(http_client_t *c) {
    c->rstate = RS_STATUS;
    c->linelen = 0;
    c->line_overflow = false;
    c->status = 0;
    c->chunked = false;
    c->have_length = false;
    c->close_after = false;
    c->redirecting = false;
    c->remaining = 0;
    c->location[0] = 0;
    c->have_location = false;
    c->got_status = false;
}

// Must be called under the lwIP lock with c->cstate == CS_OPEN.
static bool send_request_locked(http_client_t *c) {
    static char req[HTTP_REQ_MAX];
    int n;
    bool default_port = c->port == (c->https ? 443 : 80);
    if (default_port)
        n = snprintf(req, sizeof req,
                     "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: tvtop-kiosk/" KIOSK_FW_VERSION " (" KIOSK_MODEL ")\r\n"
                     "Accept: application/json\r\nAccept-Encoding: identity\r\nConnection: keep-alive\r\n\r\n",
                     c->path, c->host);
    else
        n = snprintf(req, sizeof req,
                     "GET %s HTTP/1.1\r\nHost: %s:%u\r\nUser-Agent: tvtop-kiosk/" KIOSK_FW_VERSION " (" KIOSK_MODEL ")\r\n"
                     "Accept: application/json\r\nAccept-Encoding: identity\r\nConnection: keep-alive\r\n\r\n",
                     c->path, c->host, (unsigned)c->port);
    if (n <= 0 || n >= (int)sizeof req) return false;
    // TCP_WRITE_FLAG_COPY: req is static but reused by the next request before this one is acked.
    err_t e = altcp_write(c->pcb, req, (u16_t)n, TCP_WRITE_FLAG_COPY);
    if (e != ERR_OK) return false;
    altcp_output(c->pcb);
    return true;
}

// Opens a connection for the current request (DNS first). Returns false with the error code set.
static int conn_open(http_client_t *c) {
    conn_close(c);
    cyw43_arch_lwip_begin();
#if KIOSK_TLS
    if (c->https) {
        if (!c->tls_conf) { cyw43_arch_lwip_end(); return HTTP_ERR_TLS; }
        c->pcb = altcp_tls_new(c->tls_conf, IPADDR_TYPE_ANY);
        if (c->pcb) {
            mbedtls_ssl_context *ssl = (mbedtls_ssl_context *)altcp_tls_context(c->pcb);
            // SNI + the name the peer certificate is checked against; must precede connect.
            if (!ssl || mbedtls_ssl_set_hostname(ssl, c->host) != 0) {
                altcp_abort(c->pcb);
                c->pcb = NULL;
                cyw43_arch_lwip_end();
                return HTTP_ERR_TLS;
            }
            // No CA bundle (dev flag): the build's ALTCP_MBEDTLS_AUTHMODE is VERIFY_REQUIRED, which
            // would fail every handshake without a chain. struct altcp_tls_config is private to
            // the SDK's altcp_tls_mbedtls.c, so the shared config is reached through the context
            // (MBEDTLS_ALLOW_PRIVATE_ACCESS); it is ours, created in http_client_init.
            if (!c->ca_pem) mbedtls_ssl_conf_authmode((mbedtls_ssl_config *)ssl->conf, MBEDTLS_SSL_VERIFY_NONE);
        }
    } else
#endif
    {
        c->pcb = altcp_tcp_new_ip_type(IPADDR_TYPE_ANY);
    }
    if (!c->pcb) { cyw43_arch_lwip_end(); return HTTP_ERR_CONNECT; }
    altcp_arg(c->pcb, c);
    altcp_recv(c->pcb, cb_recv);
    altcp_err(c->pcb, cb_err);
    altcp_poll(c->pcb, cb_poll, 2);   // 2 × 500 ms
    c->conn_https = c->https;
    strncpy(c->conn_host, c->host, sizeof c->conn_host - 1);
    c->conn_host[sizeof c->conn_host - 1] = 0;
    c->conn_port = c->port;
    c->dns_done = false;
    c->dns_ok = false;
    c->connected = false;
    c->pcb_gone = false;
    c->rx_closed = false;
    c->dns_gen++;
    err_t e = dns_gethostbyname(c->host, &c->addr, cb_dns, (void *)(uintptr_t)c->dns_gen);
    int rc = HTTP_OK;
    if (e == ERR_OK) {
        // cached (or a dotted quad): connect straight away
        c->cstate = CS_CONNECTING;
        if (altcp_connect(c->pcb, &c->addr, c->port, cb_connected) != ERR_OK) rc = HTTP_ERR_CONNECT;
    } else if (e == ERR_INPROGRESS) {
        c->cstate = CS_RESOLVING;
    } else {
        rc = HTTP_ERR_DNS;
    }
    cyw43_arch_lwip_end();
    if (rc != HTTP_OK) conn_close(c);
    c->last_activity_ms = now_ms();
    return rc;
}

// Issues the request on the existing idle connection if it matches, else opens a new one.
static int start_request(http_client_t *c) {
    reset_response_parser(c);
    c->last_activity_ms = now_ms();
    bool same = c->cstate == CS_OPEN && c->pcb && !c->pcb_gone && !c->rx_closed &&
                c->conn_https == c->https && c->conn_port == c->port && strcmp(c->conn_host, c->host) == 0;
    if (same) {
        c->reused = true;
        bool ok;
        cyw43_arch_lwip_begin();
        drop_rx_locked(c);   // stray bytes from the previous exchange belong to nobody
        ok = send_request_locked(c);
        cyw43_arch_lwip_end();
        if (ok) return HTTP_OK;
        // A write failure on a kept-alive socket usually means it died quietly; reconnect.
    }
    c->reused = false;
    return conn_open(c);
}

// ---------------------------------------------------------------------------------------------
// response parsing (main context, no lock held)
// ---------------------------------------------------------------------------------------------

static bool ieq(const char *a, size_t alen, const char *lit) {
    size_t n = strlen(lit);
    if (alen != n) return false;
    for (size_t i = 0; i < n; i++) {
        char x = a[i], y = lit[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return false;
    }
    return true;
}

// True if `tok` appears as a comma-separated token in value (case-insensitive).
static bool has_token(const char *v, size_t vlen, const char *tok) {
    size_t i = 0;
    while (i < vlen) {
        while (i < vlen && (v[i] == ' ' || v[i] == '\t' || v[i] == ',')) i++;
        size_t s = i;
        while (i < vlen && v[i] != ',' && v[i] != ' ' && v[i] != '\t') i++;
        if (i > s && ieq(v + s, i - s, tok)) return true;
    }
    return false;
}

static bool parse_u32(const char *s, size_t len, uint32_t *out) {
    if (len == 0) return false;
    uint32_t v = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        if (v > 0x0FFFFFFFu) return false;
        v = v * 10 + (uint32_t)(s[i] - '0');
    }
    *out = v;
    return true;
}

static bool is_redirect(int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

// Returns an HTTP_ERR_* on a malformed status line.
static int handle_status_line(http_client_t *c) {
    const char *l = c->line;
    size_t n = c->linelen;
    if (n == 0) return HTTP_OK;   // tolerate a stray CRLF before the status line (RFC 7230 §3.5)
    if (n < 12 || strncmp(l, "HTTP/1.", 7) != 0) return HTTP_ERR_PROTOCOL;
    bool http10 = l[7] == '0';
    if (l[8] != ' ') return HTTP_ERR_PROTOCOL;
    int st = 0;
    for (int i = 9; i < 12; i++) {
        if (l[i] < '0' || l[i] > '9') return HTTP_ERR_PROTOCOL;
        st = st * 10 + (l[i] - '0');
    }
    c->status = st;
    c->got_status = true;
    c->close_after = http10;   // HTTP/1.0 closes unless it says keep-alive
    c->rstate = RS_HEADERS;
    return HTTP_OK;
}

static void handle_header_line(http_client_t *c) {
    const char *l = c->line;
    size_t n = c->linelen;
    const char *colon = memchr(l, ':', n);
    if (!colon) return;   // not a header; ignore
    size_t nlen = (size_t)(colon - l);
    const char *v = colon + 1;
    size_t vlen = n - nlen - 1;
    while (vlen && (*v == ' ' || *v == '\t')) { v++; vlen--; }
    while (vlen && (v[vlen - 1] == ' ' || v[vlen - 1] == '\t')) vlen--;
    if (ieq(l, nlen, "content-length")) {
        uint32_t len;
        if (parse_u32(v, vlen, &len)) { c->have_length = true; c->remaining = len; }
    } else if (ieq(l, nlen, "transfer-encoding")) {
        if (has_token(v, vlen, "chunked")) c->chunked = true;
    } else if (ieq(l, nlen, "connection")) {
        if (has_token(v, vlen, "close")) c->close_after = true;
        else if (has_token(v, vlen, "keep-alive")) c->close_after = false;
    } else if (ieq(l, nlen, "location")) {
        if (vlen < sizeof c->location) { memcpy(c->location, v, vlen); c->location[vlen] = 0; c->have_location = true; }
    }
    if (!c->redirecting && c->sink && c->sink->on_header) c->sink->on_header(c->ctx, l, nlen, v, vlen);
}

// End of the header block: decide how the body is framed.
static void headers_done(http_client_t *c) {
    if (c->status >= 100 && c->status < 200) {   // 1xx: another status line follows
        reset_response_parser(c);
        return;
    }
    c->redirecting = is_redirect(c->status) && c->have_location;
    if (!c->redirecting && c->sink && c->sink->on_status) c->sink->on_status(c->ctx, c->status);
    // Headers were withheld while redirecting; nobody needs them. on_status came first so a
    // sink that keys on the status sees it before any header (the parser delivers headers as
    // they stream in for the non-redirect case only after this point... see note below).
    if (c->status == 304 || c->status == 204) {
        c->rstate = RS_DONE;
    } else if (c->chunked) {
        c->rstate = RS_CHUNK_SIZE;
    } else if (c->have_length) {
        c->rstate = c->remaining ? RS_BODY : RS_DONE;
    } else {
        c->rstate = RS_BODY;        // until close
        c->close_after = true;
    }
}

// Feeds `len` body bytes. Returns false if the sink aborted.
static bool body_out(http_client_t *c, const uint8_t *d, size_t len) {
    if (len == 0 || c->redirecting) return true;
    if (c->sink && c->sink->on_body) return c->sink->on_body(c->ctx, d, len);
    return true;
}

// Consumes bytes; returns bytes used, or sets *err (HTTP_ERR_*) on failure. Stops at RS_DONE.
static size_t parse_bytes(http_client_t *c, const uint8_t *d, size_t len, int *err) {
    size_t i = 0;
    *err = HTTP_OK;
    while (i < len && c->rstate != RS_DONE) {
        switch (c->rstate) {
        case RS_STATUS:
        case RS_HEADERS:
        case RS_CHUNK_SIZE:
        case RS_TRAILERS: {
            uint8_t ch = d[i++];
            if (ch == '\n') {
                // strip a trailing CR
                if (c->linelen && c->line[c->linelen - 1] == '\r') c->linelen--;
                if (c->rstate == RS_STATUS) {
                    if (c->line_overflow) { *err = HTTP_ERR_PROTOCOL; return i; }
                    *err = handle_status_line(c);
                    if (*err) return i;
                } else if (c->rstate == RS_HEADERS) {
                    if (c->linelen == 0) headers_done(c);
                    else if (!c->line_overflow) handle_header_line(c);
                    // an overlong header line is skipped: it cannot be one we need
                } else if (c->rstate == RS_CHUNK_SIZE) {
                    if (c->line_overflow) { *err = HTTP_ERR_PROTOCOL; return i; }
                    if (c->linelen == 0) { c->linelen = 0; break; }   // tolerate a blank line (some servers)
                    uint32_t sz = 0;
                    size_t k = 0;
                    for (; k < c->linelen; k++) {
                        char h = c->line[k];
                        int dv;
                        if (h >= '0' && h <= '9') dv = h - '0';
                        else if (h >= 'a' && h <= 'f') dv = h - 'a' + 10;
                        else if (h >= 'A' && h <= 'F') dv = h - 'A' + 10;
                        else break;
                        if (sz > 0x0FFFFFFFu) { *err = HTTP_ERR_PROTOCOL; return i; }
                        sz = (sz << 4) | (uint32_t)dv;
                    }
                    if (k == 0) { *err = HTTP_ERR_PROTOCOL; return i; }   // no hex digits (extensions after ';' are fine)
                    c->remaining = sz;
                    c->rstate = sz ? RS_CHUNK_DATA : RS_TRAILERS;
                } else {   // RS_TRAILERS
                    if (c->linelen == 0) c->rstate = RS_DONE;
                }
                c->linelen = 0;
                c->line_overflow = false;
            } else if (c->linelen < HTTP_LINE_MAX) {
                c->line[c->linelen++] = (char)ch;
            } else {
                c->line_overflow = true;
            }
            break;
        }
        case RS_BODY: {
            size_t avail = len - i;
            size_t take = c->have_length ? (avail < c->remaining ? avail : c->remaining) : avail;
            if (!body_out(c, d + i, take)) { *err = HTTP_ERR_ABORTED; return i + take; }
            i += take;
            if (c->have_length) {
                c->remaining -= (uint32_t)take;
                if (c->remaining == 0) c->rstate = RS_DONE;
            }
            break;
        }
        case RS_CHUNK_DATA: {
            size_t avail = len - i;
            size_t take = avail < c->remaining ? avail : c->remaining;
            if (!body_out(c, d + i, take)) { *err = HTTP_ERR_ABORTED; return i + take; }
            i += take;
            c->remaining -= (uint32_t)take;
            if (c->remaining == 0) c->rstate = RS_CHUNK_CRLF;
            break;
        }
        case RS_CHUNK_CRLF: {
            uint8_t ch = d[i++];
            if (ch == '\n') c->rstate = RS_CHUNK_SIZE;
            else if (ch != '\r') { *err = HTTP_ERR_PROTOCOL; return i; }
            break;
        }
        case RS_DONE:
            break;
        }
    }
    return i;
}

// The response is complete (RS_DONE, or EOF on a length-unknown body).
static void response_done(http_client_t *c) {
    bool close_conn = c->close_after;
    if (c->redirecting) {
        if (c->redirects >= HTTP_MAX_REDIRECTS) { fail(c, HTTP_ERR_TOO_MANY_REDIRECTS); return; }
        c->redirects++;
        char host[KIOSK_MAX_URL], path[KIOSK_MAX_URL];
        bool https; uint16_t port;
        if (!http_url_split(c->location, &https, host, sizeof host, &port, path, sizeof path)) {
            fail(c, HTTP_ERR_URL);   // relative or unsupported Location
            return;
        }
#if !KIOSK_TLS
        if (https) { fail(c, HTTP_ERR_URL); return; }
#endif
        if (close_conn) conn_close(c);
        c->https = https; c->port = port;
        memcpy(c->host, host, sizeof host);
        memcpy(c->path, path, sizeof path);
        c->retried = false;
        int rc = start_request(c);
        if (rc != HTTP_OK) fail(c, rc);
        return;
    }
    if (close_conn) conn_close(c);
    finish(c, HTTP_OK);
}

// ---------------------------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------------------------

void http_client_init(http_client_t *c, const char *ca_pem, size_t ca_len) {
    memset(c, 0, sizeof *c);
    c->ca_pem = ca_pem;
    c->ca_len = ca_len;
#if KIOSK_TLS
    cyw43_arch_lwip_begin();
    // ca_len must include the PEM's terminating NUL (mbedtls_x509_crt_parse's PEM rule).
    c->tls_conf = altcp_tls_create_config_client((const u8_t *)ca_pem, ca_pem ? ca_len : 0);
    cyw43_arch_lwip_end();
    if (!c->tls_conf) printf("http: TLS config failed (CA parse or no memory)\n");
    else if (!ca_pem) printf("http: WARNING no CA bundle, TLS peers are not verified\n");
#endif
}

int http_get(http_client_t *c, const char *url, const http_sink_t *sink, void *ctx, uint32_t timeout_ms) {
    if (c->busy) return HTTP_ERR_BUSY;
    bool https; uint16_t port;
    char host[KIOSK_MAX_URL], path[KIOSK_MAX_URL];
    if (!http_url_split(url, &https, host, sizeof host, &port, path, sizeof path)) return HTTP_ERR_URL;
#if !KIOSK_TLS
    if (https) return HTTP_ERR_URL;
#endif
    c->https = https; c->port = port;
    memcpy(c->host, host, sizeof host);
    memcpy(c->path, path, sizeof path);
    c->sink = sink; c->ctx = ctx;
    c->timeout_ms = timeout_ms ? timeout_ms : 45000;
    c->redirects = 0;
    c->retried = false;
    c->busy = true;
    c->deferred_err = 0;
    int rc = start_request(c);
    if (rc != HTTP_OK) {
        // Reported through on_complete like every other failure so the caller has one path — but
        // from the next http_poll, never from inside http_get: a caller that re-issues from
        // on_complete would otherwise recurse without bound on a persistent failure.
        conn_close(c);
        c->deferred_err = rc;
    }
    return HTTP_OK;
}

// http_cancel discards the request without calling on_complete: the caller is walking away from
// it (kiosk_loop_restart) and must not be re-entered from here.
void http_cancel(http_client_t *c) {
    conn_close(c);
    c->busy = false;
    c->sink = NULL;
    c->ctx = NULL;
}

bool http_busy(const http_client_t *c) { return c->busy; }

// A stale kept-alive connection closed under our request before answering: reconnect once.
static bool retry_on_fresh_connection(http_client_t *c) {
    if (!c->reused || c->retried || c->got_status) return false;
    c->retried = true;
    reset_response_parser(c);
    int rc = conn_open(c);
    if (rc != HTTP_OK) { fail(c, rc); }
    return true;
}

void http_poll(http_client_t *c) {
    uint32_t now = now_ms();

    // ---- connection setup phases ----
    if (c->cstate == CS_RESOLVING) {
        if (!c->busy) { conn_close(c); return; }
        if (c->dns_done) {
            if (!c->dns_ok) { fail(c, HTTP_ERR_DNS); return; }
            c->dns_done = false;
            err_t e;
            cyw43_arch_lwip_begin();
            c->cstate = CS_CONNECTING;
            e = altcp_connect(c->pcb, &c->addr, c->port, cb_connected);
            cyw43_arch_lwip_end();
            if (e != ERR_OK) { fail(c, HTTP_ERR_CONNECT); return; }
            c->last_activity_ms = now;
        } else if (now - c->last_activity_ms > HTTP_CONNECT_TIMEOUT_MS) {
            fail(c, HTTP_ERR_DNS);
        }
        return;
    }
    if (c->cstate == CS_CONNECTING) {
        if (!c->busy) { conn_close(c); return; }
        if (c->pcb_gone) {
            // TLS handshake failures arrive here as ERR_CLSD from altcp_tls_mbedtls.c
            int err = HTTP_ERR_CONNECT;
#if KIOSK_TLS
            if (c->https && c->gone_err == ERR_CLSD) err = HTTP_ERR_TLS;
#endif
            fail(c, err);
            return;
        }
        if (c->connected) {
            c->connected = false;
            c->cstate = CS_OPEN;
            c->last_activity_ms = now;
            bool ok;
            cyw43_arch_lwip_begin();
            ok = send_request_locked(c);
            cyw43_arch_lwip_end();
            if (!ok) fail(c, HTTP_ERR_CONNECT);
        } else if (now - c->last_activity_ms > HTTP_CONNECT_TIMEOUT_MS) {
            fail(c, HTTP_ERR_TIMEOUT);
        }
        return;
    }
    if (c->cstate != CS_OPEN) return;

    // ---- open connection: take what arrived ----
    struct pbuf *p;
    bool closed, gone;
    cyw43_arch_lwip_begin();
    p = c->rx; c->rx = NULL;
    closed = c->rx_closed;
    gone = c->pcb_gone;
    cyw43_arch_lwip_end();

    if (!c->busy) {
        // idle keep-alive connection: unsolicited bytes are dropped; a close is honoured
        if (p) { cyw43_arch_lwip_begin(); pbuf_free(p); cyw43_arch_lwip_end(); }
        if (closed || gone) conn_close(c);
        return;
    }

    if (p) {
        c->last_activity_ms = now;
        int err = HTTP_OK;
        u16_t tot = p->tot_len;
        for (struct pbuf *q = p; q && err == HTTP_OK && c->rstate != RS_DONE; q = q->next) {
            size_t used = 0;
            const uint8_t *d = (const uint8_t *)q->payload;
            while (used < q->len && err == HTTP_OK && c->rstate != RS_DONE)
                used += parse_bytes(c, d + used, q->len - used, &err);
        }
        // Acknowledge everything we took off the queue, even bytes past the end of the response:
        // they are discarded either way and the window must reopen.
        cyw43_arch_lwip_begin();
        if (c->pcb && !c->pcb_gone && !gone) altcp_recved(c->pcb, tot);
        pbuf_free(p);
        cyw43_arch_lwip_end();
        if (err != HTTP_OK) { fail(c, err); return; }
        if (c->rstate == RS_DONE) { response_done(c); return; }
    }

    if (closed || gone) {
        // Peer finished (FIN) or the pcb died (RST/abort) before we saw the end of the response.
        if (c->rstate == RS_BODY && !c->have_length && closed) {
            // length-unknown body: EOF is the terminator
            c->rstate = RS_DONE;
            c->close_after = true;
            response_done(c);
            return;
        }
        if (retry_on_fresh_connection(c)) return;
        fail(c, gone ? (c->got_status ? HTTP_ERR_CLOSED : HTTP_ERR_CONNECT) : HTTP_ERR_CLOSED);
        return;
    }

    if (now - c->last_activity_ms > c->timeout_ms) fail(c, HTTP_ERR_TIMEOUT);
}
