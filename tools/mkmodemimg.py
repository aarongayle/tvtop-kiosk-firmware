#!/usr/bin/env python3
"""Pack the ESP modem firmware into the blob the RP2350 flashes it from.

v3 has one USB-C port and it goes to the RP2354A, so the modem is programmed by the kiosk over the
same UART it talks to it on (docs/MODEM.md). This produces what the kiosk reads: a small header and
the whole ESP flash image, zlib-compressed.

Compressed because the ESP ROM loader inflates it on the way in — the FLASH_DEFL_* commands take
deflate data — so it costs the RP2354A nothing to decompress and roughly halves both the flash it
occupies and the time it spends on the wire.

    tools/mkmodemimg.py modem/build --out build-modem/modem-image.bin [--uf2 --offset 0x200000]
"""
import argparse, os, struct, subprocess, sys, zlib

MAGIC = 0x314D444D          # 'MDM1'
HEADER_LEN = 64
UF2_MAGIC0, UF2_MAGIC1, UF2_MAGIC2 = 0x0A324655, 0x9E5D5157, 0x0AB16F30
UF2_FLAG_FAMILY = 0x00002000
RP2350_ARM_S_FAMILY = 0xE48BFF59


def merge(build_dir, chip):
    """esptool's merge_bin, so the three pieces become one image starting at offset 0."""
    out = os.path.join(build_dir, "modem-merged.bin")
    parts = [
        ("0x0", os.path.join(build_dir, "bootloader", "bootloader.bin")),
        ("0x8000", os.path.join(build_dir, "partition_table", "partition-table.bin")),
        ("0x10000", os.path.join(build_dir, "tvtop_modem.bin")),
    ]
    for _, p in parts:
        if not os.path.exists(p):
            sys.exit(f"missing {p} — build the modem first (cd modem && idf.py build)")
    cmd = [sys.executable, "-m", "esptool", "--chip", chip, "merge_bin", "-o", out,
           "--flash_mode", "dio", "--flash_freq", "80m", "--flash_size", "4MB"]
    for addr, p in parts:
        cmd += [addr, p]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
    return out


def uf2(data, base_addr, path):
    """A UF2 the Pico bootloader accepts, so the blob installs like any other firmware file."""
    blocks, n = bytearray(), (len(data) + 255) // 256
    for i in range(n):
        chunk = data[i * 256:(i + 1) * 256]
        blocks += struct.pack("<IIIIIIII", UF2_MAGIC0, UF2_MAGIC1, UF2_FLAG_FAMILY,
                              base_addr + i * 256, 256, i, n, RP2350_ARM_S_FAMILY)
        blocks += chunk.ljust(476, b"\0") + struct.pack("<I", UF2_MAGIC2)
    open(path, "wb").write(blocks)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("build_dir")
    ap.add_argument("--out", required=True)
    ap.add_argument("--chip", default="esp32c3")
    ap.add_argument("--offset", default="0x200000", help="where in RP2350 flash the blob lives")
    ap.add_argument("--uf2", action="store_true")
    a = ap.parse_args()

    merged = merge(a.build_dir, a.chip)
    raw = open(merged, "rb").read()
    comp = zlib.compress(raw, 9)

    # crc32 of the compressed payload: a blob that was truncated or never written at all is caught
    # before the kiosk puts the modem into its bootloader, not after.
    head = struct.pack("<IIIIII16s", MAGIC, 1, 0, len(raw), len(comp),
                       zlib.crc32(comp) & 0xFFFFFFFF, b"")
    head = head.ljust(HEADER_LEN, b"\0")
    blob = head + comp
    open(a.out, "wb").write(blob)

    base = int(a.offset, 0)
    if a.uf2:
        uf2(blob, 0x10000000 + base, a.out + ".uf2")

    print(f"modem image: {len(raw)} bytes -> {len(comp)} compressed ({len(blob)} with header)")
    print(f"  {a.out}" + (f"\n  {a.out}.uf2 at {a.offset}" if a.uf2 else ""))


if __name__ == "__main__":
    main()
