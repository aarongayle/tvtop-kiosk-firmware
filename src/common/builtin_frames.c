// The screens firmware draws itself, expressed as protocol-v3 frames so the one decoder and
// renderer draw everything. Colours are the app's own (see helpers/Kiosk/scene.js).
#include <stdio.h>
#include <string.h>
#include "builtin_frames.h"
#include "font.h"
#include "kiosk_config.h"

#define BG "#0d1b2a"
#define INK "#f8f4ec"
#define DIM "#9fb3c8"
#define FAINT "#6b8199"
#define ACCENT "#ffcc66"

// JSON-escapes the first len bytes of s into out (cap includes the NUL). Non-ASCII passes through
// as UTF-8.
static size_t json_escape_n(const char *s, size_t len, char *out, size_t cap) {
    size_t n = 0;
    if (!s) len = 0;
    for (size_t i = 0; i < len && s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        const char *rep = NULL;
        char hex[7];
        switch (c) {
            case '"': rep = "\\\""; break;
            case '\\': rep = "\\\\"; break;
            case '\n': rep = "\\n"; break;
            case '\r': rep = "\\r"; break;
            case '\t': rep = "\\t"; break;
            default:
                if (c < 0x20) { snprintf(hex, sizeof hex, "\\u%04x", c); rep = hex; }
        }
        size_t rlen = rep ? strlen(rep) : 1;
        if (n + rlen + 1 > cap) break;
        if (rep) memcpy(out + n, rep, rlen); else out[n] = (char)c;
        n += rlen;
    }
    out[n] = 0;
    return n;
}

static size_t json_escape(const char *s, char *out, size_t cap) {
    return json_escape_n(s, s ? strlen(s) : 0, out, cap);
}

static const char *icon_row(void) {
    // All ten icons in a row, for the test pattern.
    return "[\"i\",140,560,60,\"crown\",\"" ACCENT "\"],[\"i\",240,560,60,\"dice\",\"" INK "\"],"
           "[\"i\",340,560,60,\"star\",\"" ACCENT "\"],[\"i\",440,560,60,\"check\",\"#4caf50\"],"
           "[\"i\",540,560,60,\"cross\",\"#f04d24\"],[\"i\",640,560,60,\"clock\",\"" INK "\"],"
           "[\"i\",740,560,60,\"person\",\"" DIM "\"],[\"i\",840,560,60,\"cast\",\"" INK "\"],"
           "[\"i\",940,560,60,\"wifi\",\"" INK "\"],[\"i\",1040,560,60,\"warning\",\"" ACCENT "\"]";
}

// The canvas every built-in frame declares, and how much of its width a stacked line may use.
// A television crops a little of each edge, so centred text is kept well inside the full 1280.
#define CANVAS_W    1280
#define STACK_MAX_W 1100

// Advance width of a UTF-8 run in px8 at size_px, regular weight. font_measure answers 0 when the
// font blob has not been initialised; half an em per byte then stands in for it, which
// over-estimates and so wraps early rather than running off the screen.
static int32_t run_width8(const char *s, size_t len, int size, bool have_font) {
    return have_font ? font_measure(s, len, size, false) : (int32_t)len * size * 4;
}

// How many bytes of s go on one row at most STACK_MAX_W wide. Breaks at the last space that fits,
// or, for a single word wider than the screen, hard-cuts it on a UTF-8 boundary. Never returns 0
// for a non-empty string, so the caller always makes progress.
static size_t wrap_take(const char *s, int size, bool have_font) {
    const int32_t max_w8 = STACK_MAX_W * 8;
    size_t len = strlen(s), i = 0, fits = 0, last_space = 0;
    int32_t w = 0;
    while (i < len) {
        size_t j = i + 1;
        while (j < len && ((unsigned char)s[j] & 0xC0) == 0x80) j++;   // one UTF-8 character
        w += run_width8(s + i, j - i, size, have_font);
        if (w > max_w8) break;
        if (s[i] == ' ') last_space = i;
        i = fits = j;
    }
    if (i >= len) return len;
    size_t take = last_space ? last_space : (fits ? fits : 1);
    while (take > 0 && s[take - 1] == ' ') take--;
    // A separator left dangling at the break ("Home · Office ·") reads as a mistake: break before
    // it instead.
    if (take >= 2 && (unsigned char)s[take - 2] == 0xC2 && (unsigned char)s[take - 1] == 0xB7) {
        take -= 2;
        while (take > 0 && s[take - 1] == ' ') take--;
    }
    return take ? take : (fits ? fits : 1);
}

