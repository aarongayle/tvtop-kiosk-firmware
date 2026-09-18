// provision.c on the host: request parsing, form decoding, captive-portal routing, the staged
// save → config_save → reboot path, the scan table and the console interpreter. The lwIP
// transport is compiled out (PROVISION_HOST_TEST); flash_store.c is included with its RAM sector.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FLASH_STORE_HOST_TEST 1
#define PROVISION_HOST_TEST 1

// What board.h would provide.
typedef enum { VIDEO_720P30 = 0, VIDEO_720P30_RB = 1, VIDEO_480P60 = 2, VIDEO_MODE_COUNT } video_mode_t;
typedef struct { const char *name; } video_mode_info_t;
static const video_mode_info_t modes[6] = { { "720p30" }, { "720p30rb" }, { "480p60" }, { "720x480p60" }, { "960x540p60" }, { "1066x600p50" } };
const video_mode_info_t *video_mode_info(video_mode_t m) { return &modes[m < 3 ? m : 0]; }
video_mode_t video_mode_from_name(const char *name) {
    for (int i = 0; i < 3; i++) if (strcmp(modes[i].name, name) == 0) return (video_mode_t)i;
    return VIDEO_720P30;
}

#include "../../src/pico/flash_store.c"
#include "../../src/pico/provision.c"

static int failures;
#define CHECK(cond) do { if (!(cond)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

// ---- host plumbing --------------------------------------------------------------------------
uint8_t host_flash_sector[4096];
uint32_t host_now_ms;
int host_video_mode_from_name(const char *name) { return (int)video_mode_from_name(name); }

static int reboots, scans, restarts, builtins, ap_starts, ap_stops;
static int last_builtin;
static char last_builtin_arg1[64], last_builtin_arg2[64];
static wifi_state_t fake_wifi_state = WIFI_DOWN;

void host_reboot_hook(void) { reboots++; }
void host_scan_start_hook(void) { scans++; }
static bool last_ap_keep_station;
const char *net_wifi_start_ap(bool keep_station) { ap_starts++; last_ap_keep_station = keep_station; return "TVTOP-1A2B"; }
bool net_wifi_ap_is_concurrent(void) { return true; }
void net_wifi_stop_ap(void) { ap_stops++; }
wifi_state_t net_wifi_state(void) { return fake_wifi_state; }
const char *net_wifi_ip(char *buf, size_t cap) { snprintf(buf, cap, "10.0.0.7"); return buf; }
int net_wifi_rssi(void) { return -51; }
void kiosk_loop_restart(void) { restarts++; }
void kiosk_loop_status(char *buf, size_t cap) { snprintf(buf, cap, "loop: fake"); }
bool kiosk_show_builtin(int which, const char *a1, const char *a2) {
    builtins++;
    last_builtin = which;
    snprintf(last_builtin_arg1, sizeof last_builtin_arg1, "%s", a1 ? a1 : "(null)");
    snprintf(last_builtin_arg2, sizeof last_builtin_arg2, "%s", a2 ? a2 : "(null)");
    return true;
}

// Feeds a request in `chunk`-byte pieces and returns the feed result; the response is in c.
static int feed(conn_t *c, const char *req, size_t chunk) {
    memset(c, 0, sizeof *c);
    size_t len = strlen(req);
    int r = FEED_MORE;
    for (size_t i = 0; i < len && r == FEED_MORE; i += chunk) {
        size_t n = len - i < chunk ? len - i : chunk;
        r = conn_feed(c, (const uint8_t *)req + i, n);
    }
    return r;
}

static int status_of(const conn_t *c) { return atoi(c->hdr + 9); }
static bool hdr_has(const conn_t *c, const char *s) { return strstr(c->hdr, s) != NULL; }
static bool body_is(const conn_t *c, const char *s) {
    return c->body_total == strlen(s) && memcmp(c->body, s, c->body_total) == 0;
}
static bool body_has(const conn_t *c, const char *s) {
    char tmp[PORTAL_BUF + 1];
    size_t n = c->body_total < PORTAL_BUF ? c->body_total : PORTAL_BUF;
    memcpy(tmp, c->body, n);
    tmp[n] = 0;
    return strstr(tmp, s) != NULL;
}

