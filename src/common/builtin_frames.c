// The screens firmware draws itself, expressed as protocol-v3 frames so the one decoder and
// renderer draw everything. Colours are the app's own (see helpers/Kiosk/scene.js).
#include <stdio.h>
#include <string.h>
#include "builtin_frames.h"
#include "kiosk_config.h"

#define BG "#0d1b2a"
#define INK "#f8f4ec"
#define DIM "#9fb3c8"
#define FAINT "#6b8199"
#define ACCENT "#ffcc66"

// JSON-escapes s into out (cap includes the NUL). Non-ASCII passes through as UTF-8.
static size_t json_escape(const char *s, char *out, size_t cap) {
    size_t n = 0;
    if (!s) s = "";
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
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
        size_t len = rep ? strlen(rep) : 1;
        if (n + len + 1 > cap) break;
        if (rep) memcpy(out + n, rep, len); else out[n] = (char)c;
        n += len;
    }
    out[n] = 0;
    return n;
}

static const char *icon_row(void) {
    // All ten icons in a row, for the test pattern.
    return "[\"i\",140,560,60,\"crown\",\"" ACCENT "\"],[\"i\",240,560,60,\"dice\",\"" INK "\"],"
           "[\"i\",340,560,60,\"star\",\"" ACCENT "\"],[\"i\",440,560,60,\"check\",\"#4caf50\"],"
           "[\"i\",540,560,60,\"cross\",\"#f04d24\"],[\"i\",640,560,60,\"clock\",\"" INK "\"],"
           "[\"i\",740,560,60,\"person\",\"" DIM "\"],[\"i\",840,560,60,\"cast\",\"" INK "\"],"
           "[\"i\",940,560,60,\"wifi\",\"" INK "\"],[\"i\",1040,560,60,\"warning\",\"" ACCENT "\"]";
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
