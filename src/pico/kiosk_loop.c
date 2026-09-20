// The protocol state machine (docs/KIOSK_PROTOCOL.md, "The whole protocol"):
//
//   loop forever: GET next_url → 200: draw, remember next_url; 304: keep drawing; sleep next_ms
//
// plus the three moments the device has no frame for: no Wi-Fi credentials (provisioning), joining
// Wi-Fi, and registering. Each of those is drawn from a built-in JSON frame through the normal
// decoder so there is exactly one rendering path.
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/rand.h"
#include "hardware/clocks.h"
#include "kiosk_loop.h"
#include "kiosk_config.h"
#include "flash_store.h"
#include "net_wifi.h"
#include "http_client.h"
#include "provision.h"
#include "scanout.h"
#include "frame.h"
#include "font.h"
#include "palette.h"
#include "geom.h"
#include "raster.h"
#include "linepool.h"
#include "icons.h"
#include "json.h"
#include "builtin_frames.h"
#include "ca_certs.h"
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"

extern linepool_t kiosk_linepool;

#define LONG_POLL_TIMEOUT_MS 45000u    // the server parks a request for 25 s; the protocol says >= 40 s
#define REGISTER_TIMEOUT_MS 20000u
#define BACKOFF_MIN_MS 1000u
#define BACKOFF_MAX_MS 60000u
#define POLL_FLOOR_MS 250u
#define OFFLINE_AFTER_FAILURES 2

typedef enum { KS_BOOT, KS_PROVISION, KS_WIFI_WAIT, KS_REGISTER, KS_POLL, KS_HALT } state_t;

static state_t state;
static frame_t frame;
static frame_decoder_t decoder;
static palette_t pal;
static uint8_t scratch[KIOSK_SCRATCH_BYTES] __attribute__((aligned(8)));
static uint8_t raster_scratch[RASTER_SCRATCH_BYTES];
static geom_store_t *geom;
static const video_mode_info_t *mode;

static char cur_url[KIOSK_MAX_URL];      // the URL to GET next
static bool request_active;
static int rsp_status;
static bool rsp_decoding;                // a 200 body is streaming into the decoder
static uint32_t next_request_ms;         // earliest time for the next GET
static uint32_t consecutive_failures;
static bool frame_valid;                 // frame_t holds a drawable frame (for overlays)
static bool overlay_offline, overlay_version;
static char reg_body[600];
static size_t reg_len;
static uint32_t stat_decode_ms, stat_render_ms, stat_pool_bytes, stat_frames, stat_errors;
static uint32_t netcheck_asked_ms;          // see "joined, but is anything out there?" below
static bool netcheck_waiting;               // a verdict we asked for and have not acted on
static char last_error[48];

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

// ---- URLs ----------------------------------------------------------------------------------------

static void config_url(char *out, size_t cap) {
    snprintf(out, cap, "%s/v1/config?t=%s", kiosk_config.server_base, kiosk_config.token);
}

static void register_url(char *out, size_t cap) {
    snprintf(out, cap, "%s/v1/register?model=%s&fw=%s", kiosk_config.server_base, KIOSK_MODEL, KIOSK_FW_VERSION);
}

// Copies url without its rev=… query parameter, so two URLs that differ only by revision compare
// equal (that is the only part that changes on every frame, and flash must not be rewritten for it).
static void strip_rev(const char *url, char *out, size_t cap) {
    size_t n = 0;
    const char *q = strchr(url, '?');
    for (const char *p = url; *p && n + 1 < cap;) {
        if (q && p > q && strncmp(p, "rev=", 4) == 0 && (p[-1] == '?' || p[-1] == '&')) {
            const char *e = p + 4;
            while (*e && *e != '&') e++;
            if (*e == '&') e++;                       // drop "rev=…&"
            else if (n && out[n - 1] == '&') n--;    // drop "&rev=…" at the end
            else if (n && out[n - 1] == '?') n--;    // "?rev=…" was the whole query
            p = e;
            continue;
        }
        out[n++] = *p++;
    }
    out[n] = 0;
}

static bool url_has_static(const char *url) {
    const char *q = strchr(url, '?');
    if (!q) return false;
    for (const char *p = q + 1; *p;) {
        if (strncmp(p, "s=", 2) == 0) return true;
        while (*p && *p != '&') p++;
        if (*p == '&') p++;
    }
    return false;
}

// Remembers next_url in flash when its shape changed (endpoint or static id), never per revision.
static void persist_next_url(const char *url, const char *static_id) {
    char a[KIOSK_MAX_URL], b[KIOSK_MAX_URL];
    strip_rev(url, a, sizeof a);
    strip_rev(kiosk_config.next_url, b, sizeof b);
    bool same_static = strncmp(kiosk_config.static_id, static_id ? static_id : "", KIOSK_MAX_STATIC_ID) == 0;
    if (strcmp(a, b) == 0 && same_static) return;
    strncpy(kiosk_config.next_url, url, KIOSK_MAX_URL - 1);
    kiosk_config.next_url[KIOSK_MAX_URL - 1] = 0;
    memset(kiosk_config.static_id, 0, sizeof kiosk_config.static_id);
    if (static_id) strncpy(kiosk_config.static_id, static_id, KIOSK_MAX_STATIC_ID - 1);
    config_mark_dirty();
}

