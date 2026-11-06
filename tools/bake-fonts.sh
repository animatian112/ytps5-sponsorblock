#!/usr/bin/env bash
# ps5-homebrew-ui - Rebuild the baked SDF fonts in assets/fonts.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Each line of the table is: source TTF, output name, pixel size, SDF range,
# atlas size, glyph set, and optionally a fallback face with its scale for the
# letters the source lacks (Google Sans has no Cyrillic or Greek: they come
# from Inter, scaled to Google Sans's letter height).
# This app shows video titles it did not write, so every face is baked with
# the `2048 european` set: printable ASCII plus accented Latin, Greek and
# Cyrillic (ADOPTING.md's lesson - YouTube titles are not ASCII). Scripts
# beyond that are faces of their own (the `thai` set) that every face falls
# back on at run time (gfx::Font::add_fallback, in src/main.cpp).
# To add a typeface, drop its TTF and licence into third_party/fonts, add a
# line here, run `make fonts`, and load the new .huifont where the others are
# loaded (src/main.cpp and host/snapshot_main.cpp).

set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cxx=$(command -v "${HOST_CXX:-clang++}")
mkdir -p "$root/build/host" "$root/assets/fonts"
"$cxx" -std=c++20 -O2 -w "$root/tools/font-baker/bake_font.cpp" -o "$root/build/host/bake_font"
while read -r source output size range atlas glyphs fallback fallback_scale; do
    [[ -n $source ]] || continue
    extra=()
    if [[ -n ${fallback:-} ]]; then
        extra=("$root/third_party/fonts/$fallback" "$fallback_scale")
    fi
    "$root/build/host/bake_font" "$root/third_party/fonts/$source" \
        "$root/assets/fonts/$output.huifont" "$size" "$range" "$atlas" "$glyphs" "${extra[@]}"
done <<'FONTS'
GoogleSansFlex-Regular.ttf google-sans-regular 56 8 2048 european Inter-Regular.ttf 0.95
GoogleSansFlex-Medium.ttf google-sans-medium 56 8 2048 european Inter-Medium.ttf 0.95
GoogleSansFlex-Bold.ttf google-sans-bold 56 8 2048 european Inter-SemiBold.ttf 0.95
DejaVuSansMono.ttf dejavu-sans-mono 52 8 2048 european
NotoSansThai-Regular.ttf noto-sans-thai 56 8 1024 thai
FONTS
cp "$root"/third_party/fonts/*-LICENSE.txt "$root/assets/fonts/"
