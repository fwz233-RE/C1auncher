"""Check-only updater regression: real curl, loopback HTTP and throwaway Ed25519.

Run on Linux/WSL: C1_UPDATE_TEST_BUILD_DIR=build/update-check python3 tests/test_update_check.py
Production deadlines are retained; one test accelerates only its linked clock.
No production paths, signing keys, devices, or external servers are written.
"""
from concurrent.futures import ThreadPoolExecutor
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import fcntl
import hashlib
import os
import signal
import stat
import subprocess
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get("C1_UPDATE_TEST_BUILD_DIR", "build/update-check"))
if not BUILD.is_absolute():
    BUILD = ROOT / BUILD


class UpdateCheckTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.driver = BUILD / "host-update-check-driver"
        sources = ["update/protocol", "update/state", "update/io", "update/log", "update/repository",
                   "update/transaction", "update/boot", "update/slot", "update/supervise",
                   "update/supervise_policy", "launcher/policy", "platform/update_request", "platform/shutdown",
                   "security/secure_file", "security/sha256", "security/trusted_ed25519"]
        objects = [str(BUILD / f"host/src/{source}.o") for source in sources]
        objects += [str(BUILD / f"host/third_party/ed25519/{name}.o")
                    for name in ("fe", "ge", "sc", "sha512", "verify")]
        # Object targets keep this regression runnable before the parent adds
        # check.c to UPDATE_SOURCES and HOST_UPDATE_TEST_SOURCES in Makefile.
        subprocess.run(["make", f"BUILD_DIR={BUILD}", *objects], cwd=ROOT, check=True)
        subprocess.run(["cc", "-D_POSIX_C_SOURCE=200809L", "-std=c11", "-Wall", "-Wextra",
                        "-Wpedantic", "-Werror", "-Isrc", "tests/update_check_driver.c", "src/update/check.c",
                        *objects, "-Wl,--wrap=execv,--wrap=setrlimit,--wrap=clock_gettime",
                        "-o", str(cls.driver)], cwd=ROOT, check=True)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="c1-check-http-")
        self.root = Path(self.temp.name)
        self.staging = self.root / "staging"
        self.state = self.root / "state"
        self.core = self.root / "core"
        for directory in (self.staging, self.state, self.core):
            directory.mkdir(mode=0o700)
        # These sibling files simulate an active transaction/prepared package.
        (self.staging / ".txn.keep").mkdir(mode=0o700)
        (self.staging / ".txn.keep" / "payload").write_bytes(b"partial download to preserve")
        (self.core / "prepared").mkdir(mode=0o700)
        (self.core / "prepared" / "C1ancher").write_bytes(b"prepared binary to preserve")
        (self.core / "current").symlink_to("prepared")
        self.paths = []
        self.observed_directories = set()
        self.delay = 0
        self.http_status = 200
        self.signature_status = 200
        self.on_manifest = None
        test = self

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                test.paths.append((self.headers.get("Host"), self.path))
                # Observe directory privacy while the real downloader is active.
                for path in test.staging.glob(".check.*"):
                    test.observed_directories.add((str(path), stat.S_IMODE(path.stat().st_mode)))
                is_manifest = self.path.endswith("/manifest.v1")
                is_signature = self.path.endswith("/manifest.v1.sig")
                status = test.http_status if is_manifest else test.signature_status if is_signature else 404
                body = test.manifest if is_manifest else test.signature if is_signature else b"forbidden"
                if is_manifest and test.on_manifest is not None:
                    test.on_manifest()
                if test.delay:
                    time.sleep(test.delay)
                self.send_response(status)
                self.send_header("Content-Length", str(len(body)))
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
        self.profile = self.root / "repository.url"
        self.profile.write_text(f"http://127.0.0.1:{self.server.server_port}/core\n")
        self.profile.chmod(0o600)
        self.private = self.root / "private.pem"
        subprocess.run(["openssl", "genpkey", "-algorithm", "ED25519", "-out", str(self.private)],
                       check=True, capture_output=True)
        public = subprocess.run(["openssl", "pkey", "-in", str(self.private), "-pubout", "-outform", "DER"],
                                check=True, capture_output=True).stdout
        self.key = self.root / "key"
        self.key.write_bytes(public[-32:])
        self.key.chmod(0o600)
        self.env = dict(os.environ, C1_TEST_HTTP_PORT=str(self.server.server_port),
                        C1_TEST_CURL_LOG=str(self.root / "curl.log"), C1_TEST_STAGING_ROOT=str(self.staging),
                        C1_TEST_STATE_ROOT=str(self.state), C1_TEST_CORE_ROOT=str(self.core),
                        C1_TEST_KEY=str(self.key), C1_TEST_REPOSITORY_CONFIG=str(self.profile))
        self.fixture()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
        self.temp.cleanup()

    def sign(self, data):
        payload = self.root / "signed-data"
        payload.write_bytes(data)
        return subprocess.run(["openssl", "pkeyutl", "-sign", "-rawin", "-inkey", str(self.private),
                               "-in", str(payload)], check=True, capture_output=True).stdout

    def fixture(self, sequence=42, epoch=7, version="1.2.3", bootstrap="1.0.0", updater="1.0.0",
                compatibility="c1-core-v1", target="mips32r2-little-o32-hard-float-double-static"):
        manifest = (f"C1CORE-MANIFEST 1\nS\t{sequence}\nV\t{version}\nE\t{epoch}\nT\t{target}\n"
                    f"B\t{bootstrap}\nU\t{updater}\nC\t{compatibility}\nR\tabcdef012345\nD\t1700000000\n")
        for role, name in (("c1ancher", "C1ancher"), ("c1pkg", "c1pkg"),
                           ("launcher", "C1ancher-launcher"), ("updater", "c1updater")):
            manifest += f"F\t{role}\tartifacts/{name}\t{hashlib.sha256(b'abc').hexdigest()}\t3\t700\n"
        self.manifest = manifest.encode()
        self.signature = self.sign(self.manifest)

    def committed_state(self, sequence=41, epoch=7, release="1.2.3", phase="confirmed", generation=1,
                        digest=None):
        directory = self.state / "generations" / str(generation)
        directory.parent.mkdir(mode=0o700, exist_ok=True)
        directory.mkdir(mode=0o700)
        data = (f"C1CORE-STATE 1\nP\t{phase}\nS\t{sequence}\nE\t{epoch}\nR\t{release}\n"
                f"D\t{digest or hashlib.sha256(self.manifest).hexdigest()}\n")
        record = directory / "state.v1"
        record.write_text(data)
        record.chmod(0o600)
        temporary = self.state / "current.new"
        temporary.symlink_to(f"generations/{generation}")
        temporary.replace(self.state / "current")

    @staticmethod
    def snapshot(root):
        result = {}
        if not root.exists():
            return result
        for path in [root, *root.rglob("*")]:
            metadata = path.lstat()
            content = os.readlink(path) if path.is_symlink() else path.read_bytes() if path.is_file() else None
            # Ignore atime, which normal reads may update. Detect rewrites, chmod,
            # replacements and state-generation/lock changes, including root dir.
            result[str(path.relative_to(root))] = (metadata.st_mode, metadata.st_ino, metadata.st_mtime_ns,
                                                   metadata.st_ctime_ns, content)
        return result

    def run_check(self, *extra, timeout=10):
        state_before, core_before = self.snapshot(self.state), self.snapshot(self.core)
        sibling_before = self.snapshot(self.staging / ".txn.keep")
        result = subprocess.run([str(self.driver), "check-configured", *extra], env=self.env,
                                capture_output=True, text=True, timeout=timeout)
        self.assertEqual(self.snapshot(self.state), state_before)
        self.assertEqual(self.snapshot(self.core), core_before)
        self.assertEqual(self.snapshot(self.staging / ".txn.keep"), sibling_before)
        self.assertEqual(sorted(path.name for path in self.staging.iterdir()), [".txn.keep"])
        self.assertFalse(any("/artifacts/" in path for _, path in self.paths))
        self.assertTrue(all(mode == 0o700 for _, mode in self.observed_directories))
        return result

    def assert_check(self, result, sequence=42, version="1.2.3", available=True):
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, f"C1UPDATE-CHECK 1\nS\t{sequence}\nV\t{version}\nA\t{int(available)}\n")

    def assert_error(self, result):
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "")
        self.assertTrue(result.stderr)

    def test_same_version_higher_sequence_is_available_and_metadata_only(self):
        self.committed_state()
        self.assert_check(self.run_check())
        self.assertEqual([path for _, path in self.paths], ["/core/manifest.v1", "/core/manifest.v1.sig"])
        self.assertEqual(len(self.observed_directories), 1)
        self.assertEqual(len((self.root / "curl.log").read_text().splitlines()), 2)

    def test_confirmed_exact_identity_is_not_available(self):
        self.committed_state(sequence=42)
        self.assert_check(self.run_check(), available=False)

    def test_prepared_exact_identity_is_not_available_and_retained(self):
        self.committed_state(sequence=42, phase="prepared")
        self.assert_check(self.run_check(), available=False)

    def test_sequence_not_version_orders_releases(self):
        self.committed_state(release="9.9.9")
        self.assert_check(self.run_check())

    def test_absent_state_is_read_only(self):
        self.state.rmdir()
        self.assert_check(self.run_check())
        self.assertFalse(self.state.exists())

    def test_invalid_signature_is_silent_and_cleans_up(self):
        self.signature = b"x" * 64
        self.assert_error(self.run_check())

    def test_malformed_signed_manifest_is_silent_and_cleans_up(self):
        self.manifest = self.manifest.replace(b"V\t1.2.3\n", b"V\t1.2.3\nA\t1\n")
        self.signature = self.sign(self.manifest)
        self.assert_error(self.run_check())

    def test_wrong_key_and_missing_key_fail_closed(self):
        self.key.write_bytes(b"x" * 32)
        self.assert_error(self.run_check())
        self.key.unlink()
        self.assert_error(self.run_check())

    def test_sequence_rollback_is_rejected(self):
        self.committed_state(sequence=43)
        self.assert_error(self.run_check())

    def test_epoch_rollback_is_rejected_despite_newer_sequence(self):
        self.committed_state(epoch=8)
        self.assert_error(self.run_check())

    def test_equal_sequence_conflicting_identity_is_rejected(self):
        self.committed_state(sequence=42, digest="f" * 64)
        self.assert_error(self.run_check())

    def test_minimum_bootstrap_updater_target_and_contract_are_checked(self):
        for fields in ({"bootstrap": "9.0.0"}, {"updater": "9.0.0"},
                       {"target": "wrong-target"}, {"compatibility": "c1-core-v2"}):
            with self.subTest(fields=fields):
                self.fixture(**fields)
                self.assert_error(self.run_check())

    def test_unsigned_http_error_preserves_prepared(self):
        self.committed_state(sequence=42, phase="prepared")
        self.http_status = 404
        self.assert_error(self.run_check())
        self.assertEqual([path for _, path in self.paths], ["/core/manifest.v1"])

    def test_offline_check_retains_prepared_state_and_package(self):
        self.committed_state(sequence=42, phase="prepared")
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
        self.assert_error(self.run_check())
        self.assertEqual(self.paths, [])

    def test_full_u64_sequence_is_reported_without_truncation(self):
        self.fixture(sequence=2**64 - 1)
        self.committed_state(sequence=2**64 - 2)
        self.assert_check(self.run_check(), sequence=2**64 - 1)

    def test_missing_signature_preserves_prepared_and_cleans_manifest(self):
        self.committed_state(sequence=42, phase="prepared")
        self.signature_status = 404
        self.assert_error(self.run_check())

    def test_invalid_config_and_extra_arguments_never_contact_http(self):
        self.profile.write_text("file:///tmp/untrusted\n")
        self.assert_error(self.run_check())
        self.assert_error(self.run_check("ignored"))
        self.assertEqual(self.paths, [])

    def test_unsafe_config_and_state_never_contact_http(self):
        self.profile.chmod(0o666)
        self.assert_error(self.run_check())
        self.profile.chmod(0o600)
        (self.state / "current").write_bytes(b"not a state symlink")
        self.assert_error(self.run_check())
        self.assertEqual(self.paths, [])

    def test_symlink_staging_is_rejected_without_deleting_target(self):
        link = self.root / "staging-link"
        link.symlink_to(self.staging, target_is_directory=True)
        self.env["C1_TEST_STAGING_ROOT"] = str(link)
        self.assert_error(self.run_check())
        self.assertTrue(link.is_symlink())
        self.assertEqual(self.paths, [])

    def test_held_transaction_lock_does_not_block_or_change_it(self):
        self.committed_state(phase="prepared")
        with (self.state / ".c1updater.lock").open("w") as stream:
            stream.write("existing transaction lock")
            stream.flush()
            fcntl.lockf(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.assert_check(self.run_check())

    def test_two_checks_own_disjoint_private_directories(self):
        self.committed_state()
        self.delay = 0.15
        before = self.snapshot(self.state)
        with ThreadPoolExecutor(max_workers=2) as executor:
            futures = [executor.submit(subprocess.run, [str(self.driver), "check-configured"],
                                       env=self.env, capture_output=True, text=True, timeout=10) for _ in range(2)]
            for future in futures:
                self.assert_check(future.result())
        self.assertEqual(self.snapshot(self.state), before)
        self.assertEqual(len(self.observed_directories), 2)
        self.assertTrue(all(mode == 0o700 for _, mode in self.observed_directories))
        self.assertEqual(sorted(path.name for path in self.staging.iterdir()), [".txn.keep"])
        self.assertFalse(any("/artifacts/" in path for _, path in self.paths))

    def test_concurrent_new_state_is_reloaded_before_reporting(self):
        self.committed_state()
        def advance():
            self.committed_state(sequence=42, phase="prepared", generation=2)
        self.on_manifest = advance
        result = subprocess.run([str(self.driver), "check-configured"], env=self.env,
                                capture_output=True, text=True, timeout=10)
        self.assert_check(result, available=False)
        self.assertEqual(os.readlink(self.state / "current"), "generations/2")
        self.assertEqual(sorted(path.name for path in self.staging.iterdir()), [".txn.keep"])

    def test_primary_fallback_remains_metadata_only(self):
        self.profile.write_text("http://www.fwz233.com/c1/core/v1/stable\n")
        self.env["C1_TEST_PRIMARY_FAILURE"] = "dns"
        self.assert_check(self.run_check())
        self.assertEqual(self.paths, [("123.56.214.77", "/c1/core/v1/stable/manifest.v1"),
                                     ("123.56.214.77", "/c1/core/v1/stable/manifest.v1.sig")])

    def test_default_profile_uses_existing_repository_default(self):
        self.profile.unlink()
        self.assert_check(self.run_check())
        self.assertEqual([host for host, _ in self.paths], ["www.fwz233.com"] * 2)

    def test_oversized_metadata_is_rejected_and_removed(self):
        self.manifest = b"x" * 65537
        self.assert_error(self.run_check())
        self.assertEqual(len(self.paths), 1)

    def assert_stuck_curl_stopped(self, pid_file):
        pid = int(pid_file.read_text())
        status = Path(f"/proc/{pid}/status")
        if status.exists():
            self.assertIn("State:\tZ", status.read_text())

    def test_outer_deadline_stops_stuck_curl_and_cleans_up(self):
        pid_file = self.root / "stuck.pid"
        self.env["C1_TEST_STUCK_CURL_PID"] = str(pid_file)
        self.env["C1_TEST_FAST_CLOCK"] = "1"
        self.committed_state(phase="prepared")
        started = time.monotonic()
        result = self.run_check(timeout=5)
        self.assert_error(result)
        self.assertIn("deadline exceeded", result.stderr)
        self.assertLess(time.monotonic() - started, 3)
        self.assert_stuck_curl_stopped(pid_file)

    def test_cancellation_stops_only_its_curl_and_cleans_up(self):
        pid_file = self.root / "stuck.pid"
        self.env["C1_TEST_STUCK_CURL_PID"] = str(pid_file)
        before = self.snapshot(self.state)
        with subprocess.Popen([str(self.driver), "check-configured"], env=self.env,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True) as process:
            deadline = time.monotonic() + 5
            while not pid_file.exists() and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertTrue(pid_file.exists())
            process.send_signal(signal.SIGTERM)
            output, error = process.communicate(timeout=5)
        self.assertNotEqual(process.returncode, 0)
        self.assertEqual(output, "")
        self.assertIn("cancelled", error)
        self.assert_stuck_curl_stopped(pid_file)
        self.assertEqual(self.snapshot(self.state), before)
        self.assertEqual(sorted(path.name for path in self.staging.iterdir()), [".txn.keep"])


if __name__ == "__main__":
    unittest.main()