// ---- rendering -----------------------------------------------------------------------------------

typedef struct { uint32_t bytes; } sink_ctx_t;

// Encoded into scratch first, then only the bytes the line needs are taken from the pool. Reserving
// the worst case (LINE_MAX_BYTES: 7.7 KB per line on HSTX at 1080p) on every line made a detailed
// board fail to fit while old and new frames share the ring, and a line that fails is drawn as a
// copy of the one above it — text near the bottom of the screen came out smeared and blocky.
static void line_sink(void *ctx, uint16_t y, const uint8_t *px, uint16_t width) {
    static uint8_t scratch[LINE_MAX_BYTES];   // core 0 only
    sink_ctx_t *s = ctx;
    uint16_t n = scanout_encode_line(px, width, scratch, LINE_MAX_BYTES);
    if (!n) { linepool_commit_dup(&kiosk_linepool, y); return; }
    uint8_t *p = linepool_alloc(&kiosk_linepool, y, n);
    if (!p) { linepool_commit_dup(&kiosk_linepool, y); return; }
    memcpy(p, scratch, n);
    linepool_commit(&kiosk_linepool, y, p, n);
    s->bytes += n;
}

static void render_frame_now(void) {
    op_t extra[3];
    uint16_t nextra = 0;
    if (overlay_offline || overlay_version) {
        uint8_t dark = palette_add(&pal, 0x101820), ink = palette_add(&pal, 0xf8f4ec), warn = palette_add(&pal, 0xffcc66);
        if (frame_make_overlay_rect(&extra[nextra], mode->w, mode->h, 1180, 640, 80, 60, 12, dark, 220)) nextra++;
        if (overlay_offline && frame_make_overlay_icon(&extra[nextra], mode->w, mode->h, 1198, 648, 44, icon_lookup("wifi", 4), ink)) nextra++;
        else if (overlay_version && frame_make_overlay_icon(&extra[nextra], mode->w, mode->h, 1198, 648, 44, icon_lookup("warning", 7), warn)) nextra++;
    }
    scanout_frame_begin();
    scanout_upload_palette(&pal);   // colours the decoder added must be live before their lines are
    raster_t r;
    raster_init(&r, scratch, raster_scratch, mode->w, mode->h, &pal);
    linepool_frame_begin(&kiosk_linepool);
    sink_ctx_t sc = {0};
    render_stats_t rs = {0};
    uint32_t t0 = now_ms();
    frame_render(&frame, &pal, geom, &r, extra, nextra, line_sink, &sc, &rs);
    scanout_upload_palette(&pal);   // blends allocated while rendering
    stat_render_ms = now_ms() - t0;
    stat_pool_bytes = sc.bytes;
    stat_frames++;
}

bool kiosk_show_builtin(int which, const char *arg1, const char *arg2) {
    static char buf[3072];   // the test pattern, the largest built-in frame, is ~2.5 KB
    size_t n = builtin_frame_json((builtin_frame_t)which, buf, sizeof buf, arg1, arg2);
    if (!n) return false;
    frame_decoder_init(&decoder, &frame, &pal, geom, scratch, sizeof scratch, mode->w, mode->h);
    frame_decoder_feed(&decoder, buf, n);
    if (frame_decoder_finish(&decoder) != FD_OK) { frame_valid = false; return false; }
    frame_valid = true;
    render_frame_now();
    return true;
}

// ---- HTTP: registration ----------------------------------------------------------------------

typedef struct { char token[27]; char id[7]; char next_url[KIOSK_MAX_URL]; uint8_t which; size_t len; } reg_parse_t;

static bool reg_cb(void *ctx, const json_stream_t *js, json_event_t ev, const char *data, size_t len, bool final) {
    reg_parse_t *r = ctx;
    if (js->depth != 1 || ev != JSON_EV_STRING) return true;
    char *dst; size_t cap;
    if (!strcmp(js->key[0], "token")) { dst = r->token; cap = sizeof r->token; }
    else if (!strcmp(js->key[0], "id")) { dst = r->id; cap = sizeof r->id; }
    else if (!strcmp(js->key[0], "next_url")) { dst = r->next_url; cap = sizeof r->next_url; }
    else return true;
    if (js->index[0] != r->which) { r->which = (uint8_t)js->index[0]; r->len = 0; }   // new string value
    size_t room = cap - 1 - r->len;
    if (len > room) len = room;
    memcpy(dst + r->len, data, len);
    r->len += len;
    dst[r->len] = 0;
    (void)final;
    return true;
}

static void reg_on_status(void *ctx, int status) { (void)ctx; rsp_status = status; reg_len = 0; }
static bool reg_on_body(void *ctx, const uint8_t *data, size_t len) {
    (void)ctx;
    if (rsp_status != 200) return true;
    size_t room = sizeof reg_body - 1 - reg_len;
    if (len > room) len = room;
    memcpy(reg_body + reg_len, data, len);
    reg_len += len;
    return true;
}

