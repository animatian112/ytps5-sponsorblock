#!/usr/bin/env python3
# youtube-tv - Write a release ZIP from an app folder's contents, flat.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
"""ps5upload unpacks a release ZIP into a folder it names itself, so an archive
that wraps everything in one top-level directory lands one level too deep
(``/data/homebrew/PPSA99350/PPSA99350/...``). This tool writes the ZIP from the
*contents* of the app folder instead: entries are relative to it (no wrapping
directory), so unpacking into any folder produces a correct app folder.

Every entry is stored as 0777: the console only starts an app whose files are
open to every user. The archive is written to a temporary file and moved into
place, then re-read to prove every entry is 0777 and the archive is flat.

usage: zip-app-folder.py ARCHIVE.zip SOURCE_DIR
"""
import os
import shutil
import sys
import zipfile

if len(sys.argv) != 3:
    raise SystemExit("usage: zip-app-folder.py ARCHIVE.zip SOURCE_DIR")

archive, source = sys.argv[1], sys.argv[2]
if not os.path.isdir(source):
    raise SystemExit(f"{source}: no such directory")

temporary = archive + ".tmp"
entries = 0
with zipfile.ZipFile(temporary, "w") as target:
    for parent, directories, files in os.walk(source):
        directories.sort()
        files.sort()
        relative = os.path.relpath(parent, source)
        if relative != os.curdir:
            info = zipfile.ZipInfo(relative.replace(os.sep, "/") + "/")
            info.create_system = 3  # Unix: the high half of external_attr is the mode
            info.external_attr = (0o040777 << 16) | 0x10
            info.compress_type = zipfile.ZIP_DEFLATED
            target.writestr(info, b"")
            entries += 1
        for name in files:
            path = os.path.join(parent, name)
            info = zipfile.ZipInfo(os.path.relpath(path, source).replace(os.sep, "/"))
            info.create_system = 3
            info.external_attr = 0o100777 << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            with open(path, "rb") as handle, target.open(info, "w") as member:
                shutil.copyfileobj(handle, member)
            entries += 1
os.replace(temporary, archive)

with zipfile.ZipFile(archive) as check:
    infos = check.infolist()
    if not infos:
        raise SystemExit(f"{archive}: empty archive")
    wrong = [i.filename for i in infos if (i.external_attr >> 16) & 0o777 != 0o777]
    if wrong:
        raise SystemExit(f"{archive}: entries are not 0777: {wrong[:3]}")
    if check.testzip() is not None:
        raise SystemExit(f"{archive}: corrupt entry")
    # No wrapping directory: the first entry belongs to the archive root.
    first = infos[0].filename
    if first.rstrip("/").count("/"):
        raise SystemExit(f"{archive}: first entry {first!r} is not at the archive root")
    root = first.rstrip("/")
    if all(i.filename.rstrip("/") == root or i.filename.startswith(root + "/") for i in infos):
        raise SystemExit(f"{archive}: a wrapping directory {root!r} holds every entry")

print(f"{archive}: {entries} entries stored as 0777 (flat)")
