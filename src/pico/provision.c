// Wi-Fi provisioning: the captive-portal HTTP server (lwIP raw TCP) and the USB serial console.
//
// Contexts: lwIP callbacks run in the cyw43 background context (a low-priority IRQ on core 0,
// holding the async-context lock); provision_poll/console_poll run in main context. Anything that
// writes flash (config_save, ~50 ms with interrupts off) or starts a driver operation (a scan) is
// therefore deferred from the callbacks to provision_poll through small flag/staging variables,
// and main context reads the shared scan table under cyw43_arch_lwip_begin/end.
//
// The request parser, the form decoder, the response builder and the console command interpreter
// are plain C so host/tests/test_provision.c can compile this file with PROVISION_HOST_TEST and
// drive them without lwIP or the SDK; the transport glue at the bottom is device-only.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "provision.h"
#ifndef PROVISION_HOST_TEST
#include "ddc.h"
#endif
#include "flash_store.h"
#include "kiosk_config.h"
#include "builtin_frames.h"
#include "kiosk_loop.h"
#include "net_wifi.h"
#include "portal_html.h"

#ifdef PROVISION_HOST_TEST
// The test defines video_mode_t / video_mode_info / video_mode_from_name (board.h drags in libdvi),
// the fake clock, and hooks for the device-only actions.
extern uint32_t host_now_ms;
void host_reboot_hook(void);
void host_scan_start_hook(void);
static uint32_t provision_now_ms(void) { return host_now_ms; }
static uint32_t free_heap(void) { return 12345; }
#else
#include "board.h"
#include "scanout.h"
#include "pico/cyw43_arch.h"
#include "pico/stdio.h"
#include "pico/time.h"
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"
#include "lwip/tcp.h"
#include "lwip/pbuf.h"
#include <malloc.h>
static uint32_t provision_now_ms(void) { return to_ms_since_boot(get_absolute_time()); }
// newlib's sbrk heap starts at __end__ and may grow to __StackLimit (memmap_default.ld).
extern char __StackLimit, __end__;
static uint32_t free_heap(void) {
    struct mallinfo mi = mallinfo();
    return (uint32_t)(&__StackLimit - &__end__) - (uint32_t)mi.uordblks;
}
#endif

#define PORTAL_IP "192.168.4.1"
#define PORTAL_URL "http://" PORTAL_IP "/"
#define PORTAL_MAX_CONN 2    // a phone opens one or two connections to the portal; RAM is tight        // phones open the page, /config.json and /scan concurrently
#define PORTAL_BUF 2048          // request head + body; reused for a dynamic response body
#define PORTAL_HDR 192
#define PORTAL_MAX_NETS 16
#define PORTAL_IDLE_POLLS 10     // tcp_poll ticks of 1 s before an idle connection is dropped
#define SCAN_MIN_INTERVAL_MS 8000u
#define REBOOT_GRACE_MS 2000u    // after a portal save: wait for the response to drain, then reboot

// ---- Small string helpers -------------------------------------------------------------------

static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Case-insensitive header-name compare (strncasecmp is POSIX, not C11).
static bool name_is(const char *s, size_t n, const char *name) {
    if (strlen(name) != n) return false;
    for (size_t i = 0; i < n; i++) {
        char a = s[i], b = name[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b) return false;
    }
    return true;
}

static void copy_bounded(char *dst, size_t cap, const char *src, size_t n) {
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

// Decodes application/x-www-form-urlencoded text ('+' → space, %XX). Returns the decoded length,
// or -1 when the result would not fit in cap (including the NUL) — callers reject rather than
// silently truncate a password or URL. A malformed %XX is kept literally, as browsers do.
static int url_decode(const char *src, size_t n, char *dst, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        char c = src[i];
        if (c == '+') {
            c = ' ';
        } else if (c == '%' && i + 2 < n) {
            int h = hex_val(src[i + 1]), l = hex_val(src[i + 2]);
            if (h >= 0 && l >= 0) {
                c = (char)((h << 4) | l);
                i += 2;
            }
        }
        if (o + 1 >= cap) return -1;
        dst[o++] = c;
    }
    dst[o] = 0;
    return (int)o;
}

// Finds `name=` in a form body and decodes its value. Returns 1 found, 0 absent, -1 too long.
static int form_field(const char *body, size_t blen, const char *name, char *out, size_t cap) {
    size_t nlen = strlen(name);
    size_t i = 0;
    out[0] = 0;
    while (i < blen) {
        size_t end = i;
        while (end < blen && body[end] != '&') end++;
        size_t eq = i;
        while (eq < end && body[eq] != '=') eq++;
        if (eq - i == nlen && memcmp(body + i, name, nlen) == 0) {
            size_t vstart = eq < end ? eq + 1 : end;
            return url_decode(body + vstart, end - vstart, out, cap) < 0 ? -1 : 1;
        }
        i = end + 1;
    }
    return 0;
}

// Appends a JSON string literal (with quotes) to buf; returns false when it does not fit.
static bool json_append_str(char *buf, size_t cap, size_t *len, const char *s, size_t smax) {
    static const char hexd[] = "0123456789abcdef";
    size_t o = *len;
    if (o + 2 >= cap) return false;
    buf[o++] = '"';
    for (size_t i = 0; i < smax && s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') {
            if (o + 2 >= cap) return false;
            buf[o++] = '\\';
            buf[o++] = (char)c;
        } else if (c < 0x20) {
            if (o + 6 >= cap) return false;
            buf[o++] = '\\'; buf[o++] = 'u'; buf[o++] = '0'; buf[o++] = '0';
            buf[o++] = hexd[c >> 4]; buf[o++] = hexd[c & 15];
        } else {
            if (o + 1 >= cap) return false;
            buf[o++] = (char)c;
        }
    }
    if (o + 1 >= cap) return false;
    buf[o++] = '"';
    buf[o] = 0;
    *len = o;
    return true;
}