static void enter_backoff(const char *why) {
    consecutive_failures++;
    stat_errors++;
    strncpy(last_error, why, sizeof last_error - 1);
    uint32_t cap = BACKOFF_MIN_MS << (consecutive_failures > 6 ? 6 : consecutive_failures - 1);
    if (cap > BACKOFF_MAX_MS) cap = BACKOFF_MAX_MS;
    uint32_t delay = get_rand_32() % (cap + 1);            // full jitter
    if (delay < BACKOFF_MIN_MS / 2) delay = BACKOFF_MIN_MS / 2;
    next_request_ms = now_ms() + delay;
    printf("kiosk: %s; retry in %lu ms (failure %lu)\n", why, (unsigned long)delay, (unsigned long)consecutive_failures);
    if (consecutive_failures >= OFFLINE_AFTER_FAILURES && !overlay_offline && frame_valid) {
        overlay_offline = true;
        render_frame_now();
    }
}

// A radio that reports the link up while every request times out has wedged its driver (seen as an
// endless "[CYW43] STALL ... send_ethernet failed"). Nothing short of a reset recovers it, and a
// reboot resumes the cached next_url and the static set from flash within seconds. The count of
// such reboots survives the reset in a watchdog scratch register so a genuine outage (a server
// that accepts connections but never answers) backs off instead of reboot-looping.
#define WEDGE_TIMEOUTS 3
#define WEDGE_REBOOT_LIMIT 3
#define WEDGE_MAGIC 0x57454400u
static uint32_t consecutive_timeouts;
static uint32_t request_started_ms;   // when the current poll request began, for error logs

static void wedge_check(void) {
    if (consecutive_timeouts < WEDGE_TIMEOUTS || net_wifi_state() != WIFI_UP) return;
    uint32_t s = watchdog_hw->scratch[0];
    uint32_t n = (s & 0xffffff00u) == WEDGE_MAGIC ? (s & 0xffu) : 0u;
    if (n >= WEDGE_REBOOT_LIMIT) return;
    watchdog_hw->scratch[0] = WEDGE_MAGIC | (n + 1u);
    printf("kiosk: %lu timeouts in a row with the link up; resetting (recovery %lu of %u)\n",
           (unsigned long)consecutive_timeouts, (unsigned long)(n + 1u), WEDGE_REBOOT_LIMIT);
    watchdog_reboot(0, 0, 100);
    for (;;) tight_loop_contents();
}

// The video clock depends on the Wi-Fi channel (see video_mode_clock_khz), but the clock is set at
// boot and the channel is only known after joining. Remember the channel; when the access point is
// on a channel whose clock differs from the one running, save it and reboot so video comes back on
// a frequency that leaves the radio alone. At most two such reboots in a row (watchdog scratch 4),
// so an access point that changes channel on every join cannot reboot-loop the kiosk.
#define RETUNE_MAGIC 0x52544e00u
static void retune_for_channel(void) {
    int ch = net_wifi_channel();
    uint32_t s = watchdog_hw->scratch[4];
    uint32_t n = (s & 0xffffff00u) == RETUNE_MAGIC ? (s & 0xffu) : 0u;
    if (ch < 1 || ch > 14) return;
    uint32_t want = video_mode_clock_khz((video_mode_t)kiosk_config.video_mode, (uint8_t)ch);
    uint32_t have = (uint32_t)(clock_get_hz(clk_sys) / 1000u);
    if (kiosk_config.wifi_channel != (uint8_t)ch) {
        kiosk_config.wifi_channel = (uint8_t)ch;
        if (want == have) config_mark_dirty();
    }
    if (want == have) { if (n) watchdog_hw->scratch[4] = 0; return; }
    if (n >= 2) { printf("wifi: channel %d wants a %lu kHz video clock; already retuned twice, staying at %lu kHz\n", ch, (unsigned long)want, (unsigned long)have); return; }
    watchdog_hw->scratch[4] = RETUNE_MAGIC | (n + 1u);
    if (!config_save()) printf("wifi: warning: config save failed\n");
    printf("wifi: channel %d; moving the video clock from %lu to %lu kHz to keep DVI harmonics out of it (rebooting)\n", ch, (unsigned long)have, (unsigned long)want);
    watchdog_reboot(0, 0, 100);
    for (;;) tight_loop_contents();
}

static void reg_on_complete(void *ctx, int err, int status) {
    (void)ctx;
    request_active = false;
    if (err != HTTP_OK || status != 200) { enter_backoff(err == HTTP_OK ? "register: bad status" : "register failed"); return; }
    reg_parse_t r;
    memset(&r, 0, sizeof r);
    r.which = 0xff;
    json_stream_t js;
    json_stream_init(&js, reg_cb, &r);
    bool ok = json_stream_feed(&js, reg_body, reg_len) && json_stream_finish(&js);
    if (!ok || strlen(r.token) != 26) { enter_backoff("register: bad body"); return; }
    memcpy(kiosk_config.token, r.token, sizeof kiosk_config.token);
    memcpy(kiosk_config.device_id, r.id, sizeof kiosk_config.device_id);
    kiosk_config.next_url[0] = 0;
    kiosk_config.static_id[0] = 0;
    config_save();
    printf("kiosk: registered as %s\n", kiosk_config.device_id);
    consecutive_failures = 0;
    if (r.next_url[0]) strncpy(cur_url, r.next_url, sizeof cur_url - 1); else config_url(cur_url, sizeof cur_url);
    next_request_ms = now_ms();
    state = KS_POLL;
}

