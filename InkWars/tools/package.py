#!/usr/bin/env python3
"""Build an unsigned, offline GNU-tar c1pkg. No network or signing keys used."""
import argparse
import gzip
import hashlib
import io
import json
from pathlib import Path
import re
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
PACKAGE_ID = "inkwars"
DISPLAY_NAME = "Ink Wars"
ENTRY = "inkwars"


def verify_binary(binary, readelf):
    report = subprocess.check_output([readelf, "-h", "-l", "-A", str(binary)], text=True)
    checks = {
        "ELF32": "Class:                             ELF32" in report,
        "little endian": "little endian" in report,
        "MIPS machine": re.search(r"Machine:\s+MIPS", report) is not None,
        "MIPS32r2": "mips32r2" in report,
        "o32 ABI": "o32" in report,
        "hard float": re.search(r"FP ABI:\s+Hard float", report) is not None,
        "32-bit FP registers": re.search(r"CPR1 size:\s+32", report) is not None,
        "static executable": "INTERP" not in report and "DYNAMIC" not in report,
    }
    failed = [name for name, passed in checks.items() if not passed]
    if failed:
        raise ValueError("ELF verification failed: " + ", ".join(failed) + "\n" + report)
    print("Verified: ELF32 little-endian MIPS32r2 o32 hard-float FP32, static")
    return report


def add_member(tar, name, data=None, mode=0o644):
    info = tarfile.TarInfo(name)
    info.uid = info.gid = 0
    info.uname = info.gname = "root"
    info.mtime = 0
    info.mode = mode
    if data is None:
        info.type = tarfile.DIRTYPE
        tar.addfile(info)
    else:
        info.size = len(data)
        if info.size > 16 * 1024 * 1024:
            raise ValueError("payload member exceeds 16 MiB")
        tar.addfile(info, io.BytesIO(data))


def verify_archive(path, manifest):
    total = 0
    with tarfile.open(path, "r:gz") as tar:
        names = set()
        for member in tar:
            name = member.name.rstrip("/")
            if name in names:
                raise ValueError("duplicate archive member")
            names.add(name)
            if member.pax_headers or not (member.isdir() or member.isfile()):
                raise ValueError("only plain GNU tar files/directories allowed")
            if name.startswith("/") or any(p == ".." for p in name.split("/")) or any(c in name for c in "\\:*?["):
                raise ValueError("unsafe archive path")
            if name != "manifest.v1" and name != "payload" and not name.startswith("payload/"):
                raise ValueError("unexpected top-level archive member")
            total += member.size
        if total > 64 * 1024 * 1024 or path.stat().st_size > 32 * 1024 * 1024:
            raise ValueError("c1pkg size limit exceeded")
        if tar.extractfile("manifest.v1").read() != manifest:
            raise ValueError("manifest mismatch")
        if not tar.getmember("payload/" + ENTRY).mode & 0o111:
            raise ValueError("entry is not executable")
    with gzip.open(path, "rb") as stream:
        if stream.read(265)[257:265] != b"ustar  \x00":
            raise ValueError("archive does not use GNU tar headers")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "build/inkwars")
    VERSION = re.search(r'#define IW_VERSION "([0-9.]+)"', (ROOT / 'src/version.h').read_text()).group(1)
    parser.add_argument("--version", default=VERSION)
    parser.add_argument("--sequence", type=int, default=1, help="unsigned offline index sequence; choose a higher value before real repository signing")
    parser.add_argument("--output", type=Path, default=ROOT / "build/package")
    parser.add_argument("--readelf", default="mipsel-linux-gnu-readelf")
    parser.add_argument("--verify-only", action="store_true")
    args = parser.parse_args()
    if not re.fullmatch(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", args.version):
        parser.error("version must be a numeric semantic version")
    if not 1 <= args.sequence <= 2147483647:
        parser.error("sequence must be a positive 31-bit integer")
    binary = args.binary.resolve()
    embedded_version = re.search(r'#define IW_VERSION "([0-9.]+)"', (ROOT / 'src/version.h').read_text()).group(1)
    if args.version != embedded_version:
        parser.error('package version must match src/version.h; rebuild after changing it')
    report = verify_binary(binary, args.readelf)
    if args.verify_only:
        return
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if (output / "index.v1.sig").exists():
        raise ValueError("refusing to replace an index beside an existing signature; use a fresh output directory")
    (output / "abi.txt").write_text(report, encoding="utf-8")
    payload = output / "payload"
    payload.mkdir(exist_ok=True)
    launch = (ROOT / "launch.sh").read_bytes().replace(b"\r\n", b"\n")
    files = [("launch.sh", launch, 0o755), (ENTRY, binary.read_bytes(), 0o755)]
    for license_path in sorted((ROOT / "licenses").glob("*.txt")):
        files.append(("licenses/" + license_path.name, license_path.read_bytes(), 0o644))
    for document in ("README.md", "TESTING.md", "KNOWN_ISSUES.md", "CHANGELOG.md", "LICENSE"):
        if (ROOT / document).is_file():
            files.append((document, (ROOT / document).read_bytes(), 0o644))
    for name, data, mode in files:
        path = payload / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        path.chmod(mode)
    manifest = ("C1PKG-PACKAGE 2\nid\t" + PACKAGE_ID + "\nversion\t" + args.version + "\nentry\t" + ENTRY + "\nmode\tdirect\n").encode("ascii")
    relative = "packages/" + PACKAGE_ID + "/" + args.version + ".tar.gz"
    archive = output / relative
    archive.parent.mkdir(parents=True, exist_ok=True)
    with archive.open("wb") as raw:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as zipped:
            with tarfile.open(fileobj=zipped, mode="w", format=tarfile.GNU_FORMAT) as tar:
                add_member(tar, "manifest.v1", manifest)
                add_member(tar, "payload/", mode=0o755)
                if any(name.startswith("licenses/") for name, _, _ in files):
                    add_member(tar, "payload/licenses/", mode=0o755)
                for name, data, mode in files:
                    add_member(tar, "payload/" + name, data, mode)
    verify_archive(archive, manifest)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    catalog = {"packages": [{"id": PACKAGE_ID, "version": args.version, "displayName": DISPLAY_NAME,
                            "entry": ENTRY, "payloadDirectory": "payload"}]}
    (output / "catalog.json").write_text(json.dumps(catalog, indent=2) + "\n", encoding="ascii")
    index = ("C1PKG-INDEX 1\nS\t" + str(args.sequence) + "\nP\t" + PACKAGE_ID + "\t" + args.version
             + "\t" + DISPLAY_NAME + "\t" + relative + "\t" + digest + "\t" + str(archive.stat().st_size) + "\t" + ENTRY + "\n")
    if len(index.encode("ascii")) > 256 * 1024 or any(len(line) > 1024 for line in index.splitlines()):
        raise ValueError("index exceeds platform limits")
    (output / "index.v1").write_text(index, encoding="ascii")
    (output / "SHA256SUMS").write_text(digest + "  " + relative + "\n", encoding="ascii")
    (output / "OFFLINE.txt").write_text(
        "UNSIGNED OFFLINE PACKAGE\nNo index.v1.sig is generated; no signature is fabricated.\n"
        "The signed repository installer will require a genuine Ed25519 signature\n"
        "from its trusted repository key and an appropriate monotonic sequence.\n"
        "No device deployment has been performed by this packaging tool.\n", encoding="ascii")
    print("Package:", archive)
    print("SHA256:", digest)
    print("Unsigned offline artifact: no index.v1.sig generated.")


if __name__ == "__main__":
    main()
