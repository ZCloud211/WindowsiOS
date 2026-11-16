#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# wimlib is used only while preparing large Windows installation files.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export PATH="$ROOT/work/tools/bin:$ROOT/work/tools/python/bin:/opt/homebrew/bin:$PATH"
DEPS="$ROOT/work/iso-deps"
VERSION=1.14.5
ARCHIVE="$DEPS/wimlib-$VERSION.tar.gz"
mkdir -p "$DEPS" "$ROOT/ios/deps/include" "$ROOT/ios/deps/lib"
if [ ! -f "$ARCHIVE" ]; then
    curl -fL --retry 3 "https://wimlib.net/downloads/wimlib-$VERSION.tar.gz" -o "$ARCHIVE.part"
    mv "$ARCHIVE.part" "$ARCHIVE"
fi
if [ ! -d "$DEPS/wimlib-$VERSION" ]; then tar -xf "$ARCHIVE" -C "$DEPS"; fi
BUILD="$DEPS/wimlib-ios"
mkdir -p "$BUILD"
if [ ! -f "$BUILD/Makefile" ]; then
    SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
    (cd "$BUILD" && CC="$(xcrun --sdk iphoneos --find clang)" \
        CFLAGS="-target arm64-apple-ios17.0 -isysroot $SDK -O2" \
        LDFLAGS="-target arm64-apple-ios17.0 -isysroot $SDK" \
        "$DEPS/wimlib-$VERSION/configure" --host=aarch64-apple-darwin \
        --without-ntfs-3g --without-fuse --disable-shared --enable-static)
fi
make -C "$BUILD" -j"$(sysctl -n hw.ncpu)" libwim.la
cp "$BUILD/.libs/libwim.a" "$ROOT/ios/deps/lib/"
cp "$DEPS/wimlib-$VERSION/include/wimlib.h" "$ROOT/ios/deps/include/"
LICENSES="$ROOT/ios/Sources/Resources/Licenses"
mkdir -p "$LICENSES"
cp "$DEPS/wimlib-$VERSION/COPYING.LGPL" "$LICENSES/wimlib-LGPL-2.1.txt"
cp "$ROOT/third_party/libudfread/COPYING" "$LICENSES/libudfread-LGPL-2.1.txt"
sed -n '1,20p' "$ROOT/third_party/fatfs/ff.c" > "$LICENSES/FatFs-notice.txt"
