#!/usr/bin/env python3
"""Offline, allowlisted external-payload assembler. Never signs or runs payloads."""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import struct
import sys
import tempfile

PROJECT = Path(__file__).resolve().parents[1]
ORIGINAL_USB_SHA256 = "c2b278b283e9bf851461d9e8f6edfd207cec3b120585f0e091777d163562e965"
ORIGINAL_DAEMON_SHA256 = "ceb56ddf2ff3c10f7c4c2cd6216b298da1cea799ca7170322f8229d5e9af6ee7"
APP_URL = "http://www.fwz233.com/c1/v2"
CORE_URL = "http://www.fwz233.com/c1/core/v1/stable"
PUBLISH_URL = "http://www.fwz233.com"
ENROLLMENT = (
    "app-daemon-bootstrap.sh", "device-core-enroll.sh", "enroll.sh",
    "core.ed25519.pub", "core.ed25519.pem", "bootstrap.v1", "bootstrap.v1.sig",
    "release/manifest.v1", "release/manifest.v1.sig", "release/artifacts/C1ancher",
    "release/artifacts/c1pkg", "release/artifacts/C1ancher-launcher", "release/artifacts/c1updater",
)
LAYERS = {
    "profile": ("repository.url", "core-repository.url", "repository.ed25519.pub",
                "device-repository-config.sh", "c1-update-check.sh"),
    "accessories": ("neofetch", "neofetch.upstream", "c1-config.conf", "c1-logo.txt", "LICENSE.md", "wallpaper.raw"),
    "usb": ("S90usb.original", "S90usb.open", "device-open-adb.sh"),
    "developer": ("c1publish.exe", "c1publish-linux-amd64", "c1publish-linux-arm64",
                  "README.md", "repository.ed25519.pub", "server.url"),
    "tools": ("adb.exe", "AdbWinApi.dll", "AdbWinUsbApi.dll"),
}
# These sit OUTSIDE enrollment: the device verifier requires exactly 13 files.
ENROLLMENT_SUMS = {
    "enrollment.SHA256SUMS": ("enrollment", ENROLLMENT),
    "enrollment-release.SHA256SUMS": ("enrollment/release", tuple(p.removeprefix("release/") for p in ENROLLMENT if p.startswith("release/"))),
    "enrollment-artifacts.SHA256SUMS": ("enrollment/release/artifacts", tuple(p.rsplit("/", 1)[1] for p in ENROLLMENT if p.startswith("release/artifacts/"))),
}
ROOT_REQUIRED = {"BUNDLE-STATUS.json", "README.txt", "USER-GUIDE.md", "THIRD-PARTY-NOTICES.txt", "PLATFORM-TOOLS-NOTICE.txt"}
ROOT_OPTIONAL = {"C1SlimInstaller.exe", "GO-LICENSE.txt", "DOTNET-THIRD-PARTY-NOTICES.txt"}
MAX_FILE = 128 * 1024 * 1024
MAX_PAYLOAD = 512 * 1024 * 1024

# Only unsigned, source-controlled shell text is normalized during assembly.
# Signed enrollment and pinned factory scripts must remain byte-for-byte intact.
SHELL_SOURCES = {
    "device-setup.sh": "installer/device-setup.sh",
    "usb/device-open-adb.sh": "scripts/device-open-adb.sh",
    "profile/device-repository-config.sh": "scripts/device-repository-config.sh",
    "profile/c1-update-check.sh": "scripts/c1-update-check.sh",
    "accessories/neofetch": "third_party/neofetch/neofetch",
    "accessories/neofetch.upstream": "third_party/neofetch/neofetch.upstream",
    "accessories/c1-config.conf": "third_party/neofetch/c1-config.conf",
}
SIGNED_SHELL = tuple("enrollment/" + name for name in ENROLLMENT if name.endswith(".sh"))
DEVICE_SHELL = tuple(SHELL_SOURCES) + SIGNED_SHELL + ("usb/S90usb.original", "usb/S90usb.open")