static bool json_append(char *buf, size_t cap, size_t *len, const char *s) {
    size_t n = strlen(s);
    if (*len + n + 1 > cap) return false;
    memcpy(buf + *len, s, n + 1);
    *len += n;
    return true;
}

static void strip_trailing_slashes(char *s) {
    size_t n = strlen(s);
    while (n > 0 && s[n - 1] == '/') s[--n] = 0;
}

static bool valid_server_url(const char *s) {
    if (strncmp(s, "http://", 7) == 0) return s[7] != 0 && s[7] != '/';
    if (strncmp(s, "https://", 8) == 0) return s[8] != 0 && s[8] != '/';
    return false;
}

// The token, id and cached URLs belong to one server; pointing elsewhere means registering anew.
static void forget_registration(void) {
    kiosk_config.token[0] = 0;
    kiosk_config.device_id[0] = 0;
    kiosk_config.next_url[0] = 0;
    kiosk_config.static_id[0] = 0;
}

// Applies a new server base; returns true if it changed (registration then forgotten).
static bool set_server_base(const char *url) {
    char tmp[KIOSK_MAX_URL];
    copy_bounded(tmp, sizeof tmp, url, strlen(url));
    strip_trailing_slashes(tmp);
    if (strcmp(tmp, kiosk_config.server_base) == 0) return false;
    memcpy(kiosk_config.server_base, tmp, sizeof tmp);
    forget_registration();
    return true;
}

static const char *wifi_state_name(wifi_state_t s) {
    switch (s) {
    case WIFI_DOWN: return "down";
    case WIFI_CONNECTING: return "connecting";
    case WIFI_UP: return "up";
    case WIFI_FAILED: return "failed";
    case WIFI_AP: return "access point";
    default: return "?";
    }
}

// ---- Shared state ---------------------------------------------------------------------------

typedef struct {
    char ssid[33];
    int16_t rssi;
    bool open;
} net_entry_t;

static bool active;
static char ap_ssid[16];
static net_entry_t nets[PORTAL_MAX_NETS];
static uint8_t nets_n;
static volatile bool scan_requested;     // set by GET /scan (callback), consumed by provision_poll
static uint32_t scan_started_ms;
static bool scan_ever;

// A portal POST is staged here by the lwIP callback and applied + saved by provision_poll.
typedef struct {
    char ssid[33];
    char pass[65];
    char server[KIOSK_MAX_URL];
    char mode[16];
} pending_save_t;
static pending_save_t pending;
static volatile bool save_requested;
static uint32_t reboot_at_ms;            // 0 = no reboot scheduled

// Merges one scan result: dedupe on SSID keeping the strongest, drop hidden networks, and when
// the table is full replace the weakest entry if this one is stronger.
static void nets_add(const uint8_t *ssid, size_t ssid_len, int16_t rssi, bool open) {
    if (ssid_len == 0 || ssid_len > 32) return;
    for (unsigned i = 0; i < nets_n; i++) {
        if (strlen(nets[i].ssid) == ssid_len && memcmp(nets[i].ssid, ssid, ssid_len) == 0) {
            if (rssi > nets[i].rssi) { nets[i].rssi = rssi; nets[i].open = open; }
            return;
        }
    }
    unsigned slot = nets_n;
    if (nets_n >= PORTAL_MAX_NETS) {
        slot = 0;
        for (unsigned i = 1; i < nets_n; i++)
            if (nets[i].rssi < nets[slot].rssi) slot = i;
        if (nets[slot].rssi >= rssi) return;
    } else {
        nets_n++;
    }
    memcpy(nets[slot].ssid, ssid, ssid_len);
    nets[slot].ssid[ssid_len] = 0;
    nets[slot].rssi = rssi;
    nets[slot].open = open;
}

// ---- HTTP: connection state, request parsing, response building ----------------------------

typedef struct {
    struct tcp_pcb *pcb;
    char buf[PORTAL_BUF];         // request; after handling, a dynamic response body
    uint16_t len;
    uint16_t head_len;            // 0 until "\r\n\r\n" has arrived
    uint16_t body_len;            // Content-Length
    char hdr[PORTAL_HDR];
    uint16_t hdr_len;
    const char *body;
    uint16_t body_total;
    bool responding;
    bool reboot_after;
    uint32_t sent;                // bytes handed to tcp_write (header + body)
    uint32_t acked;
    uint8_t idle_polls;
} conn_t;

typedef struct {
    const char *method; size_t method_len;
    const char *path; size_t path_len;
    const char *host; size_t host_len;   // absent → host_len 0
    size_t content_length;
    bool bad;
} request_t;

enum { FEED_MORE, FEED_RESPOND, FEED_ABORT };

// Parses the request line and the two headers we care about from a complete head.
static void parse_request(const char *head, size_t len, request_t *r) {
    memset(r, 0, sizeof *r);
    size_t i = 0;
    r->method = head;
    while (i < len && head[i] != ' ' && head[i] != '\r') i++;
    r->method_len = i;
    if (i >= len || head[i] != ' ' || i == 0) { r->bad = true; return; }
    i++;
    r->path = head + i;
    while (i < len && head[i] != ' ' && head[i] != '\r') i++;
    r->path_len = (size_t)(head + i - r->path);
    if (r->path_len == 0 || r->path[0] != '/') { r->bad = true; return; }
    // Strip the query: the probes and our routes never need it.
    for (size_t q = 0; q < r->path_len; q++)
        if (r->path[q] == '?') { r->path_len = q; break; }
    while (i < len && head[i] != '\n') i++;
    if (i < len) i++;
    // Headers, one per line, case-insensitive names.
    while (i < len) {
        size_t ls = i;
        while (i < len && head[i] != '\n') i++;
        size_t le = i;
        if (i < len) i++;
        if (le > ls && head[le - 1] == '\r') le--;
        if (le == ls) break;   // blank line
        size_t c = ls;
        while (c < le && head[c] != ':') c++;
        if (c == le) continue;
        size_t nlen = c - ls;
        size_t v = c + 1;
        while (v < le && is_space(head[v])) v++;
        size_t ve = le;
        while (ve > v && is_space(head[ve - 1])) ve--;
        if (name_is(head + ls, nlen, "Host")) {
            r->host = head + v;
            r->host_len = ve - v;
        } else if (name_is(head + ls, nlen, "Content-Length")) {
            size_t n = 0;
            for (size_t k = v; k < ve; k++) {
                if (head[k] < '0' || head[k] > '9' || n > 100000) { r->bad = true; return; }
                n = n * 10 + (size_t)(head[k] - '0');
            }
            r->content_length = n;
        }
    }
}

