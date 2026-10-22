#!/usr/bin/env bash
# ps5-homebrew-ui - Rebuild the baked SDF fonts in assets/fonts.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Each line of the table is: source TTF, output name, pixel size, SDF range,
# and optionally a fallback face with its scale for the letters the source
# lacks (Google Sans has no Cyrillic or Greek: they come from Inter, scaled to
# Google Sans's letter height).
# This app shows video titles it did not write, so every face is baked with
# the `2048 european` set: printable ASCII plus accented Latin, Greek and
# Cyrillic (ADOPTING.md's lesson - YouTube titles are not ASCII).
# To add a typeface, drop its TTF and licence into third_party/fonts, add a
# line here, run `make fonts`, and load the new .huifont where the others are
# loaded (src/main.cpp and host/snapshot_main.cpp).

set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cxx=$(command -v "${HOST_CXX:-clang++}")
mkdir -p "$root/build/host" "$root/assets/fonts"
"$cxx" -std=c++20 -O2 -w "$root/tools/font-baker/bake_font.cpp" -o "$root/build/host/bake_font"
while read -r source output size range fallback fallback_scale; do
    [[ -n $source ]] || continue
    extra=()
    if [[ -n ${fallback:-} ]]; then
        extra=("$root/third_party/fonts/$fallback" "$fallback_scale")
    fi
    "$root/build/host/bake_font" "$root/third_party/fonts/$source" \
        "$root/assets/fonts/$output.huifont" "$size" "$range" 2048 european "${extra[@]}"
done <<'FONTS'
GoogleSansFlex-Regular.ttf google-sans-regular 56 8 Inter-Regular.ttf 0.95
GoogleSansFlex-Medium.ttf google-sans-medium 56 8 Inter-Medium.ttf 0.95
GoogleSansFlex-Bold.ttf google-sans-bold 56 8 Inter-SemiBold.ttf 0.95
DejaVuSansMono.ttf dejavu-sans-mono 52 8
FONTS
cp "$root"/third_party/fonts/*-LICENSE.txt "$root/assets/fonts/"