static void fresh_config(void) {
    memset(host_flash_sector, 0xFF, sizeof host_flash_sector);
    config_load();
}

// ---- tests ------------------------------------------------------------------------------------

static void test_url_decode(void) {
    char out[16];
    CHECK(url_decode("a+b%20c%2Fd", 11, out, sizeof out) == 7 && strcmp(out, "a b c/d") == 0);
    CHECK(url_decode("", 0, out, sizeof out) == 0 && out[0] == 0);
    CHECK(url_decode("100%", 4, out, sizeof out) == 4 && strcmp(out, "100%") == 0);        // trailing %
    CHECK(url_decode("%4", 2, out, sizeof out) == 2 && strcmp(out, "%4") == 0);            // short
    CHECK(url_decode("%zz", 3, out, sizeof out) == 3 && strcmp(out, "%zz") == 0);          // bad hex kept
    CHECK(url_decode("%41%61", 6, out, sizeof out) == 2 && strcmp(out, "Aa") == 0);
    CHECK(url_decode("%C3%A9", 6, out, sizeof out) == 2 && (unsigned char)out[0] == 0xC3);  // UTF-8 bytes pass
    CHECK(url_decode("123456789012345", 15, out, 16) == 15);
    CHECK(url_decode("1234567890123456", 16, out, 16) == -1);                               // exactly too long
    CHECK(url_decode("%00", 3, out, sizeof out) == 1 && out[0] == 0);
}

static void test_form_field(void) {
    const char body[] = "ssid=My+Net&pass=p%26w%3Dd&empty=&server=http%3A%2F%2F10.0.0.1%3A8080&noeq";
    char out[64];
    CHECK(form_field(body, sizeof body - 1, "ssid", out, sizeof out) == 1 && strcmp(out, "My Net") == 0);
    CHECK(form_field(body, sizeof body - 1, "pass", out, sizeof out) == 1 && strcmp(out, "p&w=d") == 0);
    CHECK(form_field(body, sizeof body - 1, "empty", out, sizeof out) == 1 && out[0] == 0);
    CHECK(form_field(body, sizeof body - 1, "server", out, sizeof out) == 1 && strcmp(out, "http://10.0.0.1:8080") == 0);
    CHECK(form_field(body, sizeof body - 1, "noeq", out, sizeof out) == 1 && out[0] == 0);
    CHECK(form_field(body, sizeof body - 1, "ssi", out, sizeof out) == 0);       // prefix is not a match
    CHECK(form_field(body, sizeof body - 1, "mode", out, sizeof out) == 0);
    CHECK(form_field(body, sizeof body - 1, "ssid", out, 4) == -1);              // too long for cap
    CHECK(form_field("", 0, "ssid", out, sizeof out) == 0);
    CHECK(form_field("&&&", 3, "ssid", out, sizeof out) == 0);
}

static void test_parse_request(void) {
    request_t r;
    const char req[] = "GET /generate_204?x=1 HTTP/1.1\r\nhost:  connectivitycheck.gstatic.com \r\nContent-Length: 0\r\n\r\n";
    parse_request(req, sizeof req - 1, &r);
    CHECK(!r.bad);
    CHECK(r.method_len == 3 && memcmp(r.method, "GET", 3) == 0);
    CHECK(r.path_len == 13 && memcmp(r.path, "/generate_204", 13) == 0);
    CHECK(r.host_len == strlen("connectivitycheck.gstatic.com") && memcmp(r.host, "connectivitycheck.gstatic.com", r.host_len) == 0);
    CHECK(r.content_length == 0);

    const char post[] = "POST /save HTTP/1.1\r\nHost: 192.168.4.1\r\nCONTENT-LENGTH: 42\r\n\r\n";
    parse_request(post, sizeof post - 1, &r);
    CHECK(!r.bad && r.content_length == 42 && r.method_len == 4);

    const char noh[] = "GET / HTTP/1.0\r\n\r\n";
    parse_request(noh, sizeof noh - 1, &r);
    CHECK(!r.bad && r.host_len == 0 && r.path_len == 1);

    const char bad1[] = "GARBAGE\r\n\r\n";
    parse_request(bad1, sizeof bad1 - 1, &r);
    CHECK(r.bad);
    const char bad2[] = "GET nopath HTTP/1.1\r\n\r\n";
    parse_request(bad2, sizeof bad2 - 1, &r);
    CHECK(r.bad);
    const char bad3[] = "POST /save HTTP/1.1\r\nContent-Length: 12x\r\n\r\n";
    parse_request(bad3, sizeof bad3 - 1, &r);
    CHECK(r.bad);
    const char bad4[] = "POST /save HTTP/1.1\r\nContent-Length: 99999999999999999999\r\n\r\n";
    parse_request(bad4, sizeof bad4 - 1, &r);
    CHECK(r.bad);
    const char empty[] = "\r\n\r\n";
    parse_request(empty, sizeof empty - 1, &r);
    CHECK(r.bad);
}