static bool path_is(const request_t *r, const char *p) {
    size_t n = strlen(p);
    return r->path_len == n && memcmp(r->path, p, n) == 0;
}

static bool host_is_ours(const request_t *r) {
    if (r->host_len == 0) return true;   // HTTP/1.0 probes
    static const char a[] = PORTAL_IP, b[] = PORTAL_IP ":80";
    return (r->host_len == sizeof a - 1 && memcmp(r->host, a, sizeof a - 1) == 0) ||
           (r->host_len == sizeof b - 1 && memcmp(r->host, b, sizeof b - 1) == 0);
}

static void set_response(conn_t *c, int status, const char *reason, const char *ctype,
                         const char *body, size_t blen, const char *extra) {
    int n = snprintf(c->hdr, sizeof c->hdr,
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
                     "Cache-Control: no-store\r\nConnection: close\r\n%s\r\n",
                     status, reason, ctype, (unsigned)blen, extra ? extra : "");
    if (n < 0 || (size_t)n >= sizeof c->hdr) n = (int)sizeof c->hdr - 1;   // never: sized for the longest
    c->hdr_len = (uint16_t)n;
    c->body = body;
    c->body_total = (uint16_t)blen;
    c->responding = true;
    c->sent = 0;
    c->acked = 0;
}

static void respond_text(conn_t *c, int status, const char *reason, const char *text) {
    set_response(c, status, reason, "text/plain; charset=utf-8", text, strlen(text), NULL);
}

static void respond_redirect(conn_t *c) {
    static const char body[] = "Redirecting to " PORTAL_URL "\n";
    set_response(c, 302, "Found", "text/plain", body, sizeof body - 1, "Location: " PORTAL_URL "\r\n");
}

// The dynamic bodies are rendered into c->buf, which is free once the request has been parsed
// (the request_t pointers into it are dead by then).
static void respond_config_json(conn_t *c) {
    size_t len = 0;
    char *b = c->buf;
    bool ok = json_append(b, sizeof c->buf, &len, "{\"ssid\":")
        && json_append_str(b, sizeof c->buf, &len, kiosk_config.wifi_ssid, sizeof kiosk_config.wifi_ssid)
        && json_append(b, sizeof c->buf, &len, ",\"server\":")
        && json_append_str(b, sizeof c->buf, &len, kiosk_config.server_base, sizeof kiosk_config.server_base)
        && json_append(b, sizeof c->buf, &len, ",\"mode\":")
        && json_append_str(b, sizeof c->buf, &len, video_mode_info((video_mode_t)kiosk_config.video_mode)->name, 16)
        && json_append(b, sizeof c->buf, &len, ",\"ap\":")
        && json_append_str(b, sizeof c->buf, &len, ap_ssid, sizeof ap_ssid)
        && json_append(b, sizeof c->buf, &len, ",\"fw\":\"" KIOSK_FW_VERSION "\",\"registered\":")
        && json_append(b, sizeof c->buf, &len, kiosk_config.token[0] ? "true}" : "false}");
    if (!ok) { respond_text(c, 500, "Internal Server Error", "too long"); return; }
    set_response(c, 200, "OK", "application/json", b, len, NULL);
}

static void respond_scan_json(conn_t *c) {
    size_t len = 0;
    char *b = c->buf;
    bool ok = json_append(b, sizeof c->buf, &len, "[");
    for (unsigned i = 0; ok && i < nets_n; i++) {
        char num[40];
        snprintf(num, sizeof num, "%s{\"rssi\":%d,\"open\":%s,\"ssid\":", i ? "," : "",
                 (int)nets[i].rssi, nets[i].open ? "true" : "false");
        ok = json_append(b, sizeof c->buf, &len, num)
          && json_append_str(b, sizeof c->buf, &len, nets[i].ssid, sizeof nets[i].ssid)
          && json_append(b, sizeof c->buf, &len, "}");
    }
    ok = ok && json_append(b, sizeof c->buf, &len, "]");
    if (!ok) { respond_text(c, 500, "Internal Server Error", "too long"); return; }
    scan_requested = true;
    set_response(c, 200, "OK", "application/json", b, len, NULL);
}

// Validates the form, stages it for provision_poll and answers. The save itself is not done here:
// this runs in the lwIP callback context and flash programming belongs to main context.
static void handle_save(conn_t *c, const char *body, size_t blen) {
    pending_save_t p;
    memset(&p, 0, sizeof p);
    int fs = form_field(body, blen, "ssid", p.ssid, sizeof p.ssid);
    int fp = form_field(body, blen, "pass", p.pass, sizeof p.pass);
    int fv = form_field(body, blen, "server", p.server, sizeof p.server);
    int fm = form_field(body, blen, "mode", p.mode, sizeof p.mode);
    if (fs < 0) { respond_text(c, 400, "Bad Request", "The network name is too long (32 characters at most)."); return; }
    if (fp < 0) { respond_text(c, 400, "Bad Request", "The password is too long (64 characters at most)."); return; }
    if (fv < 0) { respond_text(c, 400, "Bad Request", "The server URL is too long."); return; }
    if (fm < 0) { respond_text(c, 400, "Bad Request", "Unknown video mode."); return; }
    if (fs == 0 || p.ssid[0] == 0) { respond_text(c, 400, "Bad Request", "Enter the Wi-Fi network name."); return; }
    size_t pl = strlen(p.pass);
    if (pl != 0 && pl < 8) { respond_text(c, 400, "Bad Request", "A WPA2 password has at least 8 characters (leave it empty for an open network)."); return; }
    if (p.server[0]) {
        strip_trailing_slashes(p.server);
        if (!valid_server_url(p.server)) { respond_text(c, 400, "Bad Request", "The server URL must start with http:// or https://."); return; }
    }
    if (p.mode[0] && strcmp(video_mode_info(video_mode_from_name(p.mode))->name, p.mode) != 0) {
        respond_text(c, 400, "Bad Request", "Unknown video mode.");
        return;
    }
    if (save_requested) { respond_text(c, 409, "Conflict", "Already saving; the kiosk is rebooting."); return; }
    pending = p;
    save_requested = true;
    c->reboot_after = true;
    respond_text(c, 200, "OK", "Saved - the kiosk is rebooting. Reconnect your phone to your own Wi-Fi.");
}