// ---- HTTP: frames ----------------------------------------------------------------------------

static void frame_on_status(void *ctx, int status) {
    (void)ctx;
    rsp_status = status;
    rsp_decoding = status == 200;
    if (rsp_decoding) {
        frame_decoder_init(&decoder, &frame, &pal, geom, scratch, sizeof scratch, mode->w, mode->h);
        frame_valid = false;   // frame_t is being overwritten; the pool keeps the last picture
    }
}

static bool frame_on_body(void *ctx, const uint8_t *data, size_t len) {
    (void)ctx;
    if (!rsp_decoding) return true;   // 304/404 bodies (if any) are irrelevant
    if (!frame_decoder_feed(&decoder, (const char *)data, len)) { rsp_decoding = false; return false; }
    return true;
}

static void frame_on_complete(void *ctx, int err, int status) {
    (void)ctx;
    request_active = false;
    uint32_t now = now_ms();
    if (err != HTTP_OK) {
        if (rsp_decoding) { frame_decoder_finish(&decoder); rsp_decoding = false; }
        consecutive_timeouts = err == HTTP_ERR_TIMEOUT ? consecutive_timeouts + 1u : 0u;
        if (err != HTTP_ERR_TIMEOUT) printf("kiosk: poll error %d after %lu ms\n", err, (unsigned long)(now - request_started_ms));
        enter_backoff(err == HTTP_ERR_TIMEOUT ? "poll timed out" : err == HTTP_ERR_URL ? "bad url" : "poll failed");
        wedge_check();
        if (err == HTTP_ERR_URL) { config_url(cur_url, sizeof cur_url); if (strncmp(cur_url, "https:", 6) == 0) kiosk_show_builtin(BUILTIN_NO_TLS, cur_url, NULL); }
        return;
    }
    consecutive_failures = 0;
    consecutive_timeouts = 0;
    netcheck_waiting = false;
    netcheck_asked_ms = 0;
    if (watchdog_hw->scratch[0]) watchdog_hw->scratch[0] = 0;   // the network works: recovery budget restored
    if (overlay_offline) overlay_offline = false;
    if (status == 304) { next_request_ms = now + POLL_FLOOR_MS; return; }
    if (status == 404) {
        // A 404 from a frame URL just means "start again from /config". A 404 from /config itself
        // means the server no longer knows this token (its registry was reset, or the kiosk was
        // deleted). Polling would never recover, so after a few in a row register afresh and show
        // a new pairing code.
        char cfg[KIOSK_MAX_URL];
        config_url(cfg, sizeof cfg);
        static uint8_t config_404s;
        if (strcmp(cur_url, cfg) != 0) { config_404s = 0; strcpy(cur_url, cfg); next_request_ms = now + POLL_FLOOR_MS; return; }
        if (++config_404s < 3) { next_request_ms = now + POLL_FLOOR_MS; return; }
        config_404s = 0;
        printf("kiosk: server does not know this kiosk; registering again\n");
        kiosk_config.token[0] = 0;
        kiosk_config.device_id[0] = 0;
        kiosk_config.next_url[0] = 0;
        kiosk_config.static_id[0] = 0;
        if (!config_save()) printf("kiosk: warning: config save failed\n");
        kiosk_show_builtin(BUILTIN_REGISTERING, NULL, NULL);
        state = KS_REGISTER;
        next_request_ms = now;
        return;
    }
    if (status != 200) { enter_backoff("unexpected status"); return; }

    uint32_t t0 = now_ms();
    frame_status_t st = rsp_decoding ? frame_decoder_finish(&decoder) : FD_ERR_JSON;
    rsp_decoding = false;
    stat_decode_ms = now_ms() - t0;
    switch (st) {
    case FD_OK:
        frame_valid = true;
        overlay_version = false;
        if (watchdog_hw->scratch[2] == 0x4e4f5244u) printf("kiosk: frame %lu bytes decoded, not drawn (norender)\n", (unsigned long)stat_decode_ms);
        else render_frame_now();
        if (frame.next_url[0]) strncpy(cur_url, frame.next_url, sizeof cur_url - 1); else config_url(cur_url, sizeof cur_url);
        cur_url[sizeof cur_url - 1] = 0;
        persist_next_url(cur_url, frame.static_id[0] ? frame.static_id : NULL);
        next_request_ms = now + (frame.next_ms > POLL_FLOOR_MS ? POLL_FLOOR_MS : POLL_FLOOR_MS);   // a request is free; next_ms is only a failsafe
        break;
    case FD_ERR_STATIC_MISSING:
        // The server assumed we cached a set we do not have: /config always restores it.
        config_url(cur_url, sizeof cur_url);
        next_request_ms = now + POLL_FLOOR_MS;
        printf("kiosk: static set %s missing, restarting from /config\n", frame.static_id);
        break;
    case FD_ERR_VERSION:
        overlay_version = true;
        enter_backoff("unsupported frame version");
        break;
    default:
        enter_backoff(st == FD_ERR_STATIC_STORE ? "static store failed" : "bad frame");
        break;
    }
}