static void test_routing(void) {
    fresh_config();
    conn_t c;
    // The page, in one piece and byte by byte.
    CHECK(feed(&c, "GET / HTTP/1.1\r\nHost: 192.168.4.1\r\n\r\n", 1000) == FEED_RESPOND);
    CHECK(status_of(&c) == 200 && hdr_has(&c, "text/html") && hdr_has(&c, "Connection: close"));
    CHECK(c.body == portal_html && c.body_total == sizeof portal_html - 1);
    char cl[40];
    snprintf(cl, sizeof cl, "Content-Length: %u\r\n", (unsigned)(sizeof portal_html - 1));
    CHECK(hdr_has(&c, cl));
    CHECK(feed(&c, "GET /index.html HTTP/1.1\r\nHost: 192.168.4.1:80\r\n\r\n", 1) == FEED_RESPOND);
    CHECK(status_of(&c) == 200 && c.body == portal_html);
    // A request split exactly at the blank line.
    CHECK(feed(&c, "GET / HTTP/1.1\r\nHost: 192.168.4.1\r\n\r\n", 30) == FEED_RESPOND);
    CHECK(status_of(&c) == 200);

    // Probes redirect, whatever the host.
    const char *probes[] = { "/generate_204", "/gen_204", "/hotspot-detect.html", "/library/test/success.html",
                             "/connecttest.txt", "/ncsi.txt", "/redirect" };
    for (size_t i = 0; i < sizeof probes / sizeof probes[0]; i++) {
        char req[160];
        snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: 192.168.4.1\r\n\r\n", probes[i]);
        CHECK(feed(&c, req, 1000) == FEED_RESPOND);
        CHECK(status_of(&c) == 302 && hdr_has(&c, "Location: http://192.168.4.1/\r\n"));
        CHECK(hdr_has(&c, "Content-Length: ") && hdr_has(&c, "Connection: close"));
    }
    // Any path on a foreign host redirects, even the page.
    CHECK(feed(&c, "GET / HTTP/1.1\r\nHost: captive.apple.com\r\n\r\n", 1000) == FEED_RESPOND);
    CHECK(status_of(&c) == 302);
    CHECK(feed(&c, "GET /scan HTTP/1.1\r\nHost: www.msftconnecttest.com\r\n\r\n", 1000) == FEED_RESPOND);
    CHECK(status_of(&c) == 302);
    // No Host header (HTTP/1.0) is treated as ours.
    CHECK(feed(&c, "GET / HTTP/1.0\r\n\r\n", 1000) == FEED_RESPOND);
    CHECK(status_of(&c) == 200);
    // Unknown path on our host: 404. Unknown method: 404 as well (not a redirect loop).
    CHECK(feed(&c, "GET /favicon.ico HTTP/1.1\r\nHost: 192.168.4.1\r\n\r\n", 1000) == FEED_RESPOND);
    CHECK(status_of(&c) == 404);
    CHECK(feed(&c, "PUT / HTTP/1.1\r\nHost: 192.168.4.1\r\n\r\n", 1000) == FEED_RESPOND);
    CHECK(status_of(&c) == 404);
    // Garbage.
    CHECK(feed(&c, "\r\n\r\n", 1000) == FEED_RESPOND);
    CHECK(status_of(&c) == 400);
    // A head that never ends fills the buffer → 431, and later bytes are ignored.
    char big[PORTAL_BUF + 100];
    memset(big, 'A', sizeof big);
    memcpy(big, "GET / HTTP/1.1\r\nX: ", 19);
    big[sizeof big - 1] = 0;
    CHECK(feed(&c, big, 100) == FEED_RESPOND);
    CHECK(status_of(&c) == 431);
    CHECK(conn_feed(&c, (const uint8_t *)"more", 4) == FEED_MORE);
    // A declared body that cannot fit → 413 before it arrives.
    CHECK(feed(&c, "POST /save HTTP/1.1\r\nHost: 192.168.4.1\r\nContent-Length: 4000\r\n\r\n", 1000) == FEED_RESPOND);
    CHECK(status_of(&c) == 413);
    // A single oversized chunk → 413.
    memset(&c, 0, sizeof c);
    static uint8_t huge[PORTAL_BUF + 1];
    memset(huge, 'B', sizeof huge);
    CHECK(conn_feed(&c, huge, sizeof huge) == FEED_RESPOND);
    CHECK(status_of(&c) == 413);
}

