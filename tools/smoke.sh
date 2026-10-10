#!/usr/bin/env bash
# youtube-tv - Build and run the live smoke test of the network layer.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Needs internet. The host's libcurl comes from .hostdeps (see
# tools/host-snapshots.sh); nothing is written outside build/ and /tmp.

set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cxx=$(command -v "${HOST_CXX:-clang++}")
build="$root/build/host"
mkdir -p "$build"
"$cxx" -std=c++20 -O2 -Wall -Wextra \
    -DHUI_SOURCE_DIR="\"$root\"" -I"$root/src" -I"$root/.hostdeps/include" \
    "$root/host/smoke_main.cpp" \
    "$root/host/http_host.cpp" \
    "$root/host/player_stub.cpp" \
    "$root/src/yt/auth.cpp" "$root/src/yt/codec.cpp" "$root/src/yt/decipher.cpp" \
    "$root/src/yt/http.cpp" \
    "$root/src/yt/innertube.cpp" "$root/src/yt/log.cpp" "$root/src/yt/qr.cpp" \
    "$root/src/yt/service.cpp" "$root/src/yt/types.cpp" \
    "$root/src/core/save_file.cpp" \
    "$root/src/third_party/yyjson/yyjson.c" \
    "$root/src/third_party/qrcodegen/qrcodegen.c" \
    -L"$root/.hostdeps/lib" -lcurl -lpthread -lm -o "$build/yt_smoke"
exec "$build/yt_smoke" "$@"