static const http_sink_t reg_sink = { reg_on_status, NULL, reg_on_body, reg_on_complete };
static const http_sink_t frame_sink = { frame_on_status, NULL, frame_on_body, frame_on_complete };

// ---- finding a network ------------------------------------------------------------------------
//
// A kiosk that travels is plugged into a strange TV in a strange room, and the only thing it can
// assume is that nothing is where it was. So: scan, try whichever remembered network is actually
// in range strongest-first, and — this is the part that matters — put the setup AP up alongside
// the search once it has been fruitless for a while. Before this, a device whose network was not
// present sat on "Connecting…" for ever with no way in short of a USB cable, which for a device
// whose whole premise is "plug it into any TV" was the wrong way round.
//
// Nothing here ever clears the stored networks. The AP is an addition, not a reset: if the network
// reappears while someone is still looking for their phone, the kiosk simply joins and carries on.

#define JOIN_ATTEMPT_MS    20000u   // one network gets this long before the next is tried
#define JOIN_FALLBACK_MS   45000u   // fruitless for this long: raise the setup AP as well
#define JOIN_SCAN_EVERY_MS 12000u
// While the setup AP is up, a scan competes with the one thing a person in the room is trying to
// do. The portal asks for its own scan when the page is loaded, which is when a fresh list
// actually matters.
#define JOIN_SCAN_AP_MS    45000u
// cyw43 only (see net_wifi_ap_is_concurrent): that radio cannot hold the AP up and keep joining,
// so the two take turns. Long enough for someone to finish with the portal, short enough that a
// network coming back is noticed within a minute.
#define JOIN_AP_WINDOW_MS  60000u
#define JOIN_STA_WINDOW_MS 25000u

static uint32_t join_started_ms;
static uint32_t attempt_started_ms;
static int attempt_index = -1;          // into kiosk_config.nets; -1 = nothing in flight
static bool fallback_ap;                // the setup AP is up alongside the search
static uint32_t window_started_ms;      // cyw43 alternation
static bool window_is_ap;
static uint32_t last_scan_ms;
// Redraw only when the words would change. A checksum rather than a copy of the text: the lists
// differ at their tails as often as anywhere else, so a prefix comparison missed changes that a
// buffer big enough to hold the whole screen would have caught, and this board has no RAM to
// spare for one. 0 means "nothing drawn yet".
static uint32_t screen_sig;

// The remembered network with the strongest live signal. -1 when the scan saw none of them (or
// has not run yet, in which case the caller falls back to the most recently used).
static int best_known_in_range(void) {
    int best = -1;
    int16_t best_rssi = INT16_MIN;
    for (uint8_t i = 0; i < wifi_scan_count(); i++) {
        wifi_sighting_t s;
        if (!wifi_scan_get(i, &s)) continue;
        int at = config_net_find(s.ssid);
        if (at < 0) continue;
        if (s.rssi > best_rssi) { best_rssi = s.rssi; best = at; }
    }
    return best;
}

// "Gayle · Home · Pixel" — bounded, and honest about what it left out. A busy room can hold a
// dozen networks and naming them all would fill the screen with a list nobody reads, so the
// strongest few stand for the rest.
#define JOIN_LIST_MAX 6

static void join_list(char *out, size_t cap, bool saved) {
    size_t n = 0;
    out[0] = 0;
    unsigned count = saved ? kiosk_config.net_count : wifi_scan_count();
    for (unsigned i = 0; i < count; i++) {
        if (i == JOIN_LIST_MAX) { if (cap - n > 8) snprintf(out + n, cap - n, " · …"); return; }
        const char *ssid;
        if (saved) {
            ssid = kiosk_config.nets[i].ssid;
        } else {
            wifi_sighting_t s;
            if (!wifi_scan_get((uint8_t)i, &s)) break;
            ssid = s.ssid;
        }
        size_t need = strlen(ssid) + 3;
        if (n + need + 4 >= cap) { snprintf(out + n, cap - n, "%s…", n ? " · " : ""); return; }
        n += (size_t)snprintf(out + n, cap - n, "%s%s", n ? " · " : "", ssid);
    }
}

