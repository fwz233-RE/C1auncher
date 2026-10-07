#!/usr/bin/env python3
"""下载上游官方发行包；设备二进制保持原样。"""
import hashlib
import json
import pathlib
import subprocess
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
CACHE = ROOT / ".cache" / "release-v2.0.0"
BASE = "https://github.com/fwz233-RE/C1auncher/releases/download/v2.0.0/"
INSTALLER = "C1SlimInstaller-2.0.0-lf-fix-20260907.zip"
APPS = ["C1Slim-App-hello-1.5.1.zip", "C1Slim-App-book-reader-0.1.20.zip",
        "C1Slim-App-pic-0.1.4.zip", "C1Slim-App-chichugames-0.1.3.zip",
        "C1Slim-App-music-player-0.3.4.zip", "C1Slim-App-pinao-0.1.4.zip"]


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def download(name):
    CACHE.mkdir(parents=True, exist_ok=True)
    path = CACHE / name
    if not path.exists():
        print("下载 " + name, flush=True)
        subprocess.run(["curl", "-fL", "--retry", "5", "--retry-all-errors", "--connect-timeout", "20",
                        "--max-time", "300", "--silent", "--show-error", "-o", str(path.with_suffix(path.suffix + ".part")), BASE + name], check=True)
        path.with_suffix(path.suffix + ".part").replace(path)
    return path


def main():
    checksums = {}
    for name in ["APPLICATIONS-SHA256SUMS", INSTALLER + ".sha256"]:
        for line in download(name).read_text().splitlines():
            parts = line.split()
            if len(parts) >= 2 and len(parts[0]) == 64:
                checksums[parts[-1].lstrip("*")] = parts[0].lower()
    download("APPLICATIONS-MANIFEST.json")
    manifest = []
    for name in [INSTALLER] + APPS:
        path = download(name)
        digest = sha256_file(path)
        if checksums.get(name) != digest:
            raise SystemExit("发行包 SHA-256 校验失败：" + name)
        with zipfile.ZipFile(path) as archive:
            entries = archive.namelist()
        manifest.append({"name": name, "sha256": digest, "url": BASE + name, "entries": entries})
        print("已校验 " + name, flush=True)
    (CACHE / "verified.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
    print("发行包清单：" + str(CACHE / "verified.json"))


if __name__ == "__main__":
    main()