static void test_config_json(void) {
    fresh_config();
    strcpy(kiosk_config.nets[0].ssid, "Caf\xc3\xa9 \"Quoted\"\\");
    kiosk_config.net_count = 1;
    strcpy(kiosk_config.server_base, "http://192.168.1.10:8080");
    kiosk_config.video_mode = VIDEO_480P60;
    strcpy(ap_ssid, "TVTOP-1A2B");
    conn_t c;
    CHECK(feed(&c, "GET /config.json HTTP/1.1\r\nHost: 192.168.4.1\r\n\r\n", 1000) == FEED_RESPOND);
    CHECK(status_of(&c) == 200 && hdr_has(&c, "application/json"));
    CHECK(body_has(&c, "\"ssid\":\"Caf\xc3\xa9 \\\"Quoted\\\"\\\\\""));
    CHECK(body_has(&c, "\"server\":\"http://192.168.1.10:8080\""));
    CHECK(body_has(&c, "\"mode\":\"480p60\""));
    CHECK(body_has(&c, "\"ap\":\"TVTOP-1A2B\""));
    CHECK(body_has(&c, "\"registered\":false}"));
    strcpy(kiosk_config.token, "abc");
    CHECK(feed(&c, "GET /config.json HTTP/1.1\r\nHost: 192.168.4.1\r\n\r\n", 1000) == FEED_RESPOND);
    CHECK(body_has(&c, "\"registered\":true}"));
    ap_ssid[0] = 0;
}

static void test_scan_table_and_json(void) {
    nets_n = 0;
    nets_add((const uint8_t *)"Alpha", 5, -70, false);
    nets_add((const uint8_t *)"Beta", 4, -40, true);
    nets_add((const uint8_t *)"Alpha", 5, -60, false);   // dedupe, keep strongest
    nets_add((const uint8_t *)"Alpha", 5, -80, false);
    nets_add((const uint8_t *)"", 0, -10, false);         // hidden: dropped
    nets_add((const uint8_t *)"0123456789012345678901234567890123", 34, -10, false);   // > 32: dropped
    CHECK(nets_n == 2);
    CHECK(strcmp(nets[0].ssid, "Alpha") == 0 && nets[0].rssi == -60);
    CHECK(strcmp(nets[1].ssid, "Beta") == 0 && nets[1].rssi == -40 && nets[1].open);
    // Fill the table; a weaker newcomer is dropped, a stronger one replaces the weakest.
    for (int i = 0; i < PORTAL_MAX_NETS; i++) {
        char name[8];
        snprintf(name, sizeof name, "N%d", i);
        nets_add((const uint8_t *)name, strlen(name), (int16_t)(-90 + i), false);
    }
    CHECK(nets_n == PORTAL_MAX_NETS);
    nets_add((const uint8_t *)"Weak", 4, -95, false);
    CHECK(nets_n == PORTAL_MAX_NETS);
    bool found_weak = false, found_strong = false, found_n0 = false;
    nets_add((const uint8_t *)"Strong", 6, -20, true);
    for (unsigned i = 0; i < nets_n; i++) {
        if (strcmp(nets[i].ssid, "Weak") == 0) found_weak = true;
        if (strcmp(nets[i].ssid, "Strong") == 0) found_strong = true;
        if (strcmp(nets[i].ssid, "N0") == 0) found_n0 = true;
    }
    CHECK(!found_weak && found_strong && !found_n0);   // N0 (-90) was the weakest

    nets_n = 0;
    nets_add((const uint8_t *)"A \"B\"", 5, -33, true);
    scan_requested = false;
    conn_t c;
    CHECK(feed(&c, "GET /scan HTTP/1.1\r\nHost: 192.168.4.1\r\n\r\n", 1000) == FEED_RESPOND);
    CHECK(status_of(&c) == 200);
    CHECK(body_is(&c, "[{\"rssi\":-33,\"open\":true,\"ssid\":\"A \\\"B\\\"\"}]"));
    CHECK(scan_requested);   // a GET /scan asks main context for a fresh scan
    nets_n = 0;
    CHECK(feed(&c, "GET /scan HTTP/1.1\r\nHost: 192.168.4.1\r\n\r\n", 1000) == FEED_RESPOND);
    CHECK(body_is(&c, "[]"));
    // 32 full-size names must fit the buffer.
    for (int i = 0; i < PORTAL_MAX_NETS; i++) {
        char name[33];
        memset(name, 'a' + i, 32);
        name[32] = 0;
        nets_add((const uint8_t *)name, 32, (int16_t)(-50 - i), false);
    }
    CHECK(feed(&c, "GET /scan HTTP/1.1\r\nHost: 192.168.4.1\r\n\r\n", 1000) == FEED_RESPOND);
    CHECK(status_of(&c) == 200 && c.body_total > PORTAL_MAX_NETS * 40);
    nets_n = 0;
}