static void join_draw(void) {
    char saved[120], nearby[160], body[420];
    join_list(saved, sizeof saved, true);
    join_list(nearby, sizeof nearby, false);

    int n = 0;
    if (kiosk_config.net_count == 0) {
        n = snprintf(body, sizeof body, "No Wi-Fi networks saved yet");
    } else if (best_known_in_range() >= 0 || wifi_scan_age_ms() == UINT32_MAX) {
        n = snprintf(body, sizeof body, ">Trying %s\nSaved: %s",
                     attempt_index >= 0 ? kiosk_config.nets[attempt_index].ssid : saved, saved);
    } else {
        n = snprintf(body, sizeof body, ">None of your networks is in range\nSaved: %s", saved);
    }
    if (n > 0 && nearby[0] && (size_t)n < sizeof body)
        snprintf(body + n, sizeof body - (size_t)n, "\nNearby: %s", nearby);

    // Redraw only on a change: this runs every poll, and re-rendering 1080p for the same words
    // would eat the frame budget for nothing.
    uint32_t sig = crc32_update(0xFFFFFFFFu, body, strlen(body));
    if (sig == screen_sig) return;
    screen_sig = sig;
    kiosk_show_builtin(BUILTIN_WIFI_SETUP, fallback_ap ? provision_ap_ssid() : "", body);
}

static void join_reset(void) {
    join_started_ms = now_ms();
    attempt_started_ms = 0;
    attempt_index = -1;
    last_scan_ms = 0;
    window_started_ms = join_started_ms;
    window_is_ap = false;
    screen_sig = 0;
}

static void join_begin_attempt(int index) {
    if (index < 0 || index >= (int)kiosk_config.net_count) return;
    attempt_index = index;
    attempt_started_ms = now_ms();
    printf("wifi: trying \"%s\"\n", kiosk_config.nets[index].ssid);
    net_wifi_connect(kiosk_config.nets[index].ssid, kiosk_config.nets[index].pass);
}

// Called once a station link is up: remember which network worked, so the list orders itself and
// the next power-on tries the right one first.
static void join_succeeded(void) {
    if (attempt_index > 0) {
        config_net_promote(attempt_index);
        config_mark_dirty();
    }
    attempt_index = -1;
    if (fallback_ap) {
        printf("wifi: joined; taking the setup AP down\n");
        provision_stop();
        fallback_ap = false;
    }
    screen_sig = 0;
}

// The search, one step per main-loop pass. Returns true once the station is up.
static bool join_poll(void) {
    uint32_t now = now_ms();
    if (net_wifi_state() == WIFI_UP) { join_succeeded(); return true; }
    if (kiosk_config.net_count == 0) { join_draw(); return false; }

    bool sta_allowed = true;
    if (fallback_ap && !net_wifi_ap_is_concurrent()) {
        // Take turns. Never interrupt someone who is mid-way through the portal.
        uint32_t window = window_is_ap ? JOIN_AP_WINDOW_MS : JOIN_STA_WINDOW_MS;
        if (now - window_started_ms >= window && !(window_is_ap && provision_busy())) {
            window_is_ap = !window_is_ap;
            window_started_ms = now;
            if (window_is_ap) provision_start(true);
            else { provision_stop(); attempt_index = -1; }
        }
        sta_allowed = !window_is_ap;
    }

    // Scan on a timer so the picture of the room stays current while the kiosk hunts — but only
    // between attempts, and only while the station side has the radio. A radio that is mid-join
    // refuses to scan (esp_wifi returns ESP_ERR_WIFI_STATE), and since a fruitless search is a
    // continuous stream of attempts, asking at the wrong moment would mean never scanning at all
    // in the one case that needs it. A scan visits every channel, so on a single-radio build it
    // takes the setup AP off the air for as long as it runs: someone would be hunting for
    // TVTOP-xxxx in their phone's list during the seconds it stops beaconing. While that AP is up
    // the nearby list is worth less than the AP being findable, so it goes slowly and never
    // during the AP's own turn.
    uint32_t scan_every = fallback_ap ? JOIN_SCAN_AP_MS : JOIN_SCAN_EVERY_MS;
    bool sta_idle = net_wifi_state() != WIFI_CONNECTING;
    if (sta_allowed && sta_idle && !wifi_scan_running() && (last_scan_ms == 0 || now - last_scan_ms >= scan_every)) {
        last_scan_ms = now;
        wifi_scan_request();
    }

    if (sta_allowed) {
        int want = best_known_in_range();
        bool stale = attempt_index < 0 || (attempt_started_ms && now - attempt_started_ms >= JOIN_ATTEMPT_MS);
        if (stale) {
            if (want < 0) {
                // Nothing known is in range. Keep cycling the list anyway: a hidden SSID never
                // shows up in a scan, and a scan can miss a network that is really there.
                int next = attempt_index < 0 ? 0 : (attempt_index + 1) % (int)kiosk_config.net_count;
                join_begin_attempt(next);
            } else if (want != attempt_index) {
                join_begin_attempt(want);
            } else {
                attempt_started_ms = now;   // give the same (and best) network another go
                net_wifi_connect(kiosk_config.nets[want].ssid, kiosk_config.nets[want].pass);
            }
        }
    }

    if (!fallback_ap && now - join_started_ms >= JOIN_FALLBACK_MS) {
        fallback_ap = true;
        window_is_ap = !net_wifi_ap_is_concurrent();
        window_started_ms = now;
        printf("wifi: no luck after %u s; bringing up the setup AP as well\n", (unsigned)(JOIN_FALLBACK_MS / 1000u));
        provision_start(true);
        screen_sig = 0;
    }

    join_draw();
    return false;
}