static void handle_request(conn_t *c) {
    request_t r;
    parse_request(c->buf, c->head_len, &r);
    if (r.bad) { respond_text(c, 400, "Bad Request", "bad request"); return; }
    bool get = r.method_len == 3 && memcmp(r.method, "GET", 3) == 0;
    bool post = r.method_len == 4 && memcmp(r.method, "POST", 4) == 0;
    if (!host_is_ours(&r)) { respond_redirect(c); return; }
    if (get && (path_is(&r, "/") || path_is(&r, "/index.html"))) {
        set_response(c, 200, "OK", "text/html; charset=utf-8", portal_html, sizeof portal_html - 1, NULL);
    } else if (get && path_is(&r, "/config.json")) {
        respond_config_json(c);
    } else if (get && path_is(&r, "/scan")) {
        respond_scan_json(c);
    } else if (post && path_is(&r, "/save")) {
        handle_save(c, c->buf + c->head_len, c->body_len);
    } else if (get && (path_is(&r, "/generate_204") || path_is(&r, "/gen_204") ||
                       path_is(&r, "/hotspot-detect.html") || path_is(&r, "/library/test/success.html") ||
                       path_is(&r, "/connecttest.txt") || path_is(&r, "/ncsi.txt") ||
                       path_is(&r, "/redirect") || path_is(&r, "/canonical.html") ||
                       path_is(&r, "/success.txt") || path_is(&r, "/check_network_status.txt"))) {
        respond_redirect(c);
    } else {
        respond_text(c, 404, "Not Found", "not found");
    }
}

// Accumulates request bytes; returns FEED_RESPOND once a response is ready to be pumped.
static int conn_feed(conn_t *c, const uint8_t *data, size_t n) {
    if (c->responding) return FEED_MORE;   // late bytes after we answered: ignore
    if (n > sizeof c->buf - 1 - c->len) {
        // A head or body we cannot hold: answer what we can and drop the rest.
        bool mid_head = c->head_len == 0 && c->len > 0;
        c->len = 0;
        if (mid_head) respond_text(c, 431, "Request Header Fields Too Large", "headers too large");
        else respond_text(c, 413, "Payload Too Large", "request too large");
        return FEED_RESPOND;
    }
    memcpy(c->buf + c->len, data, n);
    c->len = (uint16_t)(c->len + n);
    c->buf[c->len] = 0;
    if (c->head_len == 0) {
        const char *end = strstr(c->buf, "\r\n\r\n");
        if (!end) {
            if (c->len >= sizeof c->buf - 1) {
                respond_text(c, 431, "Request Header Fields Too Large", "headers too large");
                return FEED_RESPOND;
            }
            return FEED_MORE;
        }
        c->head_len = (uint16_t)(end + 4 - c->buf);
        request_t r;
        parse_request(c->buf, c->head_len, &r);
        if (r.bad) { respond_text(c, 400, "Bad Request", "bad request"); return FEED_RESPOND; }
        if (r.content_length > sizeof c->buf - 1 - c->head_len) {
            respond_text(c, 413, "Payload Too Large", "request too large");
            return FEED_RESPOND;
        }
        c->body_len = (uint16_t)r.content_length;
    }
    if (c->len < c->head_len + c->body_len) return FEED_MORE;
    handle_request(c);
    return FEED_RESPOND;
}

// ---- Console ---------------------------------------------------------------------------------

// Splits a command line in place on blanks; a "double-quoted" token may contain blanks.
static int split_args(char *s, char *argv[], int max) {
    int n = 0;
    while (*s && n < max) {
        while (*s && is_space(*s)) s++;
        if (!*s) break;
        if (*s == '"') {
            s++;
            argv[n++] = s;
            while (*s && *s != '"') s++;
            if (*s) *s++ = 0;
        } else {
            argv[n++] = s;
            while (*s && !is_space(*s)) s++;
            if (*s) *s++ = 0;
        }
    }
    return n;
}

static void do_reboot(const char *why) {
#ifndef PROVISION_HOST_TEST
    // watchdog_reboot restarts its countdown on every call, and the provisioning save asks again on
    // every poll until the reboot happens: re-arming each time postponed the reboot forever and left
    // the kiosk on its setup screen. Arm it exactly once.
    static bool armed;
    if (armed) return;
    armed = true;
#endif
    printf("rebooting: %s\n", why);
#ifdef PROVISION_HOST_TEST
    host_reboot_hook();
#else
    stdio_flush();
    watchdog_reboot(0, 0, 200);
    // Returning would let the main loop feed the watchdog, which reloads its normal 8 s timeout and
    // cancels the reboot: the kiosk then sat on its setup screen with the new settings saved. Wait here
    // for the reset instead; video keeps running on core 1 until it happens.
    for (;;) tight_loop_contents();
#endif
}

