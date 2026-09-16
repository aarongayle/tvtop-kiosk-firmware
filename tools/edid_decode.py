#!/usr/bin/env python3
"""Decodes the EDIDHEX<n> lines printed by the kiosk console's `edid` command.

    python3 tools/edid_decode.py capture.txt     (or pipe the console output in)
"""
import re, sys

def dtd(d, where):
    if d[0] == 0 and d[1] == 0:
        tag = d[3]
        text = bytes(d[5:18]).split(b"\n")[0].decode(errors="replace").strip()
        if tag == 0xFC: print(f"{where}: monitor name {text}")
        elif tag == 0xFD: print(f"{where}: range limits V {d[5]}-{d[6]} Hz, H {d[7]}-{d[8]} kHz, max pixel clock {d[9]*10} MHz")
        elif tag == 0xFF: print(f"{where}: serial {text}")
        return
    pclk = (d[0] | d[1] << 8) * 10
    ha = d[2] | (d[4] >> 4) << 8; hb = d[3] | (d[4] & 15) << 8
    va = d[5] | (d[7] >> 4) << 8; vb = d[6] | (d[7] & 15) << 8
    htot, vtot = ha + hb, va + vb
    hz = pclk * 1000 / (htot * vtot) if htot and vtot else 0
    il = " interlaced" if d[17] & 0x80 else ""
    print(f"{where}: {ha}x{va}{il} @ {hz:.2f} Hz, pixel clock {pclk/1000:.3f} MHz, H freq {pclk/htot:.2f} kHz")

VICS = {1: "640x480p60", 2: "720x480p60 4:3", 3: "720x480p60 16:9", 4: "1280x720p60", 5: "1920x1080i60", 16: "1920x1080p60",
        17: "720x576p50 4:3", 18: "720x576p50 16:9", 19: "1280x720p50", 20: "1920x1080i50", 31: "1920x1080p50", 32: "1920x1080p24",
        33: "1920x1080p25", 34: "1920x1080p30", 62: "1280x720p30", 60: "1280x720p24", 61: "1280x720p25", 95: "3840x2160p30", 97: "3840x2160p60"}

def main():
    text = open(sys.argv[1]).read() if len(sys.argv) > 1 else sys.stdin.read()
    blocks = {int(n): bytes.fromhex(h) for n, h in re.findall(r"EDIDHEX(\d) ([0-9a-f]{256})", text)}
    if 0 not in blocks: sys.exit("no EDIDHEX0 line found")
    e = blocks[0]
    mfg = "".join(chr(64 + ((e[8] << 8 | e[9]) >> s & 31)) for s in (10, 5, 0))
    print(f"manufacturer {mfg}, product {e[11]:02x}{e[10]:02x}, EDID {e[18]}.{e[19]}, {'digital' if e[20] & 0x80 else 'analog'}, "
          f"{e[21]}x{e[22]} cm, {e[126]} extension(s), checksum {'OK' if sum(e) % 256 == 0 else 'BAD'}")
    est = [(35, 7, "720x400@70"), (35, 5, "640x480@60"), (35, 0, "800x600@60"), (36, 3, "1024x768@60"), (36, 0, "1280x1024@75")]
    print("established:", ", ".join(n for b, bit, n in est if e[b] >> bit & 1) or "none")
    ar = {0: (16, 10), 1: (4, 3), 2: (5, 4), 3: (16, 9)}
    std = [f"{(e[i]+31)*8}x{(e[i]+31)*8*ar[e[i+1]>>6][1]//ar[e[i+1]>>6][0]}@{(e[i+1]&63)+60}" for i in range(38, 54, 2) if not (e[i] == 1 and e[i+1] == 1)]
    print("standard:", ", ".join(std) or "none")
    for n, off in enumerate(range(54, 126, 18)): dtd(e[off:off+18], f"descriptor {n+1}")
    for b in sorted(k for k in blocks if k):
        x = blocks[b]
        if x[0] != 2: print(f"extension {b}: tag {x[0]:#04x} (not CEA-861)"); continue
        dend = x[2]; i = 4
        while i < dend <= 127:
            tag, ln = x[i] >> 5, x[i] & 31
            if tag == 2: print(f"extension {b}: CEA modes: " + ", ".join(VICS.get(v & 127, f"VIC{v & 127}") + ("*" if v & 128 else "") for v in x[i+1:i+1+ln]))
            elif tag == 3: print(f"extension {b}: vendor block {x[i+3]:02x}{x[i+2]:02x}{x[i+1]:02x}" + (" (HDMI)" if (x[i+3], x[i+2], x[i+1]) == (0, 0x0c, 3) else ""))
            i += 1 + ln
        for n, off in enumerate(range(dend, 110, 18)):
            if x[off] == 0 and x[off+1] == 0: break
            dtd(x[off:off+18], f"extension {b} descriptor {n+1}")

main()
