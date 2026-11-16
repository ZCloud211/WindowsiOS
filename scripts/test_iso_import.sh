#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Host-only tests: no iPad connection, app launch or debugging.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export PATH="$ROOT/work/tools/bin:$ROOT/work/tools/python/bin:/opt/homebrew/bin:$PATH"
SOURCE="$ROOT/work/iso-deps/wimlib-1.14.5"
BUILD="$ROOT/work/iso-deps/wimlib-mac"
[ -f "$SOURCE/configure" ] || { echo "Run scripts/build_ios_import_deps.sh first." >&2; exit 1; }
mkdir -p "$BUILD"
if [ ! -f "$BUILD/Makefile" ]; then
    (cd "$BUILD" && "$SOURCE/configure" --without-ntfs-3g --without-fuse --disable-shared --enable-static)
fi
make -C "$BUILD" -j"$(sysctl -n hw.ncpu)" libwim.la wimlib-imagex
make -C "$ROOT" test-iso