static void print_status(void) {
    char buf[256];
    kiosk_loop_status(buf, sizeof buf);
    printf("%s\n", buf);
    char ip[16];
    wifi_state_t ws = net_wifi_state();
    printf("wifi: %s", wifi_state_name(ws));
    if (ws == WIFI_UP) printf(", ip %s, rssi %d dBm", net_wifi_ip(ip, sizeof ip), net_wifi_rssi());
    if (ws == WIFI_AP) printf(" %s at " PORTAL_IP, ap_ssid);
    printf("\nssid: %s%s\nserver: %s\nmode: %s\nregistered: %s%s%s\nfw: %s\nheap free: %u bytes\nuptime: %u s\n",
           kiosk_config.wifi_ssid, kiosk_config.wifi_ssid[0] ? "" : "(none - use `wifi <ssid> <password>`)",
           kiosk_config.server_base, video_mode_info((video_mode_t)kiosk_config.video_mode)->name,
           kiosk_config.token[0] ? "yes, id " : "no", kiosk_config.token[0] ? kiosk_config.device_id : "",
           kiosk_config.next_url[0] ? " (next_url cached)" : "",
           KIOSK_FW_VERSION, (unsigned)free_heap(), (unsigned)(provision_now_ms() / 1000u));
}

#ifndef KIOSK_MODE_NAMES   // host tests compile this file without board.h
#define KIOSK_MODE_NAMES "720p30|720p30rb|480p60|720x480p60|960x540p60|1066x600p50"
#endif

static void print_help(void) {
    printf("commands:\n"
           "  help                     this list\n"
           "  status                   loop state, wifi, config, heap\n"
           "  stats                    render/scanout counters\n"
           "  wifi <ssid> [password]   join a network (quote an ssid with spaces; no password = open)\n"
           "  server <url>             kiosk server base, e.g. server http://192.168.1.10:8080\n"
           "  mode <" KIOSK_MODE_NAMES ">   video mode (reboots)\n"
           "  scan                     list nearby networks\n"
           "  reset                    forget the registration token (re-pair), keep wifi\n"
           "  factory                  forget everything and reboot into provisioning\n"
           "  reboot\n"
           "  test                     show the test pattern\n"
           "diagnostics:\n"
           "  stats                    also prints wifi rssi, channel, and whether DVI is silenced\n"
           "  edid                     read the monitor's EDID over DDC (decode with tools/edid_decode.py)\n"
           "  label <text> | solid     draw a labelled or plain test frame\n"
           "  bench                    time the scanline expander\n"
           "  pads <2|4|8|12> [fast]   TMDS pad drive and slew\n"
           "  smps pwm|save            3V3 regulator mode\n"
           "  sigsweep                 cycle pad and regulator settings, 20 s each\n"
           "  tmds off|on              silence the DVI pins (Wi-Fi interference test); kept across resets\n"
           "  norender [off]           decode frames without drawing them; kept across resets\n"
           "the video clock follows the Wi-Fi channel so DVI harmonics stay out of it; the kiosk\n"
           "reboots once when it joins a channel that needs a different clock\n");
}

static void apply_and_restart(void) {
    if (!config_save()) printf("warning: config verify failed\n");
    if (active) provision_stop();
    kiosk_loop_restart();
}

