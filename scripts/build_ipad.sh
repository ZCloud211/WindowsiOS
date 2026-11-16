#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build an arm64 iOS app and an unsigned IPA for local development/sideloading.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export PATH="$ROOT/work/tools/bin:$ROOT/work/tools/python/bin:$ROOT/work/tools/xcodegen/bin:/opt/homebrew/bin:$PATH"
cd "$ROOT"
command -v xcodegen >/dev/null || { echo "Install XcodeGen before building." >&2; exit 1; }
python3 scripts/prepare_uefi.py --stage-bundle
python3 scripts/prepare_ramfb_firmware.py
python3 scripts/prepare_gpu3d.py
python3 scripts/prepare_virtio_display.py
bash scripts/build_ios_import_deps.sh

if [ ! -f ios/deps/lib/libslirp.a ]; then
    bash scripts/build_ios_deps.sh
fi
make -j"$(sysctl -n hw.ncpu)" ios-lib
xcodegen generate --spec ios/project.yml

# Pass DEVELOPMENT_TEAM and SIGN_APP=1 to build using an existing Xcode account.
SIGN_ARGS=(CODE_SIGNING_ALLOWED=NO)
DESTINATION='generic/platform=iOS'
if [ "${SIGN_APP:-0}" = 1 ]; then
    : "${DEVELOPMENT_TEAM:?Set DEVELOPMENT_TEAM to your Apple development team ID}"
    SIGN_ARGS=(CODE_SIGNING_ALLOWED=YES "DEVELOPMENT_TEAM=$DEVELOPMENT_TEAM" -allowProvisioningUpdates)
    if [ -n "${DEVICE_ID:-}" ]; then
        DESTINATION="id=$DEVICE_ID"
        SIGN_ARGS+=(-allowProvisioningDeviceRegistration)
    fi
fi
xcodebuild -project ios/WindowsiOS.xcodeproj -scheme WindowsiOS \
    -configuration Release -sdk iphoneos -destination "$DESTINATION" \
    -derivedDataPath build/DerivedData "${SIGN_ARGS[@]}" build

APP="$ROOT/build/DerivedData/Build/Products/Release-iphoneos/WindowsiOS.app"
STAGE="$ROOT/build/ipad-package"
mkdir -p "$STAGE/Payload"
rm -rf "$STAGE/Payload/WindowsiOS.app"
ditto "$APP" "$STAGE/Payload/WindowsiOS.app"
IPA="$ROOT/build/WindowsiOS-iPad-unsigned.ipa"
if [ "${SIGN_APP:-0}" = 1 ]; then IPA="$ROOT/build/WindowsiOS-iPad-development.ipa"; fi
rm -f "$IPA"
(cd "$STAGE" && /usr/bin/zip -qry "$IPA" Payload)
echo "App: $APP"
echo "IPA: $IPA"
if [ "${SIGN_APP:-0}" != 1 ]; then
    echo "Unsigned: sign with your sideloading tool, preserving get-task-allow, before installing."
fi