static void test_save_validation(void) {
    fresh_config();
    save_requested = false;
    reboot_at_ms = 0;
    conn_t c;
    #define POST(body) "POST /save HTTP/1.1\r\nHost: 192.168.4.1\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: "
    // Build requests with the right Content-Length.
    char req[1400];
    #define SAVE_REQ(bodystr) (snprintf(req, sizeof req, "POST /save HTTP/1.1\r\nHost: 192.168.4.1\r\nContent-Length: %u\r\n\r\n%s", (unsigned)strlen(bodystr), bodystr), req)

    CHECK(feed(&c, SAVE_REQ("pass=12345678&server=http://x"), 1000) == FEED_RESPOND);
    CHECK(status_of(&c) == 400 && body_has(&c, "network name"));
    CHECK(feed(&c, SAVE_REQ("ssid=&pass=12345678"), 1000) == FEED_RESPOND);
    CHECK(status_of(&c) == 400);
    CHECK(feed(&c, SAVE_REQ("ssid=Net&pass=short"), 1000) == FEED_RESPOND);
    CHECK(status_of(&c) == 400 && body_has(&c, "8 characters"));
    CHECK(feed(&c, SAVE_REQ("ssid=Net&pass=12345678&server=ftp://x"), 1000) == FEED_RESPOND);
    CHECK(status_of(&c) == 400 && body_has(&c, "http://"));
    CHECK(feed(&c, SAVE_REQ("ssid=Net&pass=12345678&server=http://"), 1000) == FEED_RESPOND);
    CHECK(status_of(&c) == 400);
    CHECK(feed(&c, SAVE_REQ("ssid=Net&pass=12345678&mode=1080p"), 1000) == FEED_RESPOND);
    CHECK(status_of(&c) == 400 && body_has(&c, "video mode"));
    // Over-length fields are refused, not truncated.
    char body[600];
    snprintf(body, sizeof body, "ssid=%033d", 0);
    CHECK(feed(&c, SAVE_REQ(body), 1000) == FEED_RESPOND);
    CHECK(status_of(&c) == 400 && body_has(&c, "too long"));
    snprintf(body, sizeof body, "ssid=Net&pass=%065d", 0);
    CHECK(feed(&c, SAVE_REQ(body), 1000) == FEED_RESPOND);
    CHECK(status_of(&c) == 400 && body_has(&c, "too long"));
    memset(body, 'u', sizeof body - 1);
    body[sizeof body - 1] = 0;
    memcpy(body, "ssid=Net&server=http://", 23);
    CHECK(feed(&c, SAVE_REQ(body), 1000) == FEED_RESPOND);
    CHECK(status_of(&c) == 400 && body_has(&c, "too long"));
    // Exactly at the limits is fine.
    snprintf(body, sizeof body, "ssid=%032d&pass=%064d", 0, 0);
    CHECK(feed(&c, SAVE_REQ(body), 1000) == FEED_RESPOND);
    CHECK(status_of(&c) == 200);
    CHECK(save_requested);
    save_requested = false;   // the poll would normally consume it
    CHECK(!reboots);
}