def validate_shell_text(data: bytes, label: str) -> None:
    require(bool(data) and b"\r" not in data and b"\0" not in data
            and not data.startswith(b"\xef\xbb\xbf"),
            f"Device shell text must use LF and no BOM/CR/NUL: {label}")
    try:
        data.decode("utf-8", errors="strict")
    except UnicodeDecodeError:
        raise BundleError(f"Device shell text must be valid UTF-8: {label}") from None
    if label != "accessories/c1-config.conf":
        require(data.startswith(b"#!/"), f"Device shell script must start with a shebang: {label}")


def normalize_shell_source(data: bytes, label: str) -> bytes:
    require(label in SHELL_SOURCES, f"Refusing to normalize signed/pinned or unknown input: {label}")
    result = data.replace(b"\r\n", b"\n")
    validate_shell_text(result, label)
    return result


class BundleError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise BundleError(message)


def safe_path(path: Path, *, missing: bool = False) -> Path:
    """Reject symlinks, junctions/reparse points and linked/special files."""
    path = Path(os.path.abspath(path))
    for part in (path, *path.parents):
        try:
            info = part.lstat()
        except FileNotFoundError:
            if missing:
                continue
            raise BundleError(f"Missing input: {path}") from None
        require(not stat.S_ISLNK(info.st_mode) and not (
            getattr(info, "st_file_attributes", 0) & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)
        ), f"Links/reparse points are forbidden: {part}")
        require(stat.S_ISREG(info.st_mode) or stat.S_ISDIR(info.st_mode), f"Special file forbidden: {part}")
        if stat.S_ISREG(info.st_mode):
            require(info.st_nlink == 1, f"Hard-linked file forbidden: {part}")
    return path


def read_file(path: Path, limit: int = MAX_FILE) -> bytes:
    path = safe_path(path)
    require(path.is_file(), f"Expected regular file: {path}")
    require(0 < path.stat().st_size <= limit, f"Empty or oversized file: {path}")
    return path.read_bytes()


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def write_new(path: Path, data: bytes) -> None:
    safe_path(path, missing=True)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("xb") as stream:
        stream.write(data)


def exact_tree(root: Path, files: set[str], *, optional: set[str] | None = None) -> None:
    """List names only; never read unexpected files or descend unexpected dirs."""
    safe_path(root)
    require(root.is_dir(), f"Expected directory: {root}")
    optional = optional or set()
    allowed = files | optional
    dirs = {str(p) for name in allowed for p in Path(name).parents if str(p) != "."}
    dirs = {p.replace("\\", "/") for p in dirs}
    seen: set[str] = set()
    def visit(directory: Path) -> None:
        for entry in directory.iterdir():
            name = entry.relative_to(root).as_posix()
            require(name in allowed or name in dirs, f"Unexpected bundle entry (not read): {name}")
            safe_path(entry)
            if entry.is_dir():
                require(name in dirs, f"Unexpected directory: {name}")
                visit(entry)
            else:
                require(name in allowed, f"Unexpected file: {name}")
                seen.add(name)
    visit(root)
    require(files <= seen, "Missing bundle files: " + ", ".join(sorted(files - seen)))


def canonical_lines(data: bytes, label: str) -> list[str]:
    require(0 < len(data) <= 65536 and data.endswith(b"\n") and
            all(c in (9, 10) or 32 <= c <= 126 for c in data), f"Non-canonical signed manifest: {label}")
    return data[:-1].decode("ascii").split("\n")


def verify_signature(content: bytes, signature: bytes, key: bytes, label: str) -> None:
    require(len(key) == 32 and len(signature) == 64, f"Invalid Ed25519 length: {label}")
    try:
        from cryptography.exceptions import InvalidSignature
        from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
    except ImportError:
        raise BundleError("Signature verification requires Python package cryptography; no verification bypass is available.") from None
    try:
        Ed25519PublicKey.from_public_bytes(key).verify(signature, content)
    except (InvalidSignature, ValueError):
        raise BundleError(f"Ed25519 signature mismatch: {label}") from None


