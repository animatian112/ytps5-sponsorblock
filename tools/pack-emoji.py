#!/usr/bin/env python3
# youtube-tv - Packs Twemoji's 72x72 PNGs into one file the app loads at start.
# Copyright (C) 2026 SponsorBlock for PS5 contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# usage: tools/pack-emoji.py <twemoji>/assets/72x72 assets/emoji/twemoji.pack
#
# Format (little endian): "HUIEMOJ1", u32 count, then per entry u16 key length,
# the key (code points in lowercase hex joined by "-", every fe0f left out, as
# the app matches text), u32 offset and u32 size into the data that follows
# the table. Twemoji graphics: CC-BY 4.0 (assets/emoji/LICENSE-GRAPHICS.txt).
import os
import struct
import sys


def main() -> None:
    source, target = sys.argv[1], sys.argv[2]
    entries = {}
    for name in sorted(os.listdir(source)):
        if not name.endswith(".png"):
            continue
        key = "-".join(part for part in name[:-4].lower().split("-") if part != "fe0f")
        with open(os.path.join(source, name), "rb") as png:
            data = png.read()
        # Two names for one key (with and without fe0f): the shorter file name wins.
        if key not in entries or len(name) < entries[key][0]:
            entries[key] = (len(name), data)
    table = bytearray()
    blob = bytearray()
    for key in sorted(entries):
        data = entries[key][1]
        encoded = key.encode("ascii")
        table += struct.pack("<H", len(encoded)) + encoded
        table += struct.pack("<II", len(blob), len(data))
        blob += data
    with open(target, "wb") as out:
        out.write(b"HUIEMOJ1" + struct.pack("<I", len(entries)) + table + blob)
    print(f"{len(entries)} emoji, {len(blob) / 1048576:.1f} MB of images -> {target}")


if __name__ == "__main__":
    main()