static void test_save_apply_and_reboot(void) {
    fresh_config();
    strcpy(kiosk_config.token, "oldtoken");
    strcpy(kiosk_config.next_url, "https://kiosk.tvtop.games/v1/frame/x");
    strcpy(kiosk_config.device_id, "abc123");
    save_requested = false;
    reboot_at_ms = 0;
    reboots = 0;
    active = true;   // as after provision_start
    conn_t c;
    char req[600];
    const char *body = "ssid=Home+Net&pass=p%40ss+word1&server=http%3A%2F%2F192.168.1.10%3A8080%2F&mode=480p60";
    snprintf(req, sizeof req, "POST /save HTTP/1.1\r\nHost: 192.168.4.1\r\nContent-Length: %u\r\n\r\n%s", (unsigned)strlen(body), body);
    CHECK(feed(&c, req, 7) == FEED_RESPOND);
    CHECK(status_of(&c) == 200 && body_has(&c, "rebooting") && c.reboot_after);
    CHECK(save_requested);
    // Config is untouched until main context runs.
    CHECK(kiosk_config.nets[0].ssid[0] == 0);
    // A second POST while one is pending is refused.
    CHECK(feed(&c, req, 1000) == FEED_RESPOND);
    CHECK(status_of(&c) == 409);

    host_now_ms = 5000;
    provision_poll();
    CHECK(strcmp(kiosk_config.nets[0].ssid, "Home Net") == 0);
    CHECK(strcmp(kiosk_config.nets[0].pass, "p@ss word1") == 0);
    CHECK(strcmp(kiosk_config.server_base, "http://192.168.1.10:8080") == 0);   // trailing slash stripped
    CHECK(kiosk_config.video_mode == VIDEO_480P60);
    CHECK(kiosk_config.token[0] == 0 && kiosk_config.next_url[0] == 0 && kiosk_config.device_id[0] == 0);   // new server → re-register
    // Written to flash and consistent.
    kiosk_config_t img;
    memcpy(&img, host_flash_sector, sizeof img);
    CHECK(strcmp(img.nets[0].ssid, "Home Net") == 0 && img.crc32 == crc32_update(0, &img, offsetof(kiosk_config_t, crc32)));
    // The host has no in-flight connections, so the reboot follows on the same/next poll.
    CHECK(reboots == 1);
    // Idempotent: further polls must not save again or reboot twice before the (fake) reboot.
    host_flash_sector[0] = 0x00;
    provision_poll();
    CHECK(host_flash_sector[0] == 0x00);
    CHECK(reboots == 2);   // still "rebooting" — the real watchdog would have fired by now
    active = false;
    save_requested = false;
    reboot_at_ms = 0;

    // Same server → the registration survives a Wi-Fi change.
    fresh_config();
    strcpy(kiosk_config.token, "keepme");
    strcpy(kiosk_config.next_url, "https://kiosk.tvtop.games/v1/frame/x");
    body = "ssid=Other&pass=&server=https%3A%2F%2Fkiosk.tvtop.games";
    snprintf(req, sizeof req, "POST /save HTTP/1.1\r\nHost: 192.168.4.1\r\nContent-Length: %u\r\n\r\n%s", (unsigned)strlen(body), body);
    CHECK(feed(&c, req, 1000) == FEED_RESPOND);
    CHECK(status_of(&c) == 200);
    provision_poll();
    CHECK(strcmp(kiosk_config.nets[0].ssid, "Other") == 0 && kiosk_config.nets[0].pass[0] == 0);
    CHECK(strcmp(kiosk_config.token, "keepme") == 0 && kiosk_config.next_url[0] != 0);
    save_requested = false;
    reboot_at_ms = 0;
    reboots = 0;
}