def validate_elf(data: bytes, label: str) -> None:
    """Offline header/ABI checks matching the GUI; never load or execute ELF."""
    require(len(data) >= 52 and data[:7] == b"\x7fELF\x01\x01\x01" and
            struct.unpack_from("<HH", data, 16) == (2, 8), f"Invalid static MIPS ELF32: {label}")
    flags = struct.unpack_from("<I", data, 36)[0]
    require(flags & 0xF000F000 == 0x70001000, f"MIPS32r2 o32 ABI required: {label}")
    start = struct.unpack_from("<I", data, 28)[0]
    width, count = struct.unpack_from("<HH", data, 42)
    require(width == 32 and count > 0 and start >= 52 and start + width * count <= len(data),
            f"Invalid ELF program headers: {label}")
    abi = False
    for offset in range(start, start + width * count, width):
        kind, position = struct.unpack_from("<II", data, offset)
        require(kind not in (2, 3), f"Dynamic ELF/interpreter forbidden: {label}")
        if kind == 0x70000003:
            require(position + 24 <= len(data) and data[position + 2:position + 4] == b"\x20\x02" and
                    data[position + 7] == 1, f"MIPS32r2 hard-float double ABI required: {label}")
            abi = True
    require(abi, f"MIPS ABI metadata missing: {label}")


def validate_enrollment(root: Path, expected_key_sha256: str | None = None) -> dict[str, str]:
    exact_tree(root, set(ENROLLMENT))
    data = {name: read_file(root / name, 32 * 1024 * 1024) for name in ENROLLMENT}
    key = data["core.ed25519.pub"]
    require(len(key) == 32, "Core public key must be 32 raw bytes")
    key_hash = digest(key)
    if expected_key_sha256 is not None:
        require(key_hash == expected_key_sha256, "Core trust key mismatch; refusing to replace the trust root")
    pem = data["core.ed25519.pem"]
    match = re.fullmatch(rb"-----BEGIN PUBLIC KEY-----\r?\n([A-Za-z0-9+/=\r\n]+)-----END PUBLIC KEY-----\r?\n?", pem)
    require(match is not None, "Core PEM must contain only a PUBLIC KEY, never a private key")
    der = base64.b64decode(re.sub(rb"[\r\n]", b"", match[1]), validate=True)
    require(der == bytes.fromhex("302a300506032b6570032100") + key, "Core raw/PEM public keys mismatch")
    verify_signature(data["bootstrap.v1"], data["bootstrap.v1.sig"], key, "bootstrap.v1")
    bootstrap = canonical_lines(data["bootstrap.v1"], "bootstrap.v1")
    require(11 <= len(bootstrap) <= 26 and bootstrap[:2] == ["C1CORE-BOOTSTRAP 1", "V\t1.1.0"], "Unsupported bootstrap contract")
    bindings = zip("KPBDLMGU", ("core.ed25519.pub", "core.ed25519.pem", "app-daemon-bootstrap.sh",
                   "device-core-enroll.sh", "enroll.sh", "release/manifest.v1", "release/manifest.v1.sig", "release/artifacts/c1updater"))
    for i, (tag, name) in enumerate(bindings, 2):
        require(bootstrap[i] == tag + "\t" + digest(data[name]), f"Bootstrap binding mismatch: {name}; replace the complete signed enrollment directory")
    baselines = bootstrap[10:]
    require(all(re.fullmatch(r"H\t[0-9a-f]{64}", line) for line in baselines) and
            len(set(baselines)) == len(baselines) and "H\t" + ORIGINAL_DAEMON_SHA256 in baselines,
            "Bootstrap does not approve the required factory daemon baseline")
    verify_signature(data["release/manifest.v1"], data["release/manifest.v1.sig"], key, "release/manifest.v1")
    release = canonical_lines(data["release/manifest.v1"], "release/manifest.v1")
    require(len(release) == 14 and release[0] == "C1CORE-MANIFEST 1", "Invalid release manifest contract")
    for index, tag in ((1, "S"), (3, "E"), (9, "D")):
        require(re.fullmatch(tag + r"\t[1-9][0-9]{0,19}", release[index]) is not None and
                int(release[index][2:]) <= 2**64 - 1, f"Invalid release numeric field: {tag}")
    for index, tag in ((2, "V"), (5, "B"), (6, "U"), (7, "C"), (8, "R")):
        require(re.fullmatch(tag + r"\t[A-Za-z0-9](?:[A-Za-z0-9._+-]{0,62}[A-Za-z0-9])?", release[index]) is not None,
                f"Invalid release token: {tag}")
    require(release[4] == "T\tmips32r2-little-o32-hard-float-double-static", "Unexpected release target ABI")
    total = 0
    for index, (role, name) in enumerate(zip(("c1ancher", "c1pkg", "launcher", "updater"),
                                          ("C1ancher", "c1pkg", "C1ancher-launcher", "c1updater")), 10):
        binary = data["release/artifacts/" + name]
        expected = ["F", role, "artifacts/" + name, digest(binary), str(len(binary)), "700"]
        require(release[index].split("\t") == expected,
                f"Signed release component mismatch: {name}; refresh-manifest cannot re-sign release/bootstrap; replace the complete enrollment directory")
        validate_elf(binary, name)
        total += len(binary)
    require(total <= 96 * 1024 * 1024, "Core payload exceeds size limit")
    require(b"C1RECOVERY-VERIFIER 1.1.0" in data["release/artifacts/c1updater"], "Recovery verifier capability marker missing")
    for name in SIGNED_SHELL:
        validate_shell_text(data[name.removeprefix("enrollment/")], name)
    return {"core_key_sha256": key_hash, "bootstrap_sha256": digest(data["bootstrap.v1"]),
            "release_sha256": digest(data["release/manifest.v1"]), "version": release[2][2:], "sequence": release[1][2:]}