// Emits one centred text op per line of a newline-separated block, stepping down the screen and
// stopping before max_y so the block cannot walk into whatever the frame draws below it. Lines are
// escaped individually; a line starting with '>' is drawn in the accent colour, which is how the
// caller marks the thing it wants read first. A line too wide for the canvas is wrapped onto
// further rows: the list of networks in a room is as long as the room is busy, and an unwrapped
// one ran off both edges of the television.
static size_t line_stack(char *out, size_t cap, const char *text, int y, int step, int size, int max_y) {
    size_t n = 0;
    const char *p = text;
    bool have_font = font_measure("n", 1, size, false) > 0;
    out[0] = 0;
    while (*p && y <= max_y) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char raw[200], esc[512];
        if (len >= sizeof raw) {
            len = sizeof raw - 1;
            while (len && ((unsigned char)p[len] & 0xC0) == 0x80) len--;   // never split a character
        }
        memcpy(raw, p, len);
        raw[len] = 0;
        const char *body = raw;
        const char *colour = DIM;
        if (raw[0] == '>') { body = raw + 1; colour = ACCENT; }
        do {
            size_t take = wrap_take(body, size, have_font);
            json_escape_n(body, take, esc, sizeof esc);
            int w = snprintf(out + n, cap - n, "%s[\"t\",%d,%d,\"%s\",%d,\"%s\",1]",
                             n ? "," : "", CANVAS_W / 2, y, esc, size, colour);
            if (w < 0 || (size_t)w >= cap - n) { out[n] = 0; return n; }
            n += (size_t)w;
            y += step;
            body += take;
            while (*body == ' ') body++;
            // A row that begins "· Office" reads as a bullet nobody asked for: the separator
            // belongs between two names on one row, not at the start of the next.
            if ((unsigned char)body[0] == 0xC2 && (unsigned char)body[1] == 0xB7) {
                body += 2;
                while (*body == ' ') body++;
            }
        } while (*body && y <= max_y);
        if (!nl) break;
        p = nl + 1;
    }
    out[n] = 0;
    return n;
}

