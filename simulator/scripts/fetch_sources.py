#!/usr/bin/env python3
import hashlib
import pathlib
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]
CACHE = ROOT / ".cache" / "sources"
LINUX = "6.6.60"
BUSYBOX = "1.37.0"
CURL = "8.11.0"
CURL_SHA256 = "db59cf0d671ca6e7f5c2c5ec177084a33a79e04c97e71cf183a5cdea235054eb"
FFMPEG = "6.1.5"
# Buildroot 2026.08 package/ffmpeg/ffmpeg.hash, verified against the release archive.
FFMPEG_SHA256 = "05fbe9db1f5452a3605bb1d9bf91da9d4bb08162891692e4f172b58c49109412"


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def download(url):
    CACHE.mkdir(parents=True, exist_ok=True)
    path = CACHE / url.rsplit("/", 1)[-1]
    if not path.exists():
        print("下载 " + path.name, flush=True)
        temporary = path.with_suffix(path.suffix + ".part")
        subprocess.run(["curl", "-fL", "--retry", "5", "--retry-all-errors", "--connect-timeout", "20", "--max-time", "600",
                        "--silent", "--show-error", "-o", str(temporary), url], check=True)
        temporary.replace(path)
    return path


def check(path, hashes):
    expected = next((line.split()[0] for line in hashes.read_text().splitlines()
                     if line.endswith(" " + path.name) or line.endswith("*" + path.name)), None)
    actual = sha256_file(path)
    if expected != actual:
        raise SystemExit("源码 SHA-256 校验失败：" + path.name)
    print("已校验 " + path.name, flush=True)


if __name__ == "__main__":
    base = "https://cdn.kernel.org/pub/linux/kernel/v6.x/"
    check(download(base + "linux-" + LINUX + ".tar.xz"), download(base + "sha256sums.asc"))
    base = "https://busybox.net/downloads/"
    check(download(base + "busybox-" + BUSYBOX + ".tar.bz2"), download(base + "busybox-" + BUSYBOX + ".tar.bz2.sha256"))
    path = download("https://curl.se/download/curl-" + CURL + ".tar.xz")
    if sha256_file(path) != CURL_SHA256:
        raise SystemExit("curl 源码 SHA-256 校验失败")
    print("已校验 " + path.name, flush=True)
    path = download("https://ffmpeg.org/releases/ffmpeg-" + FFMPEG + ".tar.xz")
    if sha256_file(path) != FFMPEG_SHA256:
        raise SystemExit("FFmpeg 源码 SHA-256 校验失败")
    print("已校验 " + path.name, flush=True)
