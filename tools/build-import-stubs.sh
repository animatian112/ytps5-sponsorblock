#!/usr/bin/env bash
# youtube-tv - Build the import stubs the public SDK does not ship.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Every tooling/stubs/*.c becomes tooling/stubs/<name>.so with the SONAME the
# console's loader expects (libSce<Vendor>.sprx), built with the target
# compiler from the pinned SDK. tools/build.sh calls this before linking and
# appends the results to APP_IMPORT_STUBS, so `make` stays one command.
#
# The stubs only carry symbol names: the SDK's linker derives the import NIDs
# from them (prospero-nid), and an app that never calls the functions still
# starts when the console's library is absent.

set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
sdk="$root/.deps/native/ps5-payload-sdk"

[[ -d $sdk ]] || { echo "tools/build-import-stubs.sh: run make deps first" >&2; exit 2; }

compiler="$sdk/bin/prospero-clang"
[[ -x $compiler ]] || { echo "tools/build-import-stubs.sh: $compiler missing" >&2; exit 2; }

sources=("$root"/tooling/stubs/*.c)
[[ -e ${sources[0]} ]] || exit 0

built=0
for source in "${sources[@]}"; do
    name=$(basename -- "$source" .c)
    output="$root/tooling/stubs/$name.so"
    if [[ -f $output && $output -nt $source ]]; then
        continue
    fi
    # The soname is the console's library name; the toolchain reads it back
    # when it converts the stub's symbols into NID imports.
    # -fno-lto and explicit visibility: with the SDK's default link-time
    # optimization the stubs' symbols are internalized and the stub would
    # export nothing for the NID derivation to read.
    "$compiler" -shared -fPIC -nostdlib -fno-lto -fvisibility=default \
        -Wl,-soname,"$name.sprx" -o "$output.tmp" "$source"
    mv -- "$output.tmp" "$output"
    printf '==> [stubs] %s (%s)\n' "${output#"$root/"}" "$name.sprx"
    built=$((built + 1))
done
(( built == 0 )) || printf '==> [stubs] %d stub(s) rebuilt\n' "$built"
