#!/usr/bin/env python3
"""Exercise ISO import against real UDF fixtures and independently read FAT32 output.

Requires build/iso_prepare, build/iso_prepare_split_test, and the native wimlib
test tool. No iPad is accessed and no disk image is mounted.
"""
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parent.parent
NORMAL = ROOT / "build/iso_prepare"
SPLIT = ROOT / "build/iso_prepare_split_test"
WIM = ROOT / "work/iso-deps/wimlib-mac/wimlib-imagex"


def run(command, **kwargs):
    return subprocess.run([str(x) for x in command], capture_output=True, text=True, **kwargs)


class Fat32:
    """Small read-only verifier, independent of the importer's filesystem library."""
    def __init__(self, path):
        self.file = path.open("rb")
        mbr = self.read(0, 512)
        assert mbr[510:512] == b"\x55\xaa"
        assert mbr[450] == 0x0C, "WinPE installation files must be on a visible FAT32 partition"
        assert struct.unpack_from("<I", mbr, 440)[0] not in (0, 0xFFFFFFFF), "Windows needs a valid MBR disk identity"
        self.start = struct.unpack_from("<I", mbr, 454)[0] * 512
        boot = self.read(self.start, 512)
        assert boot[510:512] == b"\x55\xaa"
        assert struct.unpack_from("<H", boot, 11)[0] == 512
        self.cluster_bytes = boot[13] * 512
        reserved = struct.unpack_from("<H", boot, 14)[0]
        fatsz = struct.unpack_from("<I", boot, 36)[0]
        self.fat = self.read(self.start + reserved * 512, fatsz * 512)
        self.data = self.start + (reserved + boot[16] * fatsz) * 512
        self.root = struct.unpack_from("<I", boot, 44)[0]

    def read(self, offset, length):
        self.file.seek(offset)
        value = self.file.read(length)
        assert len(value) == length
        return value

    def clusters(self, first):
        visited = set()
        while first < 0x0FFFFFF8:
            assert first >= 2 and first not in visited
            visited.add(first)
            yield self.read(self.data + (first - 2) * self.cluster_bytes, self.cluster_bytes)
            first = struct.unpack_from("<I", self.fat, first * 4)[0] & 0x0FFFFFFF

    def entries(self, cluster):
        long_name = {}
        for block in self.clusters(cluster):
            for position in range(0, len(block), 32):
                entry = block[position:position + 32]
                if entry[0] == 0:
                    return
                if entry[0] == 0xE5:
                    long_name.clear()
                    continue
                if entry[11] == 0x0F:
                    long_name[entry[0] & 0x1F] = entry[1:11] + entry[14:26] + entry[28:32]
                    continue
                if entry[11] & 8:
                    long_name.clear()
                    continue
                if long_name:
                    raw = b"".join(long_name[n] for n in sorted(long_name))
                    chars = []
                    for n in range(0, len(raw), 2):
                        code = raw[n:n + 2]
                        if code in (b"\0\0", b"\xff\xff"):
                            break
                        chars.append(code)
                    name = b"".join(chars).decode("utf-16le")
                else:
                    base, extension = entry[:8].rstrip(b" "), entry[8:11].rstrip(b" ")
                    name = (base + (b"." + extension if extension else b"")).decode("cp437")
                long_name.clear()
                number = struct.unpack_from("<H", entry, 26)[0] | struct.unpack_from("<H", entry, 20)[0] << 16
                size = struct.unpack_from("<I", entry, 28)[0]
                yield name, number, size, bool(entry[11] & 16)

    def lookup(self, path):
        cluster = self.root
        for component in path.strip("/").split("/"):
            match = next((e for e in self.entries(cluster) if e[0].casefold() == component.casefold()), None)
            assert match is not None, f"missing file: {path}"
            cluster = match[1]
        return match

    def contents(self, path):
        _, first, size, directory = self.lookup(path)
        assert not directory
        if not size:
            return b""
        result = b"".join(self.clusters(first))[:size]
        assert len(result) == size
        return result


def make_iso(directory, output):
    result = run(["hdiutil", "makehybrid", "-udf", "-udf-volume-name", "WIN11TEST", "-o", output, directory])
    assert result.returncode == 0, result.stderr


def import_iso(binary, iso, output, scratch, **kwargs):
    scratch.mkdir()
    return run([binary, iso, output, scratch], **kwargs)