static void console_exec(char *line) {
    char *argv[4];
    int argc = split_args(line, argv, 4);
    if (argc == 0) return;
    const char *cmd = argv[0];
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        print_help();
    } else if (strcmp(cmd, "status") == 0) {
        print_status();
    } else if (strcmp(cmd, "stats") == 0) {
        char buf[256];
        kiosk_loop_status(buf, sizeof buf);
        printf("%s\n", buf);
#ifndef PROVISION_HOST_TEST
        scanout_profile_t sp;
        scanout_profile(&sp);
        printf("scanout: %s expander, %u frames, red (missed) lines %u, dropped late lines %u since boot\n"
               "scanout: worst line %u cycles (%u span bytes, y=%u), worst IRQ %u cycles, line budget %u cycles, %u line buffers, %u lines over budget since boot\n",
               scanout_expander_name(), (unsigned)sp.frames, (unsigned)sp.missed_lines, (unsigned)sp.dropped_lines,
               (unsigned)sp.worst_line_cycles, (unsigned)sp.worst_line_span_bytes, (unsigned)sp.worst_line_y,
               (unsigned)sp.irq_max_cycles, (unsigned)sp.line_budget_cycles, (unsigned)sp.tmds_buffers, (unsigned)sp.over_budget_lines);
#endif
        printf("heap free: %u bytes, uptime %u s\n", (unsigned)free_heap(), (unsigned)(provision_now_ms() / 1000u));
#ifndef PROVISION_HOST_TEST
        printf("wifi rssi: %d dBm, channel %d, tmds %s\n", net_wifi_rssi(), net_wifi_channel(), watchdog_hw->scratch[3] == 0x544d4f46u ? "off" : "on");
#endif
#ifndef PROVISION_HOST_TEST
    } else if (strcmp(cmd, "bench") == 0) {
        scanout_bench();
    } else if (strcmp(cmd, "pads") == 0) {
        int ma = argc >= 2 ? atoi(argv[1]) : 0;
        uint8_t code = ma >= 12 ? 3 : ma >= 8 ? 2 : ma >= 4 ? 1 : 0;
        bool fast = argc >= 3 && strcmp(argv[2], "fast") == 0;
        scanout_set_pads(code, fast);
        printf("TMDS pads: %u mA, %s slew\n", 2u << code > 8 ? 12u : 2u << code, fast ? "fast" : "slow");
    } else if (strcmp(cmd, "smps") == 0) {
        bool pwm = argc >= 2 && strcmp(argv[1], "pwm") == 0;
        net_wifi_smps_pwm(pwm);
        printf("3V3 regulator: %s mode\n", pwm ? "PWM (low ripple)" : "power-save");
    } else if (strcmp(cmd, "sigsweep") == 0) {
        static const struct { uint8_t drive; bool fast, pwm; const char *label; } steps[] = {
            { 0, false, false, "1: 2mA slow" },
            { 3, true,  false, "2: 12mA fast" },
            { 2, true,  false, "3: 8mA fast" },
            { 1, true,  false, "4: 4mA fast" },
            { 3, false, false, "5: 12mA slow" },
            { 3, true,  true,  "6: 12mA fast PWM" },
            { 0, false, true,  "7: 2mA slow PWM" },
        };
        const unsigned n = sizeof steps / sizeof steps[0];
        for (unsigned i = 0; i < n; i++) {
            scanout_set_pads(steps[i].drive, steps[i].fast);
            net_wifi_smps_pwm(steps[i].pwm);
            kiosk_show_builtin(BUILTIN_LABEL, steps[i].label, NULL);
            printf("[sweep %u/%u] %s\n", i + 1, n, steps[i].label);
            for (int t = 0; t < 200; t++) { sleep_ms(100); watchdog_update(); }
        }
        scanout_set_pads(0, false);
        net_wifi_smps_pwm(false);
        kiosk_show_builtin(BUILTIN_LABEL, "sweep done", NULL);
        printf("[sweep] done; restored 2 mA slow, power-save\n");
    } else if (strcmp(cmd, "edid") == 0) {
        // Raw blocks for tools/edid_decode.py; the first line says whether the display answered.
        uint8_t e[128];
        if (!ddc_read_edid(0, e)) { printf("edid: no answer from the display on DDC\n"); return; }
        printf("EDIDHEX0 ");
        for (int i = 0; i < 128; i++) printf("%02x", e[i]);
        printf("\n");
        for (uint8_t b = 1; b <= e[126] && b <= 3; b++) {
            uint8_t x[128];
            if (!ddc_read_edid(b, x)) { printf("edid: extension block %u read failed\n", b); break; }
            printf("EDIDHEX%u ", b);
            for (int i = 0; i < 128; i++) printf("%02x", x[i]);
            printf("\n");
        }
    } else if (strcmp(cmd, "label") == 0) {
        const char *text = argc >= 2 ? argv[1] : "";
        kiosk_show_builtin(BUILTIN_LABEL, text, NULL);
        printf("label: %s\n", text);
    } else if (strcmp(cmd, "tmds") == 0) {
        bool on = argc >= 2 && strcmp(argv[1], "on") == 0;
        watchdog_hw->scratch[3] = on ? 0u : 0x544d4f46u;
        scanout_set_tmds_enabled(on);
        printf("tmds %s: DVI output %s (kept across watchdog resets)\n", on ? "on" : "off", on ? "restored" : "silenced, pins held low");
    } else if (strcmp(cmd, "norender") == 0) {
        bool on = !(argc >= 2 && strcmp(argv[1], "off") == 0);
        watchdog_hw->scratch[2] = on ? 0x4e4f5244u : 0u;
        printf("norender %s: received frames are %s (kept across watchdog resets)\n", on ? "on" : "off", on ? "decoded but not drawn" : "drawn");
    } else if (strcmp(cmd, "solid") == 0) {
        if (!kiosk_show_builtin(BUILTIN_SOLID, NULL, NULL)) printf("error: could not draw the solid frame\n");
        else printf("drew blue / white / green bands\n");
#endif
    } else if (strcmp(cmd, "wifi") == 0) {
        if (argc < 2 || !argv[1][0]) { printf("usage: wifi <ssid> [password]\n"); return; }
        const char *ssid = argv[1], *pass = argc >= 3 ? argv[2] : "";
        if (strlen(ssid) > 32) { printf("error: ssid longer than 32 characters\n"); return; }
        size_t pl = strlen(pass);
        if (pl > 64) { printf("error: password longer than 64 characters\n"); return; }
        if (pl != 0 && pl < 8) { printf("error: a WPA2 password has at least 8 characters\n"); return; }
        copy_bounded(kiosk_config.wifi_ssid, sizeof kiosk_config.wifi_ssid, ssid, strlen(ssid));
        copy_bounded(kiosk_config.wifi_pass, sizeof kiosk_config.wifi_pass, pass, pl);
        printf("wifi: joining \"%s\" (%s)\n", ssid, pl ? "WPA2" : "open");
        apply_and_restart();
    } else if (strcmp(cmd, "server") == 0) {
        if (argc < 2) { printf("usage: server <http://host[:port]>\n"); return; }
        if (strlen(argv[1]) >= KIOSK_MAX_URL) { printf("error: URL too long\n"); return; }
        if (!valid_server_url(argv[1])) { printf("error: the URL must start with http:// or https://\n"); return; }
        bool changed = set_server_base(argv[1]);
        printf("server: %s%s\n", kiosk_config.server_base, changed ? " (registration forgotten; the kiosk will register again)" : " (unchanged)");
        apply_and_restart();
    } else if (strcmp(cmd, "mode") == 0) {
        if (argc < 2) { printf("usage: mode <" KIOSK_MODE_NAMES ">\n"); return; }
        video_mode_t m = video_mode_from_name(argv[1]);
        if (strcmp(video_mode_info(m)->name, argv[1]) != 0) { printf("error: unknown mode\n"); return; }
        kiosk_config.video_mode = (uint8_t)m;
        if (!config_save()) printf("warning: config verify failed\n");
        printf("mode: %s - the clocks are set at boot, rebooting to apply\n", argv[1]);
        do_reboot("video mode changed");
    } else if (strcmp(cmd, "scan") == 0) {
        scan_requested = true;
        printf("%u network(s) known%s:\n", (unsigned)nets_n, scan_ever ? "" : " (scan starting; run again in a few seconds)");
        for (unsigned i = 0; i < nets_n; i++)
            printf("  %4d dBm  %s%s\n", (int)nets[i].rssi, nets[i].ssid, nets[i].open ? "  (open)" : "");
    } else if (strcmp(cmd, "reset") == 0) {
        forget_registration();
        printf("registration forgotten; the kiosk will register and show a new pairing code\n");
        apply_and_restart();
    } else if (strcmp(cmd, "factory") == 0) {
        config_defaults();
        if (!config_save()) printf("warning: config verify failed\n");
        do_reboot("factory reset");
    } else if (strcmp(cmd, "reboot") == 0) {
        do_reboot("console");
    } else if (strcmp(cmd, "test") == 0) {
        if (!kiosk_show_builtin(BUILTIN_TEST_PATTERN, NULL, NULL)) printf("error: could not draw the test pattern\n");
    } else {
        printf("unknown command \"%s\" - try help\n", cmd);
    }
}

