#!/usr/bin/env bash
# youtube-tv - Build the self-update helper (self-updater.elf) with the pinned SDK.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The helper is the program the app sends to the console's payload loader to put a
# downloaded release in place (see src/update/self_update_protocol.h). It is an ordinary
# payload, so it is built with the payload toolchain, not the app's: make runs
# tooling/self-update-helper/Makefile, which includes $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
# and links miniz, the SHA-256 and the JSON reader from src/update. tools/build.sh calls
# this before it assembles the app folder and copies the ELF into it as
# self-updater.elf, so every release ZIP carries the helper it needs.
#
# With no SDK under .deps, the helper cannot be built: the message says so and the
# build carries on without one (the app then reports that it cannot update itself).

set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
sdk="$root/.deps/native/ps5-payload-sdk"
output="$root/build/self-update/self-updater.elf"

if [[ ! -d $sdk ]]; then
    printf '==> [self-update] %s is missing; skipping the helper (run make deps to build it)\n' \
        "${sdk#"$root/"}" >&2
    exit 0
fi
[[ -f $sdk/toolchain/prospero.mk ]] || {
    echo "tools/build-self-update-helper.sh: $sdk/toolchain/prospero.mk is missing" >&2
    exit 2
}

make -C "$root/tooling/self-update-helper" OUTPUT="$output" PS5_PAYLOAD_SDK="$sdk"

[[ -f $output ]] || { echo "tools/build-self-update-helper.sh: $output was not built" >&2; exit 2; }
printf '==> [self-update] %s\n' "${output#"$root/"}"
