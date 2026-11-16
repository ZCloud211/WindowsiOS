#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build the production Neptune host runtime for same-process iOS rendering.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TASK_WORK="$ROOT/work/gpu3d-deps"
TASK_SOURCES="$ROOT/work/gpu-3d-sources"
export PATH="$ROOT/work/tools/python/bin:$ROOT/work/tools/bin:$PATH"
PREFIX="$TASK_WORK/neptune-ios-prefix"
SOURCE="$TASK_SOURCES/virglrenderer"
EPOXY="$TASK_SOURCES/libepoxy"
RENDERER_SHA=dd0652b13e589c4c124ea79123a3adcd85173f4f
JOBS="${GPU_BUILD_JOBS:-2}"
mkdir -p "$TASK_WORK" "$TASK_SOURCES" "$PREFIX"
if [ ! -d "$SOURCE/.git" ]; then
    git clone --depth 1 --branch macos-next https://github.com/utmapp/virglrenderer.git "$SOURCE"
fi
if [ "$(git -C "$SOURCE" rev-parse HEAD)" != "$RENDERER_SHA" ]; then
    git -C "$SOURCE" fetch --depth 1 origin "$RENDERER_SHA"
    git -C "$SOURCE" checkout --detach "$RENDERER_SHA"
fi
if [ ! -d "$EPOXY/.git" ]; then
    git clone --depth 1 --branch 1.5.10 https://github.com/anholt/libepoxy.git "$EPOXY"
fi
SDK_PATH="$(xcrun --sdk iphoneos --show-sdk-path)"
CROSS_FILE="$TASK_WORK/ios-neptune-arm64.meson"
cat > "$CROSS_FILE" <<EOF
[binaries]
c = ['xcrun', '--sdk', 'iphoneos', 'clang']
cpp = ['xcrun', '--sdk', 'iphoneos', 'clang++']
objc = ['xcrun', '--sdk', 'iphoneos', 'clang']
objcpp = ['xcrun', '--sdk', 'iphoneos', 'clang++']
ar = ['xcrun', '--sdk', 'iphoneos', 'ar']
strip = ['xcrun', '--sdk', 'iphoneos', 'strip']
pkg-config = '$ROOT/work/tools/bin/pkg-config'
[properties]
needs_exe_wrapper = true
[built-in options]
c_args = ['-arch', 'arm64', '-isysroot', '$SDK_PATH', '-miphoneos-version-min=17.0']
cpp_args = ['-arch', 'arm64', '-isysroot', '$SDK_PATH', '-miphoneos-version-min=17.0']
objc_args = ['-arch', 'arm64', '-isysroot', '$SDK_PATH', '-miphoneos-version-min=17.0']
objcpp_args = ['-arch', 'arm64', '-isysroot', '$SDK_PATH', '-miphoneos-version-min=17.0']
c_link_args = ['-arch', 'arm64', '-isysroot', '$SDK_PATH', '-miphoneos-version-min=17.0']
cpp_link_args = ['-arch', 'arm64', '-isysroot', '$SDK_PATH', '-miphoneos-version-min=17.0']
objc_link_args = ['-arch', 'arm64', '-isysroot', '$SDK_PATH', '-miphoneos-version-min=17.0']
objcpp_link_args = ['-arch', 'arm64', '-isysroot', '$SDK_PATH', '-miphoneos-version-min=17.0']
[host_machine]
system = 'darwin'
subsystem = 'ios'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
EOF
# Keep iOS shared resource FDs inside its app container without app-group entitlements.
PATCH="$ROOT/patches/gpu3d/neptune-ios-anonymous-memory.patch"
if git -C "$SOURCE" apply --check "$PATCH" 2>/dev/null; then
    git -C "$SOURCE" apply "$PATCH"
else
    git -C "$SOURCE" apply --reverse --check "$PATCH"
fi
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig"
EPOXY_BUILD="$TASK_WORK/epoxy-ios"
EPOXY_ARGS=(--cross-file "$CROSS_FILE" --prefix "$PREFIX" --libdir lib
    --buildtype release --default-library static -Dtests=false -Ddocs=false -Dglx=no -Degl=no -Dx11=false)
if [ -f "$EPOXY_BUILD/build.ninja" ]; then
    meson setup --reconfigure "$EPOXY_BUILD" "$EPOXY" "${EPOXY_ARGS[@]}"
else
    meson setup "$EPOXY_BUILD" "$EPOXY" "${EPOXY_ARGS[@]}"
fi
meson compile -C "$EPOXY_BUILD" -j "$JOBS"
meson install -C "$EPOXY_BUILD"
if [ ! -f "$SOURCE/subprojects/directx-native/src/dxn_shm_darwin.c" ]; then
    (cd "$SOURCE" && meson subprojects download directx-native)
fi
PATCH="$ROOT/patches/gpu3d/directx-native-ios-shared-memory.patch"
if git -C "$SOURCE/subprojects/directx-native" apply --check "$PATCH" 2>/dev/null; then
    git -C "$SOURCE/subprojects/directx-native" apply "$PATCH"
else
    git -C "$SOURCE/subprojects/directx-native" apply --reverse --check "$PATCH"
fi
BUILD="$TASK_WORK/neptune-ios"
ARGS=(--cross-file "$CROSS_FILE" --prefix "$PREFIX" --libdir lib
    --buildtype release --default-library static -Dplatforms=[]
    -Dtests=false -Dvtest=false -Dvideo=false -Dtracing=none
    -Dvenus=false -Dneptune=true -Drender-server-mode=thread -Drender-server-worker=thread
    -Dunstable-apis=true)
if [ -f "$BUILD/build.ninja" ]; then
    meson setup --reconfigure "$BUILD" "$SOURCE" "${ARGS[@]}"
else
    meson setup "$BUILD" "$SOURCE" "${ARGS[@]}"
fi
meson compile -C "$BUILD" -j "$JOBS"
meson install -C "$BUILD"
python3 - "$PREFIX" "$SOURCE" <<'PY'
import hashlib,json,subprocess,sys
from pathlib import Path
p,src=map(Path,sys.argv[1:]);lib=p/'lib/libvirglrenderer.a'
r=dict(renderer_commit=subprocess.check_output(['git','-C',str(src),'rev-parse','HEAD'],text=True).strip(),
    platform='arm64-apple-ios17.0',neptune=True,venus=False,render_server_mode='thread',
    library=str(lib),sha256=hashlib.file_digest(lib.open('rb'),'sha256').hexdigest(),
    guest_integration_complete=False)
(p/'BUILD.json').write_text(json.dumps(r,indent=2)+'\n');print(json.dumps(r,indent=2))
PY