// ---- joined, but is anything out there? -------------------------------------------------------
//
// Failing polls with the link up have two very different causes, and telling the user the wrong one
// wastes their time. Either the kiosk server is unreachable — nothing they can do — or this
// network wants a browser sign-in it will never get, in which case the answer is "use another
// network" and the kiosk should be showing them how.
#define NETCHECK_AFTER_FAILURES 2
#define NETCHECK_EVERY_MS 60000u

static void netcheck_poll(void) {
    uint32_t now = now_ms();
    if (consecutive_failures >= NETCHECK_AFTER_FAILURES && net_wifi_state() == WIFI_UP &&
        (netcheck_asked_ms == 0 || now - netcheck_asked_ms >= NETCHECK_EVERY_MS) &&
        !netcheck_waiting) {
        netcheck_asked_ms = now;
        netcheck_waiting = true;
        net_wifi_check_start();
    }

    // Only a verdict this loop asked for is acted on. The result is sticky, so without this a
    // console probe, or a poll that then succeeded, would re-trigger the whole thing every pass.
    if (!netcheck_waiting) return;
    net_check_t v = net_wifi_check_result();
    if (v == NET_CHECK_IDLE || v == NET_CHECK_PENDING || v == NET_CHECK_UNSUPPORTED) return;
    netcheck_waiting = false;

    if (v == NET_CHECK_ONLINE) {
        // The network is fine, so this is ours to fix, not theirs. Say nothing new: the offline
        // badge over the last frame already covers it.
        printf("net: internet reachable; the kiosk server is not\n");
        return;
    }

    const char *ssid = attempt_index >= 0 ? kiosk_config.nets[attempt_index].ssid
                     : kiosk_config.net_count ? kiosk_config.nets[0].ssid : "this network";
    const char *body =
        v == NET_CHECK_CAPTIVE
            ? ">This network needs a browser sign-in\n"
              "The kiosk has no browser, and the sign-in is tied to\n"
              "the device asking — so a phone cannot do it for it.\n"
              "A phone hotspot is the reliable way round this."
        : v == NET_CHECK_NO_DNS
            ? ">Joined, but names do not resolve\n"
              "Often a sign-in page waiting on the other side."
            : ">Joined, but nothing answers\n"
              "The network has no route to the internet.";
    printf("net: %s\n", v == NET_CHECK_CAPTIVE ? "captive portal detected" : "no internet on this network");
    kiosk_show_builtin(BUILTIN_NO_INTERNET, ssid, body);
    frame_valid = false;   // the overlay has nothing to sit on now

    // Give them the means as well as the diagnosis: with the setup AP up they can switch to a
    // hotspot without hunting for a laptop.
    if (!fallback_ap) {
        fallback_ap = true;
        provision_start(true);
    }
}

// ---- state machine ---------------------------------------------------------------------------

static void start_poll_state(void) {
    // Resume where we were if the persisted URL is usable: a URL that names a static set needs
    // that set to still be in flash. Resume at rev=0, though: the saved revision is the last frame
    // drawn before a reboot or reconnect, and in a quiet game the server would hold that request
    // open with nothing new to send, leaving a cleared or connecting screen up until someone moved.
    // rev=0 answers at once with the current frame; the static set id still avoids a map download.
    if (kiosk_config.next_url[0] && (!url_has_static(kiosk_config.next_url) || geom_store_is_open(geom))) {
        char base[KIOSK_MAX_URL];
        strip_rev(kiosk_config.next_url, base, sizeof base);
        snprintf(cur_url, sizeof cur_url, "%s%crev=0", base, strchr(base, '?') ? '&' : '?');
    } else {
        config_url(cur_url, sizeof cur_url);
    }
    cur_url[sizeof cur_url - 1] = 0;
    next_request_ms = now_ms();
    state = KS_POLL;
}

void kiosk_loop_init_display_only(void) {
    mode = video_mode_info((video_mode_t)kiosk_config.video_mode);
    palette_init(&pal);
    font_init(font_blob, font_blob_size);
    geom = geom_flash_get();
    geom_store_set_resolution(geom, mode->w, mode->h);   // a set cached for another mode must not open
}

void kiosk_loop_init(void) {
    mode = video_mode_info((video_mode_t)kiosk_config.video_mode);
    palette_init(&pal);
    font_init(font_blob, font_blob_size);
    geom = geom_flash_get();
    geom_store_set_resolution(geom, mode->w, mode->h);   // a set cached for another mode must not open
    if (kiosk_config.static_id[0]) {
        if (geom_store_open(geom, kiosk_config.static_id)) printf("kiosk: static set %s restored from flash\n", kiosk_config.static_id);
    }
    http_client_init(http_client_get(), ca_certs_pem, CA_CERTS_PEM_LEN);
    state = KS_BOOT;
}

