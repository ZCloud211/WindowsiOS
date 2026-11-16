#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Local Apple Silicon counterpart of the iOS backend, for executable integration.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TASK_WORK="$ROOT/work/gpu3d-deps"
TASK_SOURCE="$ROOT/work/gpu-3d-sources"
export PATH="$ROOT/work/tools/python/bin:$ROOT/work/tools/bin:$PATH"
JOBS="${GPU_BUILD_JOBS:-4}"
LLVM_BUILD="$TASK_WORK/llvm-host"
LLVM_PREFIX="$TASK_WORK/llvm-macos-prefix"
PREFIX="$TASK_WORK/macos-prefix"
mkdir -p "$LLVM_PREFIX/include" "$LLVM_PREFIX/lib" "$PREFIX"
LLVM_LIBRARIES=()
while IFS= read -r item; do LLVM_LIBRARIES+=("$item"); done < <(
python3 - "$TASK_SOURCE/dxmt/src/airconv/meson.build" <<'PY'
import re,sys
s=open(sys.argv[1]).read().split('llvm_deps = [',1)[1].split(']',1)[0]
print('\n'.join(dict.fromkeys(re.findall(r'-l(LLVM[A-Za-z0-9]+)',s))))
PY
)
cmake --build "$LLVM_BUILD" --target "${LLVM_LIBRARIES[@]}" --parallel "$JOBS"
cp -R "$TASK_SOURCE/llvm-project/llvm/include/llvm" "$TASK_SOURCE/llvm-project/llvm/include/llvm-c" "$LLVM_PREFIX/include/"
cp -R "$LLVM_BUILD/include/llvm" "$LLVM_PREFIX/include/"
for lib in "${LLVM_LIBRARIES[@]}"; do cp "$LLVM_BUILD/lib/lib$lib.a" "$LLVM_PREFIX/lib/"; done
BUILD="$TASK_WORK/dxmt-macos"
ARGS=(--prefix "$PREFIX" --libdir lib --buildtype release -Dnative_llvm_path="$LLVM_PREFIX"
    -Denable_tests=true -Denable_d3d12=false -Denable_nvapi=false -Denable_nvngx=false)
if [ -f "$BUILD/build.ninja" ]; then meson setup --reconfigure "$BUILD" "$TASK_SOURCE/dxmt" "${ARGS[@]}";
else meson setup "$BUILD" "$TASK_SOURCE/dxmt" "${ARGS[@]}"; fi
meson compile -C "$BUILD" -j "$JOBS"
meson install -C "$BUILD"
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig"
BUILD="$TASK_WORK/epoxy-macos"
ARGS=(--prefix "$PREFIX" --libdir lib --buildtype release --default-library static -Dtests=false -Ddocs=false -Dglx=no -Degl=no -Dx11=false)
if [ -f "$BUILD/build.ninja" ]; then meson setup --reconfigure "$BUILD" "$TASK_SOURCE/libepoxy" "${ARGS[@]}";
else meson setup "$BUILD" "$TASK_SOURCE/libepoxy" "${ARGS[@]}"; fi
meson compile -C "$BUILD" -j "$JOBS"
meson install -C "$BUILD"
BUILD="$TASK_WORK/neptune-macos"
ARGS=(--prefix "$PREFIX" --libdir lib --buildtype release --default-library static
    -Dplatforms=[] -Dtests=false -Dvtest=false -Dvideo=false -Dtracing=none -Dvenus=false -Dneptune=true
    -Drender-server-mode=thread -Drender-server-worker=thread -Dunstable-apis=true)
if [ -f "$BUILD/build.ninja" ]; then meson setup --reconfigure "$BUILD" "$TASK_SOURCE/virglrenderer" "${ARGS[@]}";
else meson setup "$BUILD" "$TASK_SOURCE/virglrenderer" "${ARGS[@]}"; fi
meson compile -C "$BUILD" -j "$JOBS"
meson install -C "$BUILD"