static void test_start_stop_and_scan_pacing(void) {
    fresh_config();
    builtins = 0; ap_starts = 0; ap_stops = 0; scans = 0;
    scan_requested = false; scan_ever = false; scan_started_ms = 0;
    provision_start(false);
    CHECK(provision_active());
    CHECK(ap_starts == 1);
    CHECK(strcmp(provision_ap_ssid(), "TVTOP-1A2B") == 0);
    CHECK(builtins == 1 && last_builtin == BUILTIN_PROVISION);
    CHECK(strcmp(last_builtin_arg1, "TVTOP-1A2B") == 0 && strcmp(last_builtin_arg2, "http://192.168.4.1") == 0);
    provision_start(false);   // idempotent
    CHECK(ap_starts == 1 && builtins == 1);
    // The first poll starts a scan straight away; repeated requests are paced.
    host_now_ms = 10000;
    provision_poll();
    CHECK(scans == 1 && !scan_requested);
    scan_requested = true;
    host_now_ms = 12000;
    provision_poll();
    CHECK(scans == 1 && scan_requested);   // too soon: stays requested
    host_now_ms = 10000 + SCAN_MIN_INTERVAL_MS;
    provision_poll();
    CHECK(scans == 2 && !scan_requested);
    provision_stop();
    CHECK(!provision_active() && ap_stops == 1 && provision_ap_ssid()[0] == 0);
    provision_stop();
    CHECK(ap_stops == 1);
    // Inactive: scans still run. The join manager needs to know which of the stored networks is
    // actually in range while the portal is down, so scanning is no longer the portal's alone.
    scan_requested = true;
    host_now_ms += 100000;
    provision_poll();
    CHECK(scans == 3 && !scan_requested);
}

static void test_split_args(void) {
    char line[] = "  wifi \"My Home Net\" pass  word ";
    char *argv[4];
    int n = split_args(line, argv, 4);
    CHECK(n == 4);
    CHECK(strcmp(argv[0], "wifi") == 0 && strcmp(argv[1], "My Home Net") == 0);
    CHECK(strcmp(argv[2], "pass") == 0 && strcmp(argv[3], "word") == 0);
    char l2[] = "\"unterminated";
    n = split_args(l2, argv, 4);
    CHECK(n == 1 && strcmp(argv[0], "unterminated") == 0);
    char l3[] = "";
    CHECK(split_args(l3, argv, 4) == 0);
    char l4[] = "a b c d e f";
    CHECK(split_args(l4, argv, 4) == 4);   // extra tokens ignored, no overflow
    char l5[] = "wifi \"\" x";
    n = split_args(l5, argv, 4);
    CHECK(n == 3 && argv[1][0] == 0);
}

