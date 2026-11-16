#!/usr/bin/env python3
"""Fetch QEMU's pinned EDK2 ARM64 firmware into work/uefi for boot testing."""
import bz2
import argparse
import hashlib
import shutil
from pathlib import Path
from urllib.request import urlopen

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "work" / "uefi"
MACHINE = OUT / "Machines" / "Win11"
BASE = "https://raw.githubusercontent.com/qemu/qemu/v9.2.0/pc-bios/"
FILES = [
    ("edk2-aarch64-code.fd.bz2", "code.fd", "2ce728c9ed6117214a1faebb9908fcc463110d4b"),
    ("edk2-arm-vars.fd.bz2", "vars.fd", "2a5a9af31a6c8f33f38598aebe014f37253d6fcc"),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage-bundle", action="store_true", help="Include firmware and licenses in the iOS app resources")
    args = parser.parse_args()
    MACHINE.mkdir(parents=True, exist_ok=True)
    for source, name, expected in FILES:
        destination = MACHINE / name
        if destination.exists():
            print(f"Preserving existing {destination}")
            continue
        with urlopen(BASE + source, timeout=60) as response:
            data = response.read()
        digest = hashlib.sha1(b"blob " + str(len(data)).encode() + b"\0" + data).hexdigest()
        if digest != expected:
            raise RuntimeError(f"Unexpected firmware contents: {source}")
        firmware = bz2.decompress(data)
        if len(firmware) != 64 << 20:
            raise RuntimeError(f"Unexpected firmware size: {source}: {len(firmware)}")
        destination.write_bytes(firmware)
        print(f"Prepared {destination} ({len(firmware) >> 20} MiB)")
    if not (OUT / "edk2-licenses.txt").exists():
        with urlopen(BASE + "edk2-licenses.txt", timeout=60) as response:
            (OUT / "edk2-licenses.txt").write_bytes(response.read())
    (OUT / "SOURCE.txt").write_text(
        "Firmware: https://github.com/qemu/qemu/tree/v9.2.0/pc-bios\n"
        "EDK2 license notices: edk2-licenses.txt\n"
        "Copy Machines/Win11 into the app's Documents directory.\n"
    )
    if args.stage_bundle:
        bundle = ROOT / "ios" / "Sources" / "Resources" / "Firmware"
        bundle.mkdir(parents=True, exist_ok=True)
        for name in ("code.fd", "vars.fd"):
            shutil.copy2(MACHINE / name, bundle / name)
        for name in ("edk2-licenses.txt", "SOURCE.txt"):
            shutil.copy2(OUT / name, bundle / name)
        print(f"Staged bundled UEFI firmware and license notices: {bundle}")


if __name__ == "__main__":
    main()
