#!/usr/bin/env python3
"""Exercise real Rime through the desktop queue; temporary host processes only."""
import argparse
import pathlib
import subprocess
import tempfile
import time


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--service", required=True)
    p.add_argument("--client", required=True)
    p.add_argument("--shared-data", required=True)
    p.add_argument("--frontend", choices=("desktop", "package"), default="desktop",
                   help="Require the chosen production-frontend harness to confirm a real Chinese commit")
    args = p.parse_args()
    expected = {"desktop": "real Rime -> desktop asynchronous queue -> UTF-8 lock text passed",
                "package": "PASS: real Rime -> package GUI Chinese-name search"}[args.frontend]
    with tempfile.TemporaryDirectory(prefix="c1-desktop-ime-") as root:
        root = pathlib.Path(root)
        runtime = root / "run"
        runtime.mkdir(mode=0o700)
        with (root / "service.log").open("w+") as log:
            service = subprocess.Popen([args.service, "--socket", str(runtime / "socket"),
                "--shared-data", args.shared_data, "--user-data", str(root / "user")],
                stdout=log, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + 120
                while not (runtime / "socket").exists():
                    if service.poll() is not None or time.monotonic() >= deadline:
                        log.seek(0)
                        raise RuntimeError("service startup failed: " + log.read())
                    time.sleep(0.05)
                result = subprocess.run([args.client, str(runtime / "socket")], check=True,
                                        timeout=150, capture_output=True, text=True)
                if expected not in result.stdout:
                    raise RuntimeError("Wrong client or missing real-frontend commit confirmation: " + result.stdout[-1000:])
                print(result.stdout, end="")
            finally:
                if service.poll() is None:
                    service.terminate()
                    try:
                        service.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        service.kill()
                        service.wait(timeout=5)
            if service.returncode != 0:
                log.seek(0)
                raise RuntimeError("service did not shut down cleanly: " + log.read()[-2000:])
    print(f"{args.frontend} real-Rime integration passed (no device access)")


if __name__ == "__main__":
    main()