#define CONSOLE_LINE_MAX 200   // "wifi" + quoted 32-char ssid + 64-char password fits easily

static char con_line[CONSOLE_LINE_MAX];
static size_t con_len;
static bool con_last_cr;
static bool con_overflow;

// Feeds one character (echo, backspace, CR/LF line ends). Exposed to the host test.
static uint8_t con_esc;   // 0 none, 1 after ESC, 2 inside a CSI sequence (ESC [ ... final byte)

static void console_feed(int ch) {
    // Terminals send arrow keys as ESC [ A etc.; swallow the whole sequence rather than echoing
    // "[A" into the command line.
    if (con_esc == 1) { con_esc = ch == '[' ? 2 : 0; return; }
    if (con_esc == 2) { if (ch >= 0x40 && ch <= 0x7e) con_esc = 0; return; }
    if (ch == 0x1b) { con_esc = 1; return; }
    if (ch == '\t') ch = ' ';
    if (ch == '\n' && con_last_cr) { con_last_cr = false; return; }   // CRLF counts once
    con_last_cr = ch == '\r';
    if (ch == '\r' || ch == '\n') {
        printf("\n");
        if (con_overflow) printf("error: line too long\n");
        else {
            con_line[con_len] = 0;
            console_exec(con_line);
        }
        con_len = 0;
        con_overflow = false;
        printf("> ");
        return;
    }
    if (ch == 8 || ch == 127) {
        if (con_len) { con_len--; printf("\b \b"); }
        return;
    }
    if (ch < 0x20 || ch > 0x7e) return;
    if (con_len + 1 >= sizeof con_line) { con_overflow = true; return; }
    con_line[con_len++] = (char)ch;
    putchar(ch);
}

#ifndef PROVISION_HOST_TEST
void console_poll(void) {
    // Bounded per call so a paste cannot stall the main loop.
    for (int i = 0; i < 64; i++) {
        int ch = getchar_timeout_us(0);
        if (ch < 0) break;
        console_feed(ch);
    }
}
#endif

// ---- Device-only: lwIP transport and the driver scan -----------------------------------------

#ifndef PROVISION_HOST_TEST

static conn_t conns[PORTAL_MAX_CONN];
static struct tcp_pcb *listen_pcb;

static void conn_detach(conn_t *c) {
    if (!c->pcb) return;
    tcp_arg(c->pcb, NULL);
    tcp_recv(c->pcb, NULL);
    tcp_sent(c->pcb, NULL);
    tcp_err(c->pcb, NULL);
    tcp_poll(c->pcb, NULL, 0);
}

static void conn_close(conn_t *c) {
    struct tcp_pcb *pcb = c->pcb;
    conn_detach(c);
    c->pcb = NULL;
    if (pcb && tcp_close(pcb) != ERR_OK) tcp_abort(pcb);
}

// For use when the caller is about to return ERR_ABRT to lwIP.
static void conn_abort(conn_t *c) {
    struct tcp_pcb *pcb = c->pcb;
    conn_detach(c);
    c->pcb = NULL;
    if (pcb) tcp_abort(pcb);
}

static void conn_pump(conn_t *c) {
    if (!c->pcb || !c->responding) return;
    uint32_t total = (uint32_t)c->hdr_len + c->body_total;
    bool wrote = false;
    while (c->sent < total) {
        uint16_t room = tcp_sndbuf(c->pcb);
        if (room == 0) break;
        const char *src;
        uint32_t avail;
        if (c->sent < c->hdr_len) { src = c->hdr + c->sent; avail = c->hdr_len - c->sent; }
        else { src = c->body + (c->sent - c->hdr_len); avail = total - c->sent; }
        uint16_t n = avail < room ? (uint16_t)avail : room;
        // COPY: the dynamic bodies live in the connection buffer, and lwIP would copy anyway
        // (LWIP_NETIF_TX_SINGLE_PBUF).
        u8_t flags = TCP_WRITE_FLAG_COPY | (c->sent + n < total ? TCP_WRITE_FLAG_MORE : 0);
        if (tcp_write(c->pcb, src, n, flags) != ERR_OK) break;
        c->sent += n;
        wrote = true;
    }
    if (wrote) tcp_output(c->pcb);
}

static err_t on_sent(void *arg, struct tcp_pcb *pcb, u16_t len) {
    conn_t *c = arg;
    (void)pcb;
    if (!c) return ERR_OK;
    c->acked += len;
    c->idle_polls = 0;
    uint32_t total = (uint32_t)c->hdr_len + c->body_total;
    if (c->acked >= total) conn_close(c);
    else conn_pump(c);
    return ERR_OK;
}

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    conn_t *c = arg;
    if (!p) {   // peer closed
        if (c) conn_close(c);
        else tcp_close(pcb);
        return ERR_OK;
    }
    if (!c || err != ERR_OK) {
        pbuf_free(p);
        if (c) conn_abort(c); else tcp_abort(pcb);
        return ERR_ABRT;
    }
    tcp_recved(pcb, p->tot_len);
    c->idle_polls = 0;
    // Walk the chain in pieces so an over-long request is refused without a big copy.
    uint8_t chunk[128];
    u16_t off = 0;
    int res = FEED_MORE;
    while (off < p->tot_len && res == FEED_MORE) {
        u16_t n = (u16_t)(p->tot_len - off < sizeof chunk ? p->tot_len - off : sizeof chunk);
        pbuf_copy_partial(p, chunk, n, off);
        off = (u16_t)(off + n);
        res = conn_feed(c, chunk, n);
    }
    pbuf_free(p);
    if (res == FEED_ABORT) { conn_abort(c); return ERR_ABRT; }
    if (res == FEED_RESPOND) conn_pump(c);
    return ERR_OK;
}