void kiosk_loop_restart(void) {
    fallback_ap = false;
    netcheck_waiting = false;
    netcheck_asked_ms = 0;
    http_cancel(http_client_get());
    request_active = false;
    rsp_decoding = false;
    consecutive_failures = 0;
    overlay_offline = overlay_version = false;
    if (provision_active()) provision_stop();
    state = KS_BOOT;
}

void kiosk_loop_poll(void) {
    uint32_t now = now_ms();
    switch (state) {
    case KS_BOOT:
        // A kiosk that has never been told about a network goes straight to the setup AP, as
        // before. One that has is handed to the join manager, which will raise the AP itself if it
        // cannot find anything — so there is no longer a path that ends with no way in.
        if (kiosk_config.net_count == 0) { provision_start(false); state = KS_PROVISION; return; }
        join_reset();
        state = KS_WIFI_WAIT;
        return;
    case KS_PROVISION:
        return;   // the portal or console will restart us
    case KS_WIFI_WAIT:
        if (!join_poll()) return;
        net_wifi_led(true);
        retune_for_channel();
        if (!kiosk_config.token[0]) {
            kiosk_show_builtin(BUILTIN_REGISTERING, NULL, NULL);
            state = KS_REGISTER;
            next_request_ms = now;
        } else {
            start_poll_state();
        }
        return;
    case KS_REGISTER:
        if (net_wifi_state() != WIFI_UP) { http_cancel(http_client_get()); request_active = false; join_reset(); state = KS_WIFI_WAIT; return; }
        if (request_active || (int32_t)(now - next_request_ms) < 0) return;
        {
            char url[KIOSK_MAX_URL];
            register_url(url, sizeof url);
            int rc = http_get(http_client_get(), url, &reg_sink, NULL, REGISTER_TIMEOUT_MS);
            if (rc == HTTP_OK) request_active = true;
            else if (rc == HTTP_ERR_URL) { kiosk_show_builtin(BUILTIN_NO_TLS, url, NULL); state = KS_HALT; }
            else enter_backoff("register: cannot start");
        }
        return;
    case KS_POLL:
        netcheck_poll();
        if (net_wifi_state() != WIFI_UP) {
            if (request_active) { http_cancel(http_client_get()); request_active = false; rsp_decoding = false; }
            net_wifi_led(false);
            join_reset();
            state = KS_WIFI_WAIT;
            return;
        }
        if (request_active || (int32_t)(now - next_request_ms) < 0) return;
        {
            int rc = http_get(http_client_get(), cur_url, &frame_sink, NULL, LONG_POLL_TIMEOUT_MS);
            if (rc == HTTP_OK) { request_active = true; request_started_ms = now; }
            else if (rc == HTTP_ERR_URL) {
                // https without TLS support, or a URL we cannot parse: fall back to the config
                // endpoint once, then halt on a screen that says what is wrong.
                char cfg[KIOSK_MAX_URL];
                config_url(cfg, sizeof cfg);
                if (strcmp(cfg, cur_url) != 0) { strcpy(cur_url, cfg); enter_backoff("bad next_url"); }
                else { kiosk_show_builtin(BUILTIN_NO_TLS, cur_url, NULL); state = KS_HALT; }
            } else enter_backoff("cannot start request");
        }
        return;
    case KS_HALT:
        return;
    }
}

void kiosk_loop_status(char *buf, size_t cap) {
    static const char *names[] = { "boot", "provision", "wifi-wait", "register", "poll", "halt" };
    char masked[KIOSK_MAX_URL];
    // Never print the token: mask everything after "t=" or "/frame/" up to the next delimiter.
    strncpy(masked, cur_url, sizeof masked - 1); masked[sizeof masked - 1] = 0;
    for (char *p = masked; *p; p++) {
        if ((p[0] == 't' && p[1] == '=' && (p == masked || p[-1] == '?' || p[-1] == '&')) || strncmp(p, "/frame/", 7) == 0) {
            p += p[0] == 't' ? 2 : 7;
            while (*p && *p != '&' && *p != '?' && *p != '/') *p++ = '*';
            p--;
        }
    }
    snprintf(buf, cap,
             "state=%s url=%s active=%d failures=%lu last_error=%s\n"
             "frames=%lu decode_ms=%lu render_ms=%lu pool_bytes=%lu/%lu pool_fail=%lu palette=%u static=%s\n"
             "video=%s late_lines=%lu vframes=%lu max_line_cycles=%lu",
             names[state], masked, request_active, (unsigned long)consecutive_failures, last_error[0] ? last_error : "-",
             (unsigned long)stat_frames, (unsigned long)stat_decode_ms, (unsigned long)stat_render_ms, (unsigned long)stat_pool_bytes,
             (unsigned long)kiosk_linepool.size, (unsigned long)kiosk_linepool.stats_alloc_fail,
             pal.count, geom_store_is_open(geom) ? geom_store_id(geom) : "-",
             mode ? mode->name : "?", (unsigned long)scanout_late_lines(), (unsigned long)scanout_frames(), (unsigned long)scanout_max_line_cycles());
}