def open_usb(original: bytes) -> bytes:
    require(digest(original) == ORIGINAL_USB_SHA256, "Factory S90usb SHA-256 mismatch")
    disabled = b"\t#/etc/init.d/usb/adb\t$1"
    require(original.count(disabled) == 1, "S90usb must contain exactly one disabled ADB startup line")
    return original.replace(disabled, b"\t/etc/init.d/usb/adb\t$1")


def public_defaults(project: Path) -> bytes:
    # Fail on source/default drift instead of silently switching endpoints or keys.
    source = read_file(project / "scripts/build-repository-profile.ps1").decode("utf-8-sig")
    for variable, expected in (("RepositoryUrl", APP_URL), ("CoreRepositoryUrl", CORE_URL)):
        match = re.search(r"\[string\]\$" + variable + r"\s*=\s*'([^']+)'", source)
        require(match is not None and match[1] == expected, f"Reviewed official HTTP default changed: {variable}")
    key = read_file(project / "config/app-repo/repository.ed25519.pub", 32)
    require(len(key) == 32, "Application repository public key must be 32 bytes")
    return key


def payload_files(ready: bool) -> set[str]:
    names = {"device-setup.sh"}
    for layer, members in LAYERS.items():
        names.update(layer + "/" + name for name in (*members, "SHA256SUMS"))
    if ready:
        names.update("enrollment/" + name for name in ENROLLMENT)
        names.update(ENROLLMENT_SUMS)
    else:
        names.add("NOT_READY.txt")
    return names


def sums(root: Path, names) -> bytes:
    return "".join(f"{digest(read_file(root / name))}  {name}\n" for name in sorted(names)).encode("ascii")