static err_t on_poll(void *arg, struct tcp_pcb *pcb) {
    conn_t *c = arg;
    (void)pcb;
    if (!c) return ERR_OK;
    if (++c->idle_polls > PORTAL_IDLE_POLLS) { conn_abort(c); return ERR_ABRT; }
    conn_pump(c);   // retry after an ERR_MEM
    return ERR_OK;
}

static void on_err(void *arg, err_t err) {
    conn_t *c = arg;
    (void)err;
    if (c) c->pcb = NULL;   // lwIP has already freed the pcb
}

static err_t on_accept(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)arg;
    if (err != ERR_OK || !pcb) return ERR_VAL;
    conn_t *c = NULL;
    for (int i = 0; i < PORTAL_MAX_CONN; i++)
        if (!conns[i].pcb) { c = &conns[i]; break; }
    if (!c) { tcp_abort(pcb); return ERR_ABRT; }
    memset(c, 0, sizeof *c);
    c->pcb = pcb;
    tcp_arg(pcb, c);
    tcp_recv(pcb, on_recv);
    tcp_sent(pcb, on_sent);
    tcp_err(pcb, on_err);
    tcp_poll(pcb, on_poll, 2);   // 2 × 500 ms
    tcp_nagle_disable(pcb);
    return ERR_OK;
}

static bool portal_listen(void) {
    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (!pcb) return false;
    if (tcp_bind(pcb, IP_ANY_TYPE, 80) != ERR_OK) { tcp_close(pcb); return false; }
    listen_pcb = tcp_listen_with_backlog(pcb, 4);
    if (!listen_pcb) { tcp_close(pcb); return false; }   // tcp_listen frees pcb on success only
    tcp_arg(listen_pcb, NULL);
    tcp_accept(listen_pcb, on_accept);
    return true;
}

static void portal_unlisten(void) {
    for (int i = 0; i < PORTAL_MAX_CONN; i++) conn_abort(&conns[i]);
    if (listen_pcb) { tcp_close(listen_pcb); listen_pcb = NULL; }
}

static bool portal_busy(void) {
    for (int i = 0; i < PORTAL_MAX_CONN; i++)
        if (conns[i].pcb && conns[i].responding) return true;
    return false;
}

static int scan_result_cb(void *env, const cyw43_ev_scan_result_t *r) {
    (void)env;
    if (r) nets_add(r->ssid, r->ssid_len, r->rssi, r->auth_mode == 0);
    return 0;
}

static void scan_start(void) {
    cyw43_wifi_scan_options_t opts;
    memset(&opts, 0, sizeof opts);
    if (cyw43_wifi_scan(&cyw43_state, &opts, NULL, scan_result_cb) == 0) {
        scan_started_ms = provision_now_ms();
        scan_ever = true;
    }
}

static bool scan_active(void) { return cyw43_wifi_scan_active(&cyw43_state); }

#else   // PROVISION_HOST_TEST

static bool portal_listen(void) { return true; }
static void portal_unlisten(void) {}
static bool portal_busy(void) { return false; }
static void scan_start(void) { scan_started_ms = provision_now_ms(); scan_ever = true; host_scan_start_hook(); }
static bool scan_active(void) { return false; }
#define cyw43_arch_lwip_begin() ((void)0)
#define cyw43_arch_lwip_end() ((void)0)

#endif

// ---- Public API -------------------------------------------------------------------------------

void provision_start(void) {
    if (active) return;
    const char *ssid = net_wifi_start_ap();
    copy_bounded(ap_ssid, sizeof ap_ssid, ssid, strlen(ssid));
    cyw43_arch_lwip_begin();
    bool ok = portal_listen();
    cyw43_arch_lwip_end();
    if (!ok) printf("portal: listen failed\n");
    active = true;
    scan_requested = true;   // have results ready before the phone loads the page
    kiosk_show_builtin(BUILTIN_PROVISION, ap_ssid, "http://" PORTAL_IP);
    printf("provisioning: join Wi-Fi \"%s\" and open " PORTAL_URL ", or type `help` here\n", ap_ssid);
}

void provision_stop(void) {
    if (!active) return;
    cyw43_arch_lwip_begin();
    portal_unlisten();
    cyw43_arch_lwip_end();
    net_wifi_stop_ap();
    active = false;
    ap_ssid[0] = 0;
}

bool provision_active(void) { return active; }

const char *provision_ap_ssid(void) { return ap_ssid; }

void provision_poll(void) {
    uint32_t now = provision_now_ms();

    if (save_requested && !reboot_at_ms) {
        pending_save_t p;
        cyw43_arch_lwip_begin();
        p = pending;
        cyw43_arch_lwip_end();
        copy_bounded(kiosk_config.wifi_ssid, sizeof kiosk_config.wifi_ssid, p.ssid, strlen(p.ssid));
        copy_bounded(kiosk_config.wifi_pass, sizeof kiosk_config.wifi_pass, p.pass, strlen(p.pass));
        if (p.server[0]) set_server_base(p.server);
        if (p.mode[0]) kiosk_config.video_mode = (uint8_t)video_mode_from_name(p.mode);
        if (!config_save()) printf("warning: config verify failed\n");
        printf("portal: saved wifi \"%s\", server %s\n", kiosk_config.wifi_ssid, kiosk_config.server_base);
        // save_requested stays set so a second POST is refused; the reboot clears everything.
        reboot_at_ms = now + REBOOT_GRACE_MS;
        if (reboot_at_ms == 0) reboot_at_ms = 1;
    }
    if (reboot_at_ms) {
        cyw43_arch_lwip_begin();
        bool busy = portal_busy();
        cyw43_arch_lwip_end();
        if (!busy || (int32_t)(now - reboot_at_ms) >= 0) do_reboot("provisioned");
        return;
    }

    if (!active) return;
    if (scan_requested) {
        bool run = !scan_active() && (!scan_ever || (uint32_t)(now - scan_started_ms) >= SCAN_MIN_INTERVAL_MS);
        if (run) {
            scan_requested = false;
            scan_start();
        } else if (scan_active()) {
            scan_requested = false;   // one is already running; its results land in the table
        }
    }
}
