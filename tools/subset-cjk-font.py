#!/usr/bin/env python3
# youtube-tv - Cut Noto Sans CJK down to the characters the app bakes.
# Copyright (C) 2026 SponsorBlock for PS5 contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# usage: tools/subset-cjk-font.py <NotoSansCJK-Regular.ttc> <face> <out.ttf>
#   face: SC or JP, the face of the collection whose glyph shapes to keep.
#
# The full collection is about 20 MB a weight and holds some 65,000 glyphs;
# the console needs the everyday ones of Chinese and Japanese text. The set
# comes from the national character standards, through Python's own codecs,
# so it needs no list from the network:
#   - GB 2312, all 6,763 hanzi (Simplified Chinese),
#   - Big5's 5,401 frequent hanzi (Traditional Chinese),
#   - JIS X 0208 levels 1 and 2, 6,355 kanji (Japanese),
#   - KS X 1001's 2,350 Hangul syllables (Korean titles),
#   - kana, CJK punctuation and the full-width forms,
#   - every character from U+2000 up in the app's own phrases and keyboards
#     (src/core/i18n_catalog.cpp, src/core/i18n.cpp, the keyboard layouts),
#     so none falls out: the button shapes ○ □ △ among them.
# The outlines are then turned from CFF cubics into TrueType quadratics:
# stb_truetype draws CFF ideographs with strokes missing, TrueType ones right.
# Then `make fonts` bakes the result (tools/bake-fonts.sh). Needs fontTools
# (pip install fonttools); the output is committed, so only a change of the
# set needs this run again.

import re
import sys
from pathlib import Path

from fontTools import subset
from fontTools.pens.cu2quPen import Cu2QuPen
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib import TTCollection, newTable


def standard(encoding, first_rows, last_rows, cells):
    """The characters of a double-byte standard: rows x cells, each decoded."""
    found = set()
    for row in range(first_rows, last_rows + 1):
        for cell in cells:
            try:
                found.add(bytes([row, cell]).decode(encoding))
            except UnicodeDecodeError:
                pass
    return found


def characters(root):
    wanted = set()
    gb_cells = range(0xA1, 0xFF)
    wanted |= standard("gb2312", 0xB0, 0xF7, gb_cells)
    wanted |= standard("big5", 0xA4, 0xC6, list(range(0x40, 0x7F)) + list(range(0xA1, 0xFF)))
    wanted |= standard("euc_jp", 0xB0, 0xF4, gb_cells)
    wanted |= standard("euc_kr", 0xB0, 0xC8, gb_cells)
    for first, last in (
        (0x3000, 0x303F),  # CJK symbols and punctuation
        (0x3040, 0x309F),  # hiragana
        (0x30A0, 0x30FF),  # katakana
        (0x31F0, 0x31FF),  # small katakana
        (0xFF01, 0xFF5E),  # full-width ASCII
        (0xFF61, 0xFF9F),  # half-width katakana
    ):
        wanted |= {chr(c) for c in range(first, last + 1)}
    for source in ("src/core/i18n_catalog.cpp", "src/core/i18n.cpp", "src/app/keyboard_languages.cpp"):
        text = (root / source).read_text(encoding="utf-8")
        # From U+2000: the button shapes (○ □ △) and punctuation the
        # phrases use beyond what the Latin faces have; those they have win.
        wanted |= {c for c in text if ord(c) >= 0x2000}
    return sorted(c for c in wanted if len(c) == 1 and ord(c) >= 0x2000)


def to_truetype(font):
    """CFF outlines to glyf ones, within one font unit of the curves."""
    glyph_set = font.getGlyphSet()
    order = font.getGlyphOrder()
    glyphs = {}
    for name in order:
        pen = TTGlyphPen(glyph_set)
        glyph_set[name].draw(Cu2QuPen(pen, 1.0, reverse_direction=True))
        glyphs[name] = pen.glyph()
    font["loca"] = newTable("loca")
    font["glyf"] = newTable("glyf")
    font["glyf"].glyphOrder = order
    font["glyf"].glyphs = glyphs
    del font["CFF "]
    if "VORG" in font:
        del font["VORG"]
    maxp = font["maxp"] = newTable("maxp")
    maxp.tableVersion = 0x00010000
    for field in ("maxTwilightPoints", "maxStorage", "maxFunctionDefs", "maxInstructionDefs",
                  "maxStackElements", "maxSizeOfInstructions", "maxComponentElements"):
        setattr(maxp, field, 0)
    maxp.maxZones = 1
    font["head"].glyphDataFormat = 0
    font.sfntVersion = "\0\1\0\0"


def main():
    if len(sys.argv) != 4 or sys.argv[2] not in ("SC", "JP"):
        raise SystemExit(__doc__ or "usage: subset-cjk-font.py <collection.ttc> <SC|JP> <out.ttf>")
    root = Path(__file__).resolve().parent.parent
    collection = TTCollection(sys.argv[1])
    face = next(
        font
        for font in collection.fonts
        if re.fullmatch(r"Noto Sans CJK " + sys.argv[2], font["name"].getDebugName(1) or "")
    )
    wanted = characters(root)
    options = subset.Options()
    options.layout_features = []  # no shaping: the app draws code points
    options.hinting = False
    options.name_IDs = ["*"]  # keep the copyright and licence records
    options.notdef_outline = True
    subsetter = subset.Subsetter(options)
    subsetter.populate(unicodes=[ord(c) for c in wanted])
    subsetter.subset(face)
    to_truetype(face)
    face.recalcTimestamp = False  # the same input gives the same bytes
    face.save(sys.argv[3])
    print(f"{sys.argv[3]}: {len(wanted)} characters")


if __name__ == "__main__":
    main()