def manifest_updates(payload: Path, ready: bool) -> dict[Path, bytes]:
    updates = {payload / layer / "SHA256SUMS": sums(payload / layer, members) for layer, members in LAYERS.items()}
    if ready:
        for filename, (base, members) in ENROLLMENT_SUMS.items():
            updates[payload / filename] = sums(payload / base, members)
    # Hash the future child manifests without writing anything during validation.
    lines = []
    for name in sorted(payload_files(ready)):
        path = payload / name
        content = updates[path] if path in updates else read_file(path)
        lines.append(f"{digest(content)}  {name}\n")
    updates[payload / "SHA256SUMS"] = "".join(lines).encode("ascii")
    return updates


def validate_public_payload(payload: Path, project: Path) -> None:
    key = public_defaults(project)
    require(read_file(payload / "profile/repository.ed25519.pub") == key and
            read_file(payload / "developer/repository.ed25519.pub") == key,
            "Application repository trust key mismatch")
    require(read_file(payload / "profile/repository.url") == (APP_URL + "\n").encode() and
            read_file(payload / "profile/core-repository.url") == (CORE_URL + "\n").encode(),
            "Official repository URL mismatch")
    server = read_file(payload / "developer/server.url").decode("ascii").strip()
    require(server.rstrip("/") == PUBLISH_URL, "Publisher server.url must use the reviewed official HTTP origin")
    require(read_file(payload / "usb/S90usb.open") == open_usb(read_file(payload / "usb/S90usb.original")),
            "S90usb.open transformation mismatch")
    require(len(read_file(payload / "accessories/wallpaper.raw")) == 5624, "Wallpaper must be exactly 5624 bytes")
    for target, source in SHELL_SOURCES.items():
        actual = read_file(payload / target)
        validate_shell_text(actual, target)
        require(actual == normalize_shell_source(read_file(project / source), target),
                f"Device shell source mismatch after CRLF-to-LF normalization: {target}")
    for target in ("usb/S90usb.original", "usb/S90usb.open"):
        validate_shell_text(read_file(payload / target), target)
    # Bound total size and reject empty files before any manifest rewrite.
    names = payload_files((payload / "enrollment/bootstrap.v1").exists())
    total = sum(len(read_file(payload / name)) for name in names if not name.endswith("SHA256SUMS"))
    require(total <= MAX_PAYLOAD, "Payload exceeds 512 MiB limit")


def status_bytes(info: dict[str, str] | None) -> bytes:
    state = {"schema": 1, "status": "READY" if info else "NOT_READY", "enrollment": info,
             "note": "Offline package validation only; no device/server tests. Core trust comes from the maintainer-supplied enrollment, not its self-contained signature alone."}
    return (json.dumps(state, ensure_ascii=True, indent=2, sort_keys=True) + "\n").encode("ascii")


def finalize_updates(output: Path, updates: dict[Path, bytes], status: bytes) -> dict[Path, bytes]:
    updates[output / "BUNDLE-STATUS.json"] = status
    names = set(ROOT_REQUIRED)
    names.update("payload/" + name for name in payload_files(json.loads(status)["status"] == "READY"))
    names.add("payload/SHA256SUMS")
    names.update(name for name in ROOT_OPTIONAL if (output / name).exists())
    lines = []
    for name in sorted(names):
        path = output / name
        content = updates[path] if path in updates else read_file(path)
        lines.append(f"{digest(content)}  {name}\n")
    updates[output / "SHA256SUMS"] = "".join(lines).encode("ascii")
    return updates


