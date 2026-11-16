#!/usr/bin/env python3
"""Stage a verified cloud-built diagnostic package as Windows-visible FAT32 media."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
BUNDLE = ROOT / "ios/Sources/Resources/GpuDiagnostics"
FILES = {"wingpudiag.inf", "WinGpuDiagnostic.sys", "WinGpuProbe.exe", "wingpudiagnostic.cat",
         "WinGpuDiagnostic-test.cer", "BUILD.json", "README.md", "LICENSE.txt"}


def require(condition, message="Invalid diagnostic package"):
    if not condition:
        raise ValueError(message)


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def verify_pe(path, subsystem, signed=False):
    data = path.read_bytes()
    require(data[:2] == b"MZ" and len(data) >= 256, str(path))
    pe = struct.unpack_from("<I", data, 60)[0]
    require(pe <= len(data) - 256 and data[pe:pe+4] == b"PE\0\0", str(path))
    require(struct.unpack_from("<H", data, pe+4)[0] == 0xAA64, str(path))
    optional = pe + 24
    require(struct.unpack_from("<H", data, optional)[0] == 0x20B, str(path))
    require(struct.unpack_from("<H", data, optional+68)[0] == subsystem, str(path))
    if signed:
        offset, size = struct.unpack_from("<II", data, optional + 112 + 4*8)
        require(offset and size >= 8 and offset + size <= len(data), str(path))
    return data


def verify_bundle():
    manifest = json.loads((BUNDLE / "manifest.json").read_text())
    image = BUNDLE / "drivers.img"
    require(image.stat().st_size == manifest["image_bytes"] == 64 << 20)
    require(digest(image) == manifest["image_sha256"])
    print("Verified staged ARM64 diagnostic driver media:", manifest["source_commit"])


def stage(package):
    checksums = {}
    for line in (package / "SHA256SUMS.txt").read_text().splitlines():
        sha, name = line.split("  ", 1)
        require(name in FILES and name not in checksums and len(sha) == 64)
        checksums[name] = sha
    require(set(checksums) == FILES)
    for name, sha in checksums.items():
        path = package / name
        require(not path.is_symlink() and path.is_file() and digest(path) == sha, name)
    build = json.loads((package / "BUILD.json").read_text(encoding="utf-8-sig"))
    require(build["architecture"] == "ARM64" and build["test_signed"] is True)
    require(build["production_signed"] is False and build["direct3d_implemented"] is False)
    require(build["private_key_exported"] is False)
    require(len(build["source_commit"]) == 40)
    verify_pe(package / "WinGpuDiagnostic.sys", 1, signed=True)
    probe = verify_pe(package / "WinGpuProbe.exe", 3)
    require(b"VCRUNTIME140.dll" not in probe and b"MSVCP140.dll" not in probe)
    subprocess.run(["make", "build/driver_media"], cwd=ROOT, check=True)
    BUNDLE.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=ROOT / "build", prefix="gpu-media-") as directory:
        image = Path(directory) / "drivers.img"
        subprocess.run([str(ROOT / "build/driver_media"), str(image),
                        *[str(package / n) for n in sorted(FILES)], str(package / "SHA256SUMS.txt")], check=True)
        with image.open("rb") as stream:
            mbr = stream.read(512)
            require(mbr[510:] == b"\x55\xaa" and mbr[450] == 0x0C)
            require(struct.unpack_from("<I", mbr, 440)[0] not in (0, 0xFFFFFFFF))
            start, sectors = struct.unpack_from("<II", mbr, 454)
            require(start and start + sectors <= (64 << 20) // 512)
            stream.seek(start * 512)
            boot = stream.read(512)
            require(boot[82:90] == b"FAT32   ")
            reserved = struct.unpack_from("<H", boot, 14)[0]
            fat_sectors = struct.unpack_from("<I", boot, 36)[0]
            root_cluster = struct.unpack_from("<I", boot, 44)[0]
            root_sector = start + reserved + boot[16] * fat_sectors + (root_cluster - 2) * boot[13]
            stream.seek(root_sector * 512)
            root = stream.read(512)
            require(any(root[o:o+11] == b"WINGPUTEST " and root[o+11] == 8 for o in range(0, 512, 32)))
        manifest = dict(image_sha256=digest(image), image_bytes=image.stat().st_size,
                        source_commit=build["source_commit"], build_run=build["build_run"],
                        certificate_thumbprint=build["certificate_thumbprint"], direct3d_implemented=False)
        os.replace(image, BUNDLE / "drivers.img")
        (BUNDLE / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        shutil.copy2(package / "README.md", BUNDLE / "README.md")
    verify_bundle()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", type=Path)
    args = parser.parse_args()
    if args.package:
        stage(args.package)
    else:
        verify_bundle()