static void test_console(void) {
    fresh_config();
    restarts = 0; reboots = 0; builtins = 0; ap_stops = 0;
    active = false;

    char l1[] = "wifi \"My Home Net\" secret123";
    console_exec(l1);
    CHECK(strcmp(kiosk_config.nets[0].ssid, "My Home Net") == 0 && strcmp(kiosk_config.nets[0].pass, "secret123") == 0);
    CHECK(restarts == 1);
    kiosk_config_t img;
    memcpy(&img, host_flash_sector, sizeof img);
    CHECK(strcmp(img.nets[0].ssid, "My Home Net") == 0);   // saved

    char l2[] = "wifi OpenNet";
    console_exec(l2);
    CHECK(strcmp(kiosk_config.nets[0].ssid, "OpenNet") == 0 && kiosk_config.nets[0].pass[0] == 0 && restarts == 2);

    char l3[] = "wifi Net short";   // rejected: no change, no restart
    console_exec(l3);
    CHECK(strcmp(kiosk_config.nets[0].ssid, "OpenNet") == 0 && restarts == 2);
    char l3b[] = "wifi 123456789012345678901234567890123 12345678";   // 33-char ssid
    console_exec(l3b);
    CHECK(strcmp(kiosk_config.nets[0].ssid, "OpenNet") == 0 && restarts == 2);
    char l3c[] = "wifi";
    console_exec(l3c);
    CHECK(restarts == 2);

    // While provisioning, a console wifi command stops the portal.
    active = true;
    char l4[] = "wifi Again 12345678";
    console_exec(l4);
    CHECK(!active && ap_stops == 1 && restarts == 3);

    strcpy(kiosk_config.token, "tok");
    strcpy(kiosk_config.next_url, "u");
    char l5[] = "server http://192.168.1.10:8080/";
    console_exec(l5);
    CHECK(strcmp(kiosk_config.server_base, "http://192.168.1.10:8080") == 0);
    CHECK(kiosk_config.token[0] == 0 && kiosk_config.next_url[0] == 0 && restarts == 4);
    strcpy(kiosk_config.token, "tok2");
    char l5b[] = "server http://192.168.1.10:8080";   // unchanged: token kept
    console_exec(l5b);
    CHECK(strcmp(kiosk_config.token, "tok2") == 0 && restarts == 5);
    char l6[] = "server nope";
    console_exec(l6);
    CHECK(strcmp(kiosk_config.server_base, "http://192.168.1.10:8080") == 0 && restarts == 5);

    char l7[] = "mode 480p60";
    console_exec(l7);
    CHECK(kiosk_config.video_mode == VIDEO_480P60 && reboots == 1);
    memcpy(&img, host_flash_sector, sizeof img);
    CHECK(img.video_mode == VIDEO_480P60);
    char l8[] = "mode 1080p";
    console_exec(l8);
    CHECK(kiosk_config.video_mode == VIDEO_480P60 && reboots == 1);

    strcpy(kiosk_config.token, "tok3");
    strcpy(kiosk_config.static_id, "s-1");
    char l9[] = "reset";
    console_exec(l9);
    CHECK(kiosk_config.token[0] == 0 && kiosk_config.static_id[0] == 0 && strcmp(kiosk_config.nets[0].ssid, "Again") == 0);
    CHECK(restarts == 6);

    char l10[] = "factory";
    console_exec(l10);
    CHECK(kiosk_config.nets[0].ssid[0] == 0 && strcmp(kiosk_config.server_base, "https://kiosk.tvtop.games") == 0);
    CHECK(reboots == 2);
    memcpy(&img, host_flash_sector, sizeof img);
    CHECK(img.nets[0].ssid[0] == 0);

    char l11[] = "reboot";
    console_exec(l11);
    CHECK(reboots == 3);
    char l12[] = "test";
    console_exec(l12);
    CHECK(builtins == 1 && last_builtin == BUILTIN_TEST_PATTERN);
    char l13[] = "bogus";
    console_exec(l13);
    char l14[] = "status";
    fake_wifi_state = WIFI_UP;
    console_exec(l14);
    char l15[] = "help";
    console_exec(l15);
    char l16[] = "stats";
    console_exec(l16);
    char l17[] = "   ";
    console_exec(l17);
    CHECK(restarts == 6 && reboots == 3);
}

static void test_console_line_editing(void) {
    fresh_config();
    restarts = 0;
    // "wifx" + backspace + "i Net 12345678" + CRLF (LF must not run an empty second command).
    const char *keys = "wifx\bi Nex\x7ft 12345678\r\n";   // BS and DEL both erase
    for (const char *k = keys; *k; k++) console_feed((unsigned char)*k);
    CHECK(strcmp(kiosk_config.nets[0].ssid, "Net") == 0 && restarts == 1);
    // Bare LF line ends work too; control characters are dropped.
    const char *keys2 = "\x1b[Awifi\tLF2 12345678\n";
    for (const char *k = keys2; *k; k++) console_feed((unsigned char)*k);
    CHECK(strcmp(kiosk_config.nets[0].ssid, "LF2") == 0 && restarts == 2);
    // An over-long line is discarded whole, and the next line works.
    for (int i = 0; i < CONSOLE_LINE_MAX + 50; i++) console_feed('w');
    console_feed('\r');
    CHECK(restarts == 2);
    const char *keys3 = "wifi After 12345678\r";
    for (const char *k = keys3; *k; k++) console_feed((unsigned char)*k);
    CHECK(strcmp(kiosk_config.nets[0].ssid, "After") == 0 && restarts == 3);
}

int main(void) {
    test_url_decode();
    test_form_field();
    test_parse_request();
    test_routing();
    test_config_json();
    test_scan_table_and_json();
    test_save_validation();
    test_save_apply_and_reboot();
    test_start_stop_and_scan_pacing();
    test_split_args();
    test_console();
    test_console_line_editing();
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("test_provision: ok\n");
    return 0;
}