def build_bundle(enrollment: Path | None, adb: Path, publisher: Path, output: Path,
                 *, project: Path = PROJECT, workspace: Path | None = None,
                 expected_key_sha256: str | None = None, installer_exe: Path | None = None,
                 go_license: Path | None = None, dotnet_notices: Path | None = None) -> bool:
    output = safe_path(output, missing=True)
    require(not output.exists(), "Output already exists; use a NEW output directory (no overwrite/rebuild in place)")
    workspace = workspace or project.parent
    info = validate_enrollment(enrollment, expected_key_sha256) if enrollment is not None else None
    key = public_defaults(project)
    # Only these explicitly named files are read. No recursive input copying.
    sources = {"device-setup.sh": project / "installer/device-setup.sh"}
    for name in LAYERS["tools"]:
        sources["tools/" + name] = adb / name
    for name in LAYERS["developer"]:
        sources["developer/" + name] = publisher / name
    for name in LAYERS["accessories"]:
        sources["accessories/" + name] = workspace / "Pic/wallpaper.raw" if name == "wallpaper.raw" else project / "third_party/neofetch" / name
    for name in ("device-repository-config.sh", "c1-update-check.sh"):
        sources["profile/" + name] = project / "scripts" / name
    sources["usb/S90usb.original"] = workspace / "firmware-analysis/system-rootfs/etc/init.d/S90usb"
    sources["usb/device-open-adb.sh"] = project / "scripts/device-open-adb.sh"
    if enrollment is not None:
        sources.update({"enrollment/" + name: enrollment / name for name in ENROLLMENT})
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".c1-bundle-", dir=output.parent) as temporary:
        stage = Path(temporary) / "bundle"
        payload = stage / "payload"
        for relative, source in sources.items():
            content = read_file(source)
            if relative in SHELL_SOURCES:
                content = normalize_shell_source(content, relative)
            write_new(payload / relative, content)
        write_new(payload / "profile/repository.ed25519.pub", key)
        write_new(payload / "profile/repository.url", (APP_URL + "\n").encode())
        write_new(payload / "profile/core-repository.url", (CORE_URL + "\n").encode())
        write_new(payload / "usb/S90usb.open", open_usb(read_file(payload / "usb/S90usb.original")))
        if info is None:
            write_new(payload / "NOT_READY.txt", b"NOT_READY: signed 13-file enrollment is missing. This is a template, NOT an installable package. Rebuild into a new directory with --enrollment-dir. Never invent signatures or production keys.\n")
        else:
            # Revalidate copied bytes to catch changed source inputs.
            require(validate_enrollment(payload / "enrollment", info["core_key_sha256"]) == info, "Enrollment changed during assembly")
        validate_public_payload(payload, project)
        root_sources = {
            "USER-GUIDE.md": project / "installer/USER-GUIDE.md",
            "THIRD-PARTY-NOTICES.txt": project / "installer/THIRD-PARTY-NOTICES.txt",
            "PLATFORM-TOOLS-NOTICE.txt": adb / "NOTICE.txt",
        }
        if go_license is not None:
            root_sources["GO-LICENSE.txt"] = go_license
        if dotnet_notices is not None:
            root_sources["DOTNET-THIRD-PARTY-NOTICES.txt"] = dotnet_notices
        for name, source in root_sources.items():
            write_new(stage / name, read_file(source))
        write_new(stage / "README.txt", (
            b"Read USER-GUIDE.md before use. Keep C1SlimInstaller.exe beside payload/. Check BUNDLE-STATUS.json before release.\n"
            b"NOT_READY templates cannot install or use refresh-manifest to become READY. Once complete signed enrollment is available, run build-bundle.py build with --enrollment-dir and --output-dir pointing to a NEW directory; do not modify the template in place.\n"
            b"For an existing READY bundle only: replace enrollment as one complete signed 13-file directory using the same approved core key, then run build-bundle.py refresh-manifest --output-dir THIS_DIRECTORY. The EXE need not be rebuilt for a core version change.\n"
            b"SHA256SUMS files are unsigned transport checks, not signing authority.\n"
        ))
        if installer_exe is not None:
            write_new(stage / "C1SlimInstaller.exe", read_file(installer_exe))
        updates = finalize_updates(stage, manifest_updates(payload, info is not None), status_bytes(info))
        for path, content in updates.items():
            write_new(path, content)
        safe_path(output, missing=True)
        require(not output.exists(), "Output appeared during assembly; refusing overwrite")
        stage.rename(output)
    return info is not None


