#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build native ARM64 iOS D3D11->Metal without Wine or a guest session.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TASK_WORK="$ROOT/work/gpu3d-deps"
TASK_SOURCES="$ROOT/work/gpu-3d-sources"
TASK_TOOLS="$ROOT/work/tools/python/bin"
export PATH="$TASK_TOOLS:$ROOT/work/tools/bin:$PATH"
DXMT_SHA=1fe2e752c98207518a99c9322482f63ebb9f0232
DXMT_SOURCE="$TASK_SOURCES/dxmt"
LLVM_SOURCE="$TASK_SOURCES/llvm-project"
HOST_BUILD="$TASK_WORK/llvm-host"
LLVM_BUILD="$TASK_WORK/llvm-ios"
LLVM_PREFIX="$TASK_WORK/llvm-ios-prefix"
DXMT_BUILD="$TASK_WORK/dxmt-ios"
PREFIX="$TASK_WORK/ios-prefix"
JOBS="${GPU_BUILD_JOBS:-4}"
mkdir -p "$TASK_WORK" "$TASK_SOURCES" "$PREFIX"
for tool in cmake ninja meson python3; do command -v "$tool" >/dev/null; done

if [ ! -d "$DXMT_SOURCE/.git" ]; then
    git clone --depth 1 --branch main https://github.com/utmapp/dxmt.git "$DXMT_SOURCE"
fi
if [ "$(git -C "$DXMT_SOURCE" rev-parse HEAD)" != "$DXMT_SHA" ]; then
    git -C "$DXMT_SOURCE" fetch --depth 1 origin "$DXMT_SHA"
    git -C "$DXMT_SOURCE" checkout --detach "$DXMT_SHA"
fi
git -C "$DXMT_SOURCE" submodule update --init --depth 1 include/native/directx

if [ ! -d "$LLVM_SOURCE/.git" ]; then
    git clone --depth 1 --filter=blob:none --sparse --branch llvmorg-15.0.7 \
        https://github.com/llvm/llvm-project.git "$LLVM_SOURCE"
    git -C "$LLVM_SOURCE" sparse-checkout set llvm cmake
fi

COMMON_LLVM=(
    -G Ninja -DCMAKE_BUILD_TYPE=Release -DLLVM_TARGETS_TO_BUILD=
    -DLLVM_BUILD_TOOLS=OFF -DLLVM_BUILD_UTILS=OFF
    -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF
    -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF
    -DLLVM_ENABLE_TERMINFO=OFF -DLLVM_ENABLE_BACKTRACES=OFF
    -DLLVM_ENABLE_CRASH_OVERRIDES=OFF -DLLVM_ENABLE_ASSERTIONS=OFF
)
if [ ! -x "$HOST_BUILD/bin/llvm-tblgen" ]; then
    echo "Building native LLVM table generator"
    cmake -S "$LLVM_SOURCE/llvm" -B "$HOST_BUILD" "${COMMON_LLVM[@]}" \
        -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0
    cmake --build "$HOST_BUILD" --target llvm-tblgen --parallel "$JOBS"
fi
SDK_PATH="$(xcrun --sdk iphoneos --show-sdk-path)"
echo "Building LLVM shader libraries for arm64 iOS"
cmake -S "$LLVM_SOURCE/llvm" -B "$LLVM_BUILD" "${COMMON_LLVM[@]}" \
    -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_SYSROOT="$SDK_PATH" -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 \
    -DLLVM_HOST_TRIPLE=aarch64-apple-ios17.0 -DLLVM_DEFAULT_TARGET_TRIPLE=aarch64-apple-ios17.0 \
    -DLLVM_TABLEGEN="$HOST_BUILD/bin/llvm-tblgen" -DLLVM_NATIVE_TOOL_DIR="$HOST_BUILD/bin"
LLVM_LIBRARIES=()
while IFS= read -r item; do LLVM_LIBRARIES+=("$item"); done < <(
    python3 - "$DXMT_SOURCE/src/airconv/meson.build" <<'PY'
import re,sys
s=open(sys.argv[1]).read().split('llvm_deps = [',1)[1].split(']',1)[0]
print('\n'.join(dict.fromkeys(re.findall(r"-l(LLVM[A-Za-z0-9]+)",s))))
PY
)
cmake --build "$LLVM_BUILD" --target "${LLVM_LIBRARIES[@]}" --parallel "$JOBS"
mkdir -p "$LLVM_PREFIX/include" "$LLVM_PREFIX/lib"
cp -R "$LLVM_SOURCE/llvm/include/llvm" "$LLVM_SOURCE/llvm/include/llvm-c" "$LLVM_PREFIX/include/"
cp -R "$LLVM_BUILD/include/llvm" "$LLVM_PREFIX/include/"
for lib in "${LLVM_LIBRARIES[@]}"; do cp "$LLVM_BUILD/lib/lib$lib.a" "$LLVM_PREFIX/lib/"; done

CROSS_FILE="$TASK_WORK/ios-arm64.meson"
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
echo "Building native iOS DXMT D3D11 backend"
MESON_ARGS=(--cross-file "$CROSS_FILE" --prefix "$PREFIX" --libdir lib
    --buildtype release -Dnative_llvm_path="$LLVM_PREFIX"
    -Denable_tests=false -Denable_d3d12=false -Denable_nvapi=false -Denable_nvngx=false)
if [ -f "$DXMT_BUILD/build.ninja" ]; then
    meson setup --reconfigure "$DXMT_BUILD" "$DXMT_SOURCE" "${MESON_ARGS[@]}"
else
    meson setup "$DXMT_BUILD" "$DXMT_SOURCE" "${MESON_ARGS[@]}"
fi
meson compile -C "$DXMT_BUILD" -j "$JOBS"
meson install -C "$DXMT_BUILD"
python3 - "$PREFIX" "$DXMT_SOURCE" "$LLVM_SOURCE" <<'PY'
import hashlib,json,subprocess,sys
from pathlib import Path
p,dxmt,llvm=map(Path,sys.argv[1:]);lib=p/'lib/libdxmt-native.dylib'
result=dict(dxmt_commit=subprocess.check_output(['git','-C',str(dxmt),'rev-parse','HEAD'],text=True).strip(),
    llvm_commit=subprocess.check_output(['git','-C',str(llvm),'rev-parse','HEAD'],text=True).strip(),
    platform='arm64-apple-ios17.0',purpose='native D3D11 backend; not yet connected to WindowsiOS WDDM',
    library=str(lib),sha256=hashlib.file_digest(lib.open('rb'),'sha256').hexdigest())
(p/'BUILD.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result,indent=2))
PY
