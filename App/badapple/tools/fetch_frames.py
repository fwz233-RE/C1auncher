#!/usr/bin/env python3
"""Fetch the public frame archive for local experiments, never application source.

The upstream has no explicit media license. This helper does not confer rights
and is not used by the build script; redistributors must obtain permission.
"""
import argparse
import hashlib
import os
from pathlib import Path
import tempfile
import urllib.request
import zipfile

URL = "https://raw.githubusercontent.com/Felixoofed/badapple-frames/main/frames.zip"
LIMIT = 32 * 1024 * 1024


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output
    if output.exists():
        parser.error("output already exists; choose a new path")
    output.parent.mkdir(parents=True, exist_ok=True)
    tmp = None
    try:
        digest = hashlib.sha256()
        size = 0
        request = urllib.request.Request(
            "https://api.github.com/repos/Felixoofed/badapple-frames/contents/frames.zip",
            headers={"User-Agent": "c1-badapple", "Accept": "application/vnd.github.raw+json"},
        )
        with urllib.request.urlopen(request, timeout=180) as response:
            with tempfile.NamedTemporaryFile(dir=output.parent, delete=False) as destination:
                tmp = Path(destination.name)
                while chunk := response.read(64 * 1024):
                    size += len(chunk)
                    if size > LIMIT:
                        raise ValueError("archive exceeds 32 MiB")
                    destination.write(chunk)
                    digest.update(chunk)
        if size == 0 or not zipfile.is_zipfile(tmp):
            raise ValueError("download is not a complete ZIP archive")
        os.rename(tmp, output)
        print(f"Source: {URL}\nBytes: {size}\nSHA256: {digest.hexdigest()}")
        print("Upstream media license is unspecified. Local download is not redistribution permission.")
    finally:
        if tmp is not None:
            tmp.unlink(missing_ok=True)


if __name__ == "__main__":
    main()
