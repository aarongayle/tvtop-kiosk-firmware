// The protocol icons. Path data is copied verbatim from ICON_PATHS in tvtop-expo's SceneCanvas.js —
// that file is the reference the device must match, so the strings are not "cleaned up" here.
#include "icons.h"
#include <stddef.h>

const icon_def_t icon_defs[ICON_COUNT] = {
    { "crown", "M4 17h16l1.6-9.6-5.9 4.8L12 4.5 8.3 12.2 2.4 7.4z M4 19h16v2.2H4z", false, false },
    { "star", "M12 2.5l2.9 5.9 6.5.9-4.7 4.6 1.1 6.5L12 17.3l-5.8 3.1 1.1-6.5L2.6 9.3l6.5-.9z", false, false },
    { "check", "M9.3 18.2L3.5 12.4l2.1-2.1 3.7 3.7 9-9 2.1 2.1z", false, false },
    { "cross", "M19 6.4L17.6 5 12 10.6 6.4 5 5 6.4 10.6 12 5 17.6 6.4 19 12 13.4 17.6 19 19 17.6 13.4 12z", false, false },
    { "person", "M12 12a4.5 4.5 0 1 0 0-9 4.5 4.5 0 0 0 0 9zm0 2c-4.4 0-8 2.2-8 5v2h16v-2c0-2.8-3.6-5-8-5z", false, false },
    { "warning", "M12 2.8l10.2 18.4H1.8zM11 9h2v6h-2zm0 7.6h2v2.2h-2z", false, true },
    { "dice",
      "M4.5 3h15A1.5 1.5 0 0 1 21 4.5v15a1.5 1.5 0 0 1-1.5 1.5h-15A1.5 1.5 0 0 1 3 19.5v-15A1.5 1.5 0 0 1 4.5 3z"
      " M7.5 6a1.5 1.5 0 1 0 0 3 1.5 1.5 0 0 0 0-3z M16.5 6a1.5 1.5 0 1 0 0 3 1.5 1.5 0 0 0 0-3z"
      " M12 10.5a1.5 1.5 0 1 0 0 3 1.5 1.5 0 0 0 0-3z M7.5 15a1.5 1.5 0 1 0 0 3 1.5 1.5 0 0 0 0-3z"
      " M16.5 15a1.5 1.5 0 1 0 0 3 1.5 1.5 0 0 0 0-3z",
      false, true },
    // Stroked rather than filled — a ring with something inside it cannot be expressed as one fill.
    { "clock", "M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18z M12 7v5.4l3.6 2.1", true, false },
    { "cast", "M3 8V5h18v14h-7 M2 20h3a3 3 0 0 0-3-3v3 M2 14a6 6 0 0 1 6 6 M2 9a11 11 0 0 1 11 11", true, false },
    { "wifi", "M3 9.5a13 13 0 0 1 18 0 M6.6 13a8 8 0 0 1 10.8 0 M10 16.4a3.5 3.5 0 0 1 4 0 M12 20h.01", true, false },
};

int icon_lookup(const char *name, size_t len) {
    if (!name) return -1;
    for (int i = 0; i < ICON_COUNT; i++) {
        const char *n = icon_defs[i].name;
        size_t k = 0;
        while (k < len && n[k] && n[k] == name[k]) k++;
        if (k == len && n[k] == 0) return i;
    }
    return -1;
}