size_t builtin_frame_json(builtin_frame_t which, char *buf, size_t cap, const char *arg1, const char *arg2) {
    char a1[300], a2[300];
    json_escape(arg1, a1, sizeof a1);
    json_escape(arg2, a2, sizeof a2);
    int n = -1;
    switch (which) {
    case BUILTIN_PROVISION:
        n = snprintf(buf, cap,
            "{\"v\":3,\"next_url\":null,\"next_ms\":0,\"w\":1280,\"h\":720,\"bg\":\"" BG "\",\"ops\":["
            "[\"i\",604,60,72,\"wifi\",\"" DIM "\"],"
            "[\"t\",640,210,\"Set up Wi-Fi\",52,\"" INK "\",1,1],"
            "[\"t\",640,300,\"On your phone, join the Wi-Fi network\",30,\"" DIM "\",1],"
            "[\"t\",640,372,\"%s\",56,\"" ACCENT "\",1,1],"
            "[\"t\",640,450,\"then open\",30,\"" DIM "\",1],"
            "[\"t\",640,512,\"%s\",40,\"" INK "\",1,1],"
            "[\"t\",640,650,\"USB serial: wifi <ssid> <password>   ·   server <url>\",22,\"" FAINT "\",1]"
            "]}", a1, a2);
        break;
    case BUILTIN_CONNECTING:
        n = snprintf(buf, cap,
            "{\"v\":3,\"next_url\":null,\"next_ms\":0,\"w\":1280,\"h\":720,\"bg\":\"" BG "\",\"ops\":["
            "[\"i\",604,220,72,\"cast\",\"" DIM "\"],"
            "[\"t\",640,360,\"Connecting to\",32,\"" DIM "\",1],"
            "[\"t\",640,430,\"%s\",52,\"" INK "\",1,1],"
            "[\"t\",640,650,\"TV-Top Kiosk " KIOSK_FW_VERSION "\",22,\"" FAINT "\",1]"
            "]}", a1);
        break;
    case BUILTIN_WIFI_SETUP: {
        // One screen covers both halves of the problem: a kiosk that has never been set up, and one
        // that travelled and cannot find anything it knows. The difference is only whether there is
        // an AP to join yet, so the same layout serves and nothing flashes between states.
        char lines[1400];
        // With the AP's instructions below, the block has the rows from 300 to 520; without them
        // it has the screen down to the version line.
        line_stack(lines, sizeof lines, arg2, 300, 46, 28, arg1[0] ? 520 : 620);
        if (arg1[0]) {
            n = snprintf(buf, cap,
                "{\"v\":3,\"next_url\":null,\"next_ms\":0,\"w\":1280,\"h\":720,\"bg\":\"" BG "\",\"ops\":["
                "[\"i\",604,50,64,\"wifi\",\"" DIM "\"],"
                "[\"t\",640,190,\"Set up Wi-Fi\",48,\"" INK "\",1,1],"
                "%s%s"
                "[\"t\",640,560,\"On your phone join\",26,\"" DIM "\",1],"
                "[\"t\",640,616,\"%s\",44,\"" ACCENT "\",1,1],"
                "[\"t\",640,672,\"then open http://192.168.4.1\",24,\"" FAINT "\",1]"
                "]}", lines, lines[0] ? "," : "", a1);
        } else {
            n = snprintf(buf, cap,
                "{\"v\":3,\"next_url\":null,\"next_ms\":0,\"w\":1280,\"h\":720,\"bg\":\"" BG "\",\"ops\":["
                "[\"i\",604,50,64,\"wifi\",\"" DIM "\"],"
                "[\"t\",640,190,\"Looking for Wi-Fi\",48,\"" INK "\",1,1],"
                "%s%s"
                "[\"t\",640,672,\"TV-Top Kiosk " KIOSK_FW_VERSION "\",22,\"" FAINT "\",1]"
                "]}", lines, lines[0] ? "," : "");
        }
        break;
    }
    case BUILTIN_NO_INTERNET: {
        char lines[1000];
        line_stack(lines, sizeof lines, arg2, 400, 46, 28, 620);
        n = snprintf(buf, cap,
            "{\"v\":3,\"next_url\":null,\"next_ms\":0,\"w\":1280,\"h\":720,\"bg\":\"" BG "\",\"ops\":["
            "[\"i\",604,80,72,\"warning\",\"" ACCENT "\"],"
            "[\"t\",640,230,\"No internet on this network\",46,\"" INK "\",1,1],"
            "[\"t\",640,310,\"%s\",38,\"" ACCENT "\",1,1],"
            "%s%s"
            "[\"t\",640,672,\"Join TVTOP setup Wi-Fi to pick another network\",22,\"" FAINT "\",1]"
            "]}", a1, lines, lines[0] ? "," : "");
        break;
    }
    case BUILTIN_REGISTERING:
        n = snprintf(buf, cap,
            "{\"v\":3,\"next_url\":null,\"next_ms\":0,\"w\":1280,\"h\":720,\"bg\":\"" BG "\",\"ops\":["
            "[\"t\",640,380,\"Registering with TV-Top…\",48,\"" INK "\",1,1],"
            "[\"t\",640,650,\"TV-Top Kiosk " KIOSK_FW_VERSION "\",22,\"" FAINT "\",1]"
            "]}");
        break;
    case BUILTIN_NO_TLS:
        n = snprintf(buf, cap,
            "{\"v\":3,\"next_url\":null,\"next_ms\":0,\"w\":1280,\"h\":720,\"bg\":\"" BG "\",\"ops\":["
            "[\"i\",604,140,72,\"warning\",\"" ACCENT "\"],"
            "[\"t\",640,290,\"This build cannot fetch https URLs\",44,\"" INK "\",1,1],"
            "[\"t\",640,360,\"%s\",24,\"" DIM "\",1],"
            "[\"t\",640,450,\"Point the kiosk at an http:// server (USB serial: server <url>)\",28,\"" DIM "\",1],"
            "[\"t\",640,500,\"or build the firmware with -DKIOSK_TLS=ON\",28,\"" DIM "\",1]"
            "]}", a1);
        break;
    case BUILTIN_SOLID:
        // Three wide bands and nothing else: every line is one to three runs, so the encoder is
        // idle most of each scanline. If a display drops out on this, the cause is the signal or
        // the clock, not encoding speed.
        n = snprintf(buf, cap,
            "{\"v\":3,\"next_url\":null,\"next_ms\":0,\"w\":1280,\"h\":720,\"bg\":\"#1e50b4\",\"ops\":["
            "[\"r\",0,240,1280,240,\"#f0f0f0\"],[\"r\",0,480,1280,240,\"#20a040\"]"
            "]}");
        break;
    case BUILTIN_LABEL:
        n = snprintf(buf, cap,
            "{\"v\":3,\"next_url\":null,\"next_ms\":0,\"w\":1280,\"h\":720,\"bg\":\"#1e50b4\",\"ops\":["
            "[\"r\",0,240,1280,240,\"#f0f0f0\"],[\"r\",0,480,1280,240,\"#20a040\"],"
            "[\"t\",640,160,\"%s\",96,\"#ffffff\",1,1]"
            "]}", a1);
        break;
    case BUILTIN_TEST_PATTERN: {
        // Colour bars across the top half, a grey ramp below, a 1 px border for overscan checks.
        static const char *bars[8] = { "#ffffff", "#ffff00", "#00ffff", "#00ff00", "#ff00ff", "#ff0000", "#0000ff", "#000000" };
        size_t used = (size_t)snprintf(buf, cap, "{\"v\":3,\"next_url\":null,\"next_ms\":0,\"w\":1280,\"h\":720,\"bg\":\"#202020\",\"ops\":[");
        for (int i = 0; i < 8 && used < cap; i++)
            used += (size_t)snprintf(buf + used, cap - used, "[\"r\",%d,0,160,320,\"%s\"],", i * 160, bars[i]);
        for (int i = 0; i < 12 && used < cap; i++) {
            int v = (i * 255) / 11;
            used += (size_t)snprintf(buf + used, cap - used, "[\"r\",%d,330,%d,120,\"#%02x%02x%02x\"],", 40 + i * 100, 100, v, v, v);
        }
        used += (size_t)snprintf(buf + used, cap - used,
            "[\"r\",0,0,1280,1,\"#ffffff\"],[\"r\",0,719,1280,1,\"#ffffff\"],[\"r\",0,0,1,720,\"#ffffff\"],[\"r\",1279,0,1,720,\"#ffffff\"],"
            "[\"c\",640,510,28,\"" ACCENT "\"],"
            "[\"t\",640,500,\"TV-Top Kiosk 1280×720\",48,\"" INK "\",1,1],"
            "[\"t\",640,540,\"firmware " KIOSK_FW_VERSION " · the yellow disc sits under this text\",22,\"" DIM "\",1],"
            "%s,[\"t\",640,690,\"abcdefghijklmnopqrstuvwxyz ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789\",26,\"" INK "\",1]"
            "]}", icon_row());
        n = used < cap ? (int)used : -1;
        break;
    }
    }
    if (n < 0 || (size_t)n >= cap) { if (cap) buf[0] = 0; return 0; }
    return (size_t)n;
}
