"""Package the reviewed publisher executables, configuration, and documentation.

Maintainer use: python tools/publisher/package_release.py --source PATH --output-dir PATH
The five pinned inputs must match the already distributed 2026-09-06 tools.
No network requests, application uploads, signing keys, or server changes occur.
"""
from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import stat
import struct
import zipfile

PINNED = {
    "c1publish.exe": "ed2ca7db3c30d27d412adc91eb7a6f65eb0712471d832a792c143cce5d27a639",
    "c1publish-linux-amd64": "a21aa32ba306c31dd0b4554bb88245a9081508f6d5333911d7b719ba5666395b",
    "c1publish-linux-arm64": "668d46fc674faf00d0df54c9c0825d9bc9d8a1dc53b8e911cfbc44856c50cf08",
    "server.url": "29da1c53bafd2424c0e31072ede2c6d4029f2ba952cf5101405a6fe2906ae31e",
    "repository.ed25519.pub": "f18a060a6125bd96e877f5518e9f44c249e17cda12f87202eb0008056ab3cd27",
}
DOCS = ("README.md", "DISTRIBUTION-NOTE.txt", "THIRD_PARTY_NOTICES.txt")
ASSET = "C1Slim-Publisher-20260907.zip"
PREFIX = "C1-Open-Publisher/"


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def require(ok: bool, message: str) -> None:
    if not ok:
        raise ValueError(message)


def validate_binary(name: str, data: bytes) -> None:
    if name.endswith(".exe"):
        require(data[:2] == b"MZ" and len(data) >= 64, "Expected Windows PE")
        offset = struct.unpack_from("<I", data, 60)[0]
        require(data[offset:offset + 4] == b"PE\0\0", "Missing PE signature")
        require(struct.unpack_from("<H", data, offset + 4)[0] == 0x8664,
                "Expected Windows x64")
        return
    require(data[:6] == b"\x7fELF\x02\x01", "Expected little-endian ELF64")
    machine = struct.unpack_from("<H", data, 18)[0]
    require(machine == (62 if name.endswith("amd64") else 183), "Wrong ELF machine")
    phoff = struct.unpack_from("<Q", data, 32)[0]
    phentsize, phnum = struct.unpack_from("<HH", data, 54)
    require(phentsize >= 56 and phoff + phentsize * phnum <= len(data),
            "Invalid ELF program header table")
    for i in range(phnum):
        kind = struct.unpack_from("<I", data, phoff + i * phentsize)[0]
        require(kind not in (2, 3), "Expected static ELF without dynamic loader")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    source = args.source.resolve(strict=True)
    docs = Path(__file__).resolve().parent
    output = args.output_dir.resolve()
    archive = output / ASSET
    checksum = output / (ASSET + ".sha256")
    require(not archive.exists() and not checksum.exists(), "Refusing to replace existing assets")
    payload = {}
    for name, expected in PINNED.items():
        path = source / name
        require(path.is_file() and not path.is_symlink(), f"Not a regular input: {name}")
        data = path.read_bytes()
        require(digest(data) == expected, f"Pinned SHA-256 mismatch: {name}")
        if name.startswith("c1publish"):
            validate_binary(name, data)
        payload[name] = data
    require(len(payload["repository.ed25519.pub"]) == 32, "Expected raw Ed25519 public key")
    for name in DOCS:
        # Universal-newline decoding keeps Windows and Linux packaging identical.
        payload[name] = (docs / name).read_text(encoding="utf-8").encode("utf-8")
    payload["SHA256SUMS"] = "".join(
        f"{digest(data)}  {name}\n" for name, data in sorted(payload.items())
    ).encode("utf-8")
    output.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive, "x", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as bundle:
        for name, data in sorted(payload.items()):
            info = zipfile.ZipInfo(PREFIX + name, date_time=(2026, 9, 7, 0, 0, 0))
            info.create_system = 3
            mode = 0o755 if name.startswith("c1publish") else 0o644
            info.external_attr = (stat.S_IFREG | mode) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            bundle.writestr(info, data)
    with zipfile.ZipFile(archive) as bundle:
        require(bundle.testzip() is None, "ZIP CRC validation failed")
        require(sorted(bundle.namelist()) == sorted(PREFIX + name for name in payload),
                "Unexpected ZIP members")
        for name, data in payload.items():
            require(bundle.read(PREFIX + name) == data, f"ZIP member differs: {name}")
    sha = digest(archive.read_bytes())
    with checksum.open("x", encoding="utf-8", newline="\n") as stream:
        stream.write(f"{sha}  {ASSET}\n")
    print(f"Verified {len(payload)} explicit archive members.")
    print(f"{sha}  {archive.name}")
    print(archive)
    print(checksum)


if __name__ == "__main__":
    main()
