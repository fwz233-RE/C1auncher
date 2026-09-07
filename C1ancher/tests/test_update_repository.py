"""Exercise the real curl downloader against a local read-only HTTP fixture.
The persistent-busy test intentionally exercises the production 120s retry budget.
"""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import hashlib
import os
import subprocess
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]


BUILD = Path(os.environ.get("C1_UPDATE_TEST_BUILD_DIR", "build/lifecycle-fixes"))
if not BUILD.is_absolute():
    BUILD = ROOT / BUILD


class RepositoryDownloadTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(["make", f"BUILD_DIR={BUILD}", "host-update-tests"], cwd=ROOT, check=True)
        cls.driver = BUILD / "host-update-repository-driver"
        sources = ["update/repository", "update/io", "update/protocol", "update/transaction", "update/state",
                   "security/secure_file", "security/sha256", "security/trusted_ed25519"]
        objects = [str(BUILD / f"host/src/{source}.o") for source in sources]
        objects += [str(BUILD / f"host/third_party/ed25519/{name}.o")
                    for name in ["fe", "ge", "sc", "sha512", "verify"]]
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Isrc",
                        "tests/update_repository_driver.c", *objects,
                        "-Wl,--wrap=execv,--wrap=setrlimit",
                        "-o", str(cls.driver)], cwd=ROOT, check=True)

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="c1-core-http-")
        self.output = Path(self.directory.name) / "artifact"
        self.statuses = [200]
        self.paths = []
        self.payload = b"abc"
        self.requests = []
        self.routes = {}
        self.log = Path(self.directory.name) / "curl.log"
        self.env = dict(os.environ, C1_TEST_CURL_LOG=str(self.log))
        test = self

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                host = self.headers.get("Host")
                test.requests.append((host, self.path))
                test.paths.append(self.path)
                status = test.statuses.pop(0) if len(test.statuses) > 1 else test.statuses[0]
                body = test.payload if status == 200 else b"server busy/error"
                status, body, location, delay = test.routes.get((host, self.path),
                                                               (status, body, None, 0))
                if delay:
                    time.sleep(delay)
                self.send_response(status)
                self.send_header("Content-Length", str(len(body)))
                if location is not None:
                    self.send_header("Location", location)
                if status in (429, 503):
                    self.send_header("Retry-After", "5")
                self.end_headers()
                try:
                    self.wfile.write(body)
                except (BrokenPipeError, ConnectionResetError):
                    pass

            def log_message(self, *args):
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.url = f"http://127.0.0.1:{self.server.server_port}/c1/core/v1/canary"
        self.env["C1_TEST_HTTP_PORT"] = str(self.server.server_port)

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
        self.directory.cleanup()

    def fetch(self, size=3, timeout=135):
        return subprocess.run([str(self.driver), self.url, str(self.output), str(size)],
                              capture_output=True, text=True, timeout=timeout, env=self.env)

    def test_429_and_503_retry_then_exact_payload(self):
        self.statuses = [429, 503, 200]
        started = time.monotonic()
        result = self.fetch()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertGreaterEqual(time.monotonic() - started, 10)
        self.assertEqual(self.output.read_bytes(), b"abc")
        self.assertEqual(self.paths, ["/c1/core/v1/canary/artifacts/C1ancher"] * 3)

    def test_503_with_small_error_body_retries(self):
        self.statuses = [503, 200]
        self.payload = b"a" * 64
        self.url = self.url.replace("/canary", "/stable")
        result = self.fetch(size=64)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.output.read_bytes(), self.payload)
        self.assertEqual(self.paths, ["/c1/core/v1/stable/artifacts/C1ancher"] * 2)

    def test_404_is_not_retried(self):
        self.statuses = [404]
        result = self.fetch()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(len(self.paths), 1)
        self.assertFalse(self.output.exists())

    def test_existing_destination_is_preserved(self):
        self.output.write_bytes(b"old")
        self.assertNotEqual(self.fetch().returncode, 0)
        self.assertEqual(self.output.read_bytes(), b"old")
        self.assertEqual(self.paths, [])

    def test_oversized_payload_is_removed(self):
        self.payload = b"too large"
        self.assertNotEqual(self.fetch().returncode, 0)
        self.assertFalse(self.output.exists())
        self.assertEqual(len(self.paths), 1)

    def test_persistent_busy_stops_at_retry_window(self):
        self.statuses = [503]
        started = time.monotonic()
        result = self.fetch()
        elapsed = time.monotonic() - started
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("retry window exhausted", result.stderr)
        self.assertGreaterEqual(elapsed, 110)
        self.assertLess(elapsed, 125)
        self.assertLessEqual(len(self.paths), 25)
        self.assertFalse(self.output.exists())


    def use_primary(self):
        self.url = "http://www.fwz233.com/c1/core/v1/stable"

    def route(self, host, name, status=200, body=b"", location=None, delay=0):
        self.routes[(host, f"/c1/core/v1/stable/{name}")] = (status, body, location, delay)

    def sign(self, manifest):
        root = Path(self.directory.name)
        payload = root / "signed-manifest"
        payload.write_bytes(manifest)
        return subprocess.run(["openssl", "pkeyutl", "-sign", "-rawin", "-inkey", str(root / "private.pem"),
                               "-in", str(payload)], check=True, capture_output=True).stdout

    def signed_fixture(self, sequence=42, epoch=7, minimum_bootstrap="1.0.0", minimum_updater="1.0.0"):
        root = Path(self.directory.name)
        private = root / "private.pem"
        if not private.exists():
            subprocess.run(["openssl", "genpkey", "-algorithm", "ED25519", "-out", str(private)],
                           check=True, capture_output=True)
        public = subprocess.run(["openssl", "pkey", "-in", str(private), "-pubout", "-outform", "DER"],
                                check=True, capture_output=True).stdout
        self.key = root / "key"
        self.key.write_bytes(public[-32:])
        self.key.chmod(0o600)
        self.artifacts = {"C1ancher": b"abc", "c1pkg": b"abcd",
                          "C1ancher-launcher": b"abcde", "c1updater": b"abcdef"}
        manifest = (f"C1CORE-MANIFEST 1\nS\t{sequence}\nV\t1.2.3\nE\t{epoch}\n"
                    "T\tmips32r2-little-o32-hard-float-double-static\n"
                    f"B\t{minimum_bootstrap}\nU\t{minimum_updater}\nC\tc1-core-v1\nR\tabcdef012345\nD\t1700000000\n")
        for role, (name, data) in zip(("c1ancher", "c1pkg", "launcher", "updater"), self.artifacts.items()):
            manifest += f"F\t{role}\tartifacts/{name}\t{hashlib.sha256(data).hexdigest()}\t{len(data)}\t700\n"
        self.manifest = manifest.encode()
        self.signature = self.sign(self.manifest)
        for host in ("www.fwz233.com", "123.56.214.77"):
            self.route(host, "manifest.v1", body=self.manifest)
            self.route(host, "manifest.v1.sig", body=self.signature)
            for name, data in self.artifacts.items():
                self.route(host, f"artifacts/{name}", body=data)

    def release(self, timeout=100):
        stage = Path(self.directory.name) / "metadata"
        stage.mkdir(exist_ok=True, mode=0o700)
        return subprocess.run([str(self.driver), "release", self.url, str(stage), str(self.key)],
                              capture_output=True, text=True, env=self.env, timeout=timeout)

    def prepare(self):
        roots = [Path(self.directory.name) / name for name in ("staging", "core", "state")]
        for root in roots:
            root.mkdir(exist_ok=True, mode=0o700)
        return subprocess.run([str(self.driver), "prepare", self.url, *map(str, roots), str(self.key)],
                              capture_output=True, text=True, env=self.env, timeout=100)

    def test_primary_success_never_contacts_fallback(self):
        self.use_primary()
        result = self.fetch()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([host for host, _ in self.requests], ["www.fwz233.com"])
        args = self.log.read_text()
        self.assertIn("--proto\t=http\t--proto-redir\t=http\t", args)
        self.assertIn("--max-time\t600\t", args)

    def test_primary_dns_and_connect_failure_use_fixed_ip(self):
        self.use_primary()
        for failure in ("dns", "connect"):
            with self.subTest(failure=failure):
                self.env["C1_TEST_PRIMARY_FAILURE"] = failure
                result = self.fetch()
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(self.output.read_bytes(), b"abc")
                self.output.unlink()
        self.assertEqual([host for host, _ in self.requests], ["123.56.214.77"] * 2)
        self.assertEqual(len(self.log.read_text().splitlines()), 4)

    def test_primary_403_and_https_308_fall_back_over_http(self):
        self.use_primary()
        for status in (403, 308):
            with self.subTest(status=status):
                self.route("www.fwz233.com", "artifacts/C1ancher", status=status,
                           location="https://www.fwz233.com/c1/core/v1/stable/artifacts/C1ancher")
                result = self.fetch()
                self.assertEqual(result.returncode, 0, result.stderr)
                self.output.unlink()
        self.assertEqual([host for host, _ in self.requests],
                         ["www.fwz233.com", "123.56.214.77"] * 2)

    def test_custom_http_repository_rejects_https_redirect(self):
        host = f"127.0.0.1:{self.server.server_port}"
        self.routes[(host, "/c1/core/v1/canary/artifacts/C1ancher")] = (
            308, b"", f"https://127.0.0.1:{self.server.server_port}/forbidden", 0)
        result = self.fetch()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(len(self.requests), 1)
        self.assertFalse(self.output.exists())

    def test_fallback_is_bounded_and_preserves_existing_destination(self):
        self.use_primary()
        self.statuses = [403]
        self.assertNotEqual(self.fetch().returncode, 0)
        self.assertEqual([host for host, _ in self.requests], ["www.fwz233.com", "123.56.214.77"])
        self.assertFalse(self.output.exists())
        self.output.write_bytes(b"old")
        self.assertNotEqual(self.fetch().returncode, 0)
        self.assertEqual(self.output.read_bytes(), b"old")
        self.assertEqual(len(self.requests), 2)

    def test_cancelled_curl_never_falls_back(self):
        self.use_primary()
        self.env["C1_TEST_PRIMARY_FAILURE"] = "cancel"
        self.assertNotEqual(self.fetch().returncode, 0)
        self.assertEqual(self.requests, [])
        self.assertEqual(len(self.log.read_text().splitlines()), 1)
        self.assertFalse(self.output.exists())

    def test_valid_signed_primary_release_never_falls_back(self):
        self.use_primary()
        self.signed_fixture()
        result = self.release()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([host for host, _ in self.requests], ["www.fwz233.com"] * 2)
        for line in self.log.read_text().splitlines():
            args = line.split("\t")
            self.assertLessEqual(int(args[args.index("--max-time") + 1]), 45)

    def test_invalid_primary_signature_refetches_entire_pair(self):
        self.use_primary()
        self.signed_fixture()
        self.route("www.fwz233.com", "manifest.v1.sig", body=b"x" * 64)
        result = self.release()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.requests, [(host, f"/c1/core/v1/stable/{name}")
                                        for host in ("www.fwz233.com", "123.56.214.77")
                                        for name in ("manifest.v1", "manifest.v1.sig")])

    def test_correctly_signed_but_invalid_primary_manifest_falls_back(self):
        self.use_primary()
        self.signed_fixture()
        invalid = self.manifest.replace(b"C1CORE-MANIFEST 1", b"C1CORE-MANIFEST 9")
        self.route("www.fwz233.com", "manifest.v1", body=invalid)
        self.route("www.fwz233.com", "manifest.v1.sig", body=self.sign(invalid))
        result = self.release()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([host for host, _ in self.requests],
                         ["www.fwz233.com"] * 2 + ["123.56.214.77"] * 2)

    def test_primary_signature_http_failure_restarts_complete_pair(self):
        self.use_primary()
        self.signed_fixture()
        self.route("www.fwz233.com", "manifest.v1.sig", status=403)
        result = self.release()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([host for host, _ in self.requests],
                         ["www.fwz233.com"] * 2 + ["123.56.214.77"] * 2)

    def test_invalid_both_signatures_fail_closed_and_clean_metadata(self):
        self.use_primary()
        self.signed_fixture()
        for host in ("www.fwz233.com", "123.56.214.77"):
            self.route(host, "manifest.v1.sig", body=b"x" * 64)
        self.assertNotEqual(self.release().returncode, 0)
        self.assertEqual(len(self.requests), 4)
        self.assertEqual(list((Path(self.directory.name) / "metadata").iterdir()), [])

    def test_primary_manifest_deadline_allows_fallback(self):
        self.use_primary()
        self.signed_fixture()
        self.route("www.fwz233.com", "manifest.v1", body=self.manifest, delay=48)
        started = time.monotonic()
        result = self.release(timeout=52)
        elapsed = time.monotonic() - started
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertGreaterEqual(elapsed, 44)
        self.assertLess(elapsed, 48)
        self.assertEqual([host for host, _ in self.requests],
                         ["www.fwz233.com", "123.56.214.77", "123.56.214.77"])

    def test_metadata_destination_is_preserved(self):
        self.use_primary()
        self.signed_fixture()
        stage = Path(self.directory.name) / "metadata"
        stage.mkdir(mode=0o700)
        (stage / "manifest.v1.sig").write_bytes(b"old")
        self.assertNotEqual(self.release().returncode, 0)
        self.assertEqual((stage / "manifest.v1.sig").read_bytes(), b"old")
        self.assertEqual(self.requests, [])

    def test_fallback_components_prepare_with_exact_hashes(self):
        self.use_primary()
        self.signed_fixture()
        self.env["C1_TEST_PRIMARY_FAILURE"] = "dns"
        result = self.prepare()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([host for host, _ in self.requests], ["123.56.214.77"] * 6)

    def test_fallback_components_wrong_size_or_hash_fail(self):
        self.use_primary()
        self.signed_fixture()
        self.env["C1_TEST_PRIMARY_FAILURE"] = "dns"
        for bad in (b"xy", b"xyz", b"toolarge"):
            with self.subTest(payload=bad):
                self.route("123.56.214.77", "artifacts/C1ancher", body=bad)
                result = self.prepare()
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(list((Path(self.directory.name) / "staging").iterdir()), [])

    def test_incompatible_release_rejected_before_payload_transfer(self):
        self.use_primary()
        for minimum in ({"minimum_bootstrap": "9.0.0"}, {"minimum_updater": "9.0.0"}):
            with self.subTest(minimum=minimum):
                self.requests.clear()
                self.signed_fixture(**minimum)
                result = self.prepare()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("trusted", result.stderr)
                self.assertEqual(len(self.requests), 2)
                self.assertFalse(any("/artifacts/" in path for _, path in self.requests))

    def test_fallback_release_cannot_roll_sequence_back(self):
        self.use_primary()
        self.signed_fixture(sequence=42)
        self.env["C1_TEST_PRIMARY_FAILURE"] = "dns"
        result = self.prepare()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.signed_fixture(sequence=41)
        result = self.prepare()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("release rollback rejected", result.stderr)
        self.signed_fixture(sequence=43, epoch=6)
        result = self.prepare()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("release rollback rejected", result.stderr)


if __name__ == "__main__":
    unittest.main()
