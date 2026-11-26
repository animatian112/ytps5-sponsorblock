#!/usr/bin/env python3
# youtube-tv - Cut DejaVu Sans down to the Arabic and Hebrew letters the app bakes.
# Copyright (C) 2026 SponsorBlock for PS5 contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# usage: tools/subset-arabic-font.py <DejaVuSans.ttf> <out.ttf>
#
# The app has no shaping engine, so Arabic is drawn from the Unicode
# "presentation forms": one code point per letter shape (initial, medial,
# final, isolated) and the lam-alef ligatures, which src/gfx/bidi.cpp picks
# for each letter. DejaVu Sans is one of the few open fonts whose cmap still
# maps those forms, which is why it is used here rather than Noto Sans Arabic
# (whose letter shapes live behind GSUB lookups the renderer cannot run).
# Kept: the Arabic block (letters, digits, marks, punctuation), Arabic
# Presentation Forms-A and -B, and the Hebrew block, which needs no shaping
# and comes along for the price of 54 glyphs.
# Then `make fonts` bakes the result (tools/bake-fonts.sh). Needs fontTools
# (pip install fonttools); the output is committed, so only a change of the
# set needs this run again.

import sys

from fontTools import subset
from fontTools.ttLib import TTFont

RANGES = (
    (0x0590, 0x05FF),  # Hebrew
    (0x0600, 0x06FF),  # Arabic
    (0xFB50, 0xFDFF),  # Arabic Presentation Forms-A (Persian and Urdu letters)
    (0xFE70, 0xFEFF),  # Arabic Presentation Forms-B
)


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: subset-arabic-font.py <DejaVuSans.ttf> <out.ttf>")
    font = TTFont(sys.argv[1])
    cmap = font.getBestCmap()
    wanted = [c for first, last in RANGES for c in range(first, last + 1) if c in cmap]
    options = subset.Options()
    options.layout_features = []  # no shaping tables: the app picks the forms itself
    options.hinting = False
    options.name_IDs = ["*"]  # keep the copyright and licence records
    options.notdef_outline = True
    subsetter = subset.Subsetter(options)
    subsetter.populate(unicodes=wanted)
    subsetter.subset(font)
    font.recalcTimestamp = False  # the same input gives the same bytes
    font.save(sys.argv[2])
    print(f"{sys.argv[2]}: {len(wanted)} characters")


if __name__ == "__main__":
    main()