def refresh_manifest(output: Path, *, project: Path = PROJECT) -> None:
    output = safe_path(output)
    status = json.loads(read_file(output / "BUNDLE-STATUS.json", 65536))
    require(status.get("schema") == 1 and status.get("status") == "READY" and isinstance(status.get("enrollment"), dict),
            "NOT_READY templates cannot be refreshed into installable bundles; rebuild in a new directory with complete signed enrollment")
    pin = status["enrollment"].get("core_key_sha256", "")
    require(re.fullmatch(r"[0-9a-f]{64}", pin) is not None, "Existing core trust fingerprint missing; refusing to establish a new trust root")
    files = ROOT_REQUIRED | {"payload/" + name for name in payload_files(True) if not name.endswith("SHA256SUMS")}
    optional = ROOT_OPTIONAL | {"SHA256SUMS", "payload/SHA256SUMS"} | {"payload/" + name for name in payload_files(True) if name.endswith("SHA256SUMS")}
    exact_tree(output, files, optional=optional)
    payload = output / "payload"
    info = validate_enrollment(payload / "enrollment", pin)
    validate_public_payload(payload, project)
    updates = finalize_updates(output, manifest_updates(payload, True), status_bytes(info))
    # All signature/hash/allowlist checks finish before any write. The top-level
    # manifest is replaced last; interruption remains detectable/fail-closed.
    for path, content in updates.items():
        safe_path(path, missing=True)
        fd, temporary = tempfile.mkstemp(prefix=".manifest-", dir=path.parent)
        try:
            with os.fdopen(fd, "wb") as stream:
                stream.write(content)
            os.replace(temporary, path)
        finally:
            if os.path.exists(temporary):
                os.unlink(temporary)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    build = commands.add_parser("build", help="assemble a new output directory; missing enrollment produces NOT_READY (exit 2)")
    build.add_argument("--enrollment-dir", type=Path, help="complete signed output from scripts/build-core-enrollment.ps1")
    build.add_argument("--adb-platform-tools-dir", "--adb-dir", dest="adb", required=True, type=Path)
    build.add_argument("--publisher-dir", required=True, type=Path)
    build.add_argument("--output-dir", required=True, type=Path)
    build.add_argument("--expected-core-key-sha256", help="optional independently reviewed public core-key fingerprint")
    build.add_argument("--installer-exe", type=Path, help="optional already-built GUI EXE; never executed")
    build.add_argument("--go-license", type=Path, help="optional explicit Go LICENSE source; no host discovery")
    build.add_argument("--dotnet-notices", type=Path, help="optional explicit .NET ThirdPartyNotices.txt source; no host discovery")
    refresh = commands.add_parser("refresh-manifest", help="verify signed enrollment and recompute unsigned outer manifests only")
    refresh.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        if args.command == "refresh-manifest":
            refresh_manifest(args.output_dir)
            print("READY: unsigned outer manifests refreshed; signed enrollment unchanged. No device/server tests performed.")
            return 0
        if args.expected_core_key_sha256 is not None:
            require(re.fullmatch(r"[0-9a-f]{64}", args.expected_core_key_sha256) is not None, "Expected core fingerprint must be lowercase SHA-256")
        ready = build_bundle(args.enrollment_dir, args.adb, args.publisher_dir, args.output_dir,
                             expected_key_sha256=args.expected_core_key_sha256, installer_exe=args.installer_exe,
                             go_license=args.go_license, dotnet_notices=args.dotnet_notices)
        print(("READY: offline package checks passed; no device/server tests. " if ready else
               "NOT_READY: template only; complete signed enrollment is missing. ") + str(args.output_dir))
        return 0 if ready else 2
    except (BundleError, OSError, UnicodeError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