def main():
    with tempfile.TemporaryDirectory(prefix="windowsios-iso-", dir=ROOT / "work") as temporary:
        base = Path(temporary)
        source = base / "source"
        (source / "efi/boot").mkdir(parents=True)
        (source / "sources").mkdir()
        pe = bytearray(1024)
        pe[:2] = b"MZ"
        struct.pack_into("<I", pe, 60, 128)
        pe[128:134] = b"PE\0\0\x64\xaa"
        (source / "efi/boot/bootaa64.efi").write_bytes(pe)
        (source / "sources/boot.wim").write_bytes(b"MSWIM\0" + bytes(range(256)) * 8)
        (source / "Installation Read Me.txt").write_text("long filename\n")
        (source / "说明.txt").write_text("Windows ARM64 安装测试\n")
        payload = base / "payload"
        payload.mkdir()
        for n in range(128):
            (payload / f"file{n:03d}.bin").write_bytes(os.urandom(16384))
        result = run([WIM, "capture", payload, source / "sources/install.wim", "--compress=none", "--no-acls", "--norpfix", "--quiet"])
        assert result.returncode == 0, result.stderr
        iso = base / "valid.iso"
        make_iso(source, iso)

        output = base / "normal.img"
        result = import_iso(NORMAL, iso, output, base / "normal-scratch")
        assert result.returncode == 0, result.stderr
        volume = Fat32(output)
        for path in source.rglob("*"):
            if path.is_file():
                assert volume.contents(str(path.relative_to(source))) == path.read_bytes()
        answer = ET.fromstring(volume.contents("autounattend.xml"))
        assert answer.find(".//{*}component").attrib["processorArchitecture"] == "arm64"
        volume.file.close()
        print("PASS: UDF -> visible FAT32 boot media, nested files, long names, Unicode and exact contents")

        with output.open("r+b") as file:
            file.seek(450)
            file.write(b"\xef")
            file.seek(0)
            before_mbr = file.read(512)
        result = run([NORMAL, "--repair", output])
        assert result.returncode == 0 and "repaired" in result.stdout, result.stderr
        with output.open("rb") as file:
            after_mbr = file.read(512)
        assert [n for n in range(512) if before_mbr[n] != after_mbr[n]] == [450]
        volume = Fat32(output)
        assert volume.contents("sources/install.wim") == (source / "sources/install.wim").read_bytes()
        volume.file.close()
        result = run([NORMAL, "--repair", output])
        assert result.returncode == 0 and "unchanged" in result.stdout
        print("PASS: legacy installer migration changes only the hidden partition type and is idempotent")

        # Both v1.2 (ESP) and v1.3 (FAT32) left the MBR disk identity zero.
        # Repair must preserve the partition's start/length and all file data.
        for old_type in (0x0C, 0xEF):
            with output.open("r+b") as file:
                file.seek(440)
                file.write(bytes(4))
                file.seek(450)
                file.write(bytes([old_type]))
                file.seek(0)
                before_mbr = file.read(512)
            result = run([NORMAL, "--repair", output])
            assert result.returncode == 0 and "repaired" in result.stdout, result.stderr
            with output.open("rb") as file:
                after_mbr = file.read(512)
            changed = {n for n in range(512) if before_mbr[n] != after_mbr[n]}
            allowed = {440, 441, 442, 443} | ({450} if old_type == 0xEF else set())
            assert changed <= allowed and changed & {440, 441, 442, 443}
            volume = Fat32(output)
            assert volume.contents("sources/install.wim") == (source / "sources/install.wim").read_bytes()
            volume.file.close()
            result = run([NORMAL, "--repair", output])
            assert result.returncode == 0 and "unchanged" in result.stdout
        print("PASS: v1.2/v1.3 zero disk identities repaired without moving partitions or altering files; repeat is unchanged")

        split_output = base / "split.img"
        result = import_iso(SPLIT, iso, split_output, base / "split-scratch")
        assert result.returncode == 0, result.stderr
        volume = Fat32(split_output)
        partdir = base / "parts"
        partdir.mkdir()
        parts = [e for e in volume.entries(volume.lookup("sources")[1]) if e[0].lower().endswith(".swm")]
        assert len(parts) >= 2
        for name, _, _, _ in parts:
            (partdir / name.lower()).write_bytes(volume.contents("sources/" + name))
        volume.file.close()
        extracted = base / "extracted"
        extracted.mkdir()
        result = run([WIM, "apply", partdir / "install.swm", "1", extracted, "--ref=" + str(partdir / "install*.swm"), "--no-acls"])
        assert result.returncode == 0, result.stderr
        for path in payload.iterdir():
            assert (extracted / path.name).read_bytes() == path.read_bytes()
        print("PASS: WIM split parts reassemble and reproduce all installation payload bytes")

        cancel_output = base / "cancel.img"
        result = import_iso(NORMAL, iso, cancel_output, base / "cancel-scratch", env={**os.environ, "ISO_PREPARE_CANCEL_AT": "0.51"})
        assert result.returncode != 0 and not cancel_output.exists()
        assert "已取消" in result.stderr
        print("PASS: cancellation removes partially prepared installation media")

        existing = base / "existing.img"
        existing.write_bytes(b"EXISTING USER DATA")
        before = hashlib.sha256(existing.read_bytes()).digest()
        result = import_iso(NORMAL, iso, existing, base / "existing-scratch")
        assert result.returncode != 0 and hashlib.sha256(existing.read_bytes()).digest() == before
        print("PASS: existing output is never overwritten")

        scratch = base / "protected-scratch"
        scratch.mkdir()
        original = scratch / "install.wim"
        original.write_bytes(b"EXISTING CACHE DATA")
        result = run([SPLIT, iso, base / "protected.img", scratch])
        assert result.returncode != 0 and original.read_bytes() == b"EXISTING CACHE DATA"
        print("PASS: pre-existing scratch files are not deleted after a failed import")

        pe[132:134] = b"\x64\x86"
        (source / "efi/boot/bootaa64.efi").write_bytes(pe)
        wrong = base / "wrong-architecture.iso"
        make_iso(source, wrong)
        bad_output = base / "wrong.img"
        result = import_iso(NORMAL, wrong, bad_output, base / "wrong-scratch")
        assert result.returncode != 0 and "ARM64" in result.stderr and not bad_output.exists()
        print("PASS: x86-64 PE in a renamed ARM64 boot file is rejected")

        invalid = base / "invalid.iso"
        invalid.write_bytes(bytes(65536))
        result = import_iso(NORMAL, invalid, base / "invalid.img", base / "invalid-scratch")
        assert result.returncode != 0 and not (base / "invalid.img").exists()
        print("PASS: malformed ISO fails without leaving an installation image")


if __name__ == "__main__":
    main()
