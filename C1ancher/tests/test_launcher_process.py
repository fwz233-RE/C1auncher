"""Host process regressions; compiles the real launcher, never accesses hardware."""
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]


class LauncherProcessTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="c1-launcher-process-")
        self.root = Path(self.directory.name)
        self.launcher = self.root / "C1ancher-launcher"
        subprocess.run([
            "cc", "-D_POSIX_C_SOURCE=200809L", "-std=c11", "-Wall", "-Wextra",
            "-Wpedantic", "-Werror", "-I" + str(ROOT / "src"),
            '-DC1_LAUNCHER_DATA_ROOT="' + str(self.root / 'usr/data') + '"',
            str(ROOT / "src/launcher/main.c"), str(ROOT / "src/launcher/cleanup.c"),
            str(ROOT / "src/update/state.c"), str(ROOT / "src/security/secure_file.c"),
            str(ROOT / "src/launcher/policy.c"),
            str(ROOT / "src/platform/liveness.c"), str(ROOT / "src/platform/shutdown.c"),
            "-o", str(self.launcher),
        ], check=True)
        self.process = None

    def tearDown(self):
        if self.process is not None:
            try:
                os.killpg(self.process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            self.process.wait(timeout=5)
        self.directory.cleanup()

    def start(self, body, *args):
        app = self.root / "C1ancher"
        app.write_text("#!/bin/sh\n" + body)
        app.chmod(0o700)
        self.process = subprocess.Popen([str(self.launcher), *args], start_new_session=True)

    def wait_for(self, predicate):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if predicate():
                return
            time.sleep(0.01)
        self.fail("launcher fixture timed out")

    def start_python(self, body, *args, **kwargs):
        app = self.root / "C1ancher"
        app.write_text("#!/usr/bin/python3\n" + body)
        app.chmod(0o700)
        env = dict(os.environ, C1_HEARTBEAT_STARTUP_MS="500",
                   C1_HEARTBEAT_TIMEOUT_MS="300")
        env.update(kwargs.pop("env", {}))
        self.process = subprocess.Popen([str(self.launcher), *args],
                                        start_new_session=True, env=env, **kwargs)

    def start_standard(self, sequence, version, body, python=False, env=None, launch=True, core=None):
        release = (core or self.root) / "releases" / f"{sequence}-{version}"
        artifacts = release / "artifacts"
        artifacts.mkdir(parents=True)
        launcher = artifacts / "C1ancher-launcher"
        shutil.copy2(self.launcher, launcher)
        launcher.chmod(0o700)
        app = artifacts / "C1ancher"
        app.write_text(("#!/usr/bin/python3\n" if python else "#!/bin/sh\n") + body)
        app.chmod(0o700)
        if env is None:
            env = dict(os.environ)
        if launch:
            self.process = subprocess.Popen([str(launcher)], start_new_session=True, env=env)
        return release, artifacts, launcher

    def test_release_cleanup_uses_running_release_and_runs_once(self):
        calls = self.root / "calls"
        late = self.root / "releases" / "8-created-after-cleanup" / "artifacts"
        body = (
            "import os,time\nfrom pathlib import Path\n"
            f"calls=Path({str(calls)!r})\n"
            "count=int(calls.read_text()) if calls.exists() else 0\n"
            "calls.write_text(str(count + 1))\n"
            f"late=Path({str(late)!r})\n"
            "if count == 0:\n"
            " late.mkdir(parents=True)\n"
            " (late/'payload').write_text('created after cleanup')\n"
            " raise SystemExit(1)\n"
            "while True:\n"
            " os.write(int(os.environ['C1_UI_HEARTBEAT_FD']), b'H')\n"
            " time.sleep(.05)\n"
        )
        old_newest = self.root / "releases" / "9-previous" / "artifacts"
        old_oldest = self.root / "releases" / "4-old" / "artifacts"
        newer = self.root / "releases" / "13-future" / "artifacts"
        old_newest.mkdir(parents=True)
        old_oldest.mkdir(parents=True)
        newer.mkdir(parents=True)
        _, artifacts, _ = self.start_standard(
            12, "new", body, python=True,
            env=dict(os.environ, C1_HEARTBEAT_STARTUP_MS="500",
                     C1_HEARTBEAT_TIMEOUT_MS="300"))
        self.wait_for(lambda: calls.exists() and calls.read_text() == "2")
        self.assertTrue((artifacts / "C1ancher").exists())
        self.assertTrue(old_newest.exists())
        self.assertFalse(old_oldest.parent.exists())
        self.assertTrue(newer.exists())
        self.assertTrue(late.exists())
        self.process.terminate()
        self.assertEqual(self.process.wait(timeout=5), 0)

    def test_partition_is_not_cleaned_by_unconfirmed_startup_or_ui_restart(self):
        data = self.root / 'usr/data'
        c1 = data / 'c1'
        core = c1 / 'core'
        (c1 / 'update/state').mkdir(parents=True, mode=0o700)
        c1.chmod(0o777)
        for name in ('21-old', '22-old', '23-previous'):
            (core / 'releases' / name / 'artifacts').mkdir(parents=True)
        (core / 'current').symlink_to('releases/24-new')
        (core / 'previous').symlink_to('releases/23-previous')
        old = c1 / 'backups/old-payload'
        old.parent.mkdir(mode=0o777)
        old.write_text('old backup')
        old.chmod(0o666)
        kept = c1 / 'book-reader/documents/book.txt'
        kept.parent.mkdir(parents=True)
        kept.write_text('keep the user document')
        calls = self.root / 'calls'
        late = data / 'pinao-smoke.sh'
        body = (
            "import os,time\nfrom pathlib import Path\n"
            f"calls=Path({str(calls)!r})\n"
            "count=int(calls.read_text()) if calls.exists() else 0\n"
            "calls.write_text(str(count + 1))\n"
            "if count == 0:\n"
            f" Path({str(late)!r}).write_text('created after startup cleanup')\n"
            " raise SystemExit(1)\n"
            "while True:\n"
            " os.write(int(os.environ['C1_UI_HEARTBEAT_FD']), b'H')\n"
            " time.sleep(.05)\n"
        )
        self.start_standard(24, 'new', body, python=True, core=core,
                            env=dict(os.environ, C1_HEARTBEAT_STARTUP_MS='500',
                                     C1_HEARTBEAT_TIMEOUT_MS='300'))
        self.wait_for(lambda: calls.exists() and calls.read_text() == '2')
        self.assertTrue(old.parent.exists())
        self.assertTrue((core / 'releases/21-old').exists())
        self.assertTrue((core / 'releases/22-old').exists())
        self.assertTrue((core / 'releases/23-previous').exists())
        self.assertEqual(kept.read_text(), 'keep the user document')
        self.assertEqual(c1.stat().st_mode & 0o777, 0o777)
        self.assertTrue(late.exists())
        self.process.terminate()
        self.assertEqual(self.process.wait(timeout=5), 0)

    def test_cleanup_failure_does_not_block_launcher(self):
        releases = self.root / "releases"
        releases.mkdir()
        old = releases / "3-oldest" / "artifacts"
        old.mkdir(parents=True)
        newest = releases / "9-previous" / "artifacts"
        newest.mkdir(parents=True)
        os.mkfifo(old / "unsafe")
        _, _, launcher = self.start_standard(
            12, "new", "echo started > '" + str(self.root / "started") + "'\nsleep 60\n",
            launch=False)
        self.process = subprocess.Popen([str(launcher)], start_new_session=True)
        self.wait_for((self.root / "started").exists)
        self.assertTrue(old.parent.exists())
        self.process.terminate()
        self.assertEqual(self.process.wait(timeout=5), 0)

    def test_missing_startup_heartbeat_revokes_ready(self):
        ready = self.root / "ready"
        self.start_python(f"from pathlib import Path\nimport time\n"
                          f"Path({str(ready)!r}).write_text('digest')\ntime.sleep(60)\n",
                          "--ready-file", str(ready))
        self.wait_for(ready.exists)
        self.wait_for(lambda: not ready.exists())
        self.process.terminate()
        self.assertEqual(self.process.wait(timeout=5), 0)

    def test_heartbeat_hang_before_and_during_ready(self):
        ready = self.root / "ready"
        marker = self.root / "pulse"
        self.start_python(
            "import os,time\nfrom pathlib import Path\n"
            "os.write(int(os.environ['C1_UI_HEARTBEAT_FD']), b'H')\n"
            f"Path({str(ready)!r}).write_text('digest')\n"
            f"Path({str(marker)!r}).touch()\ntime.sleep(60)\n",
            "--ready-file", str(ready))
        self.wait_for(marker.exists)
        self.wait_for(lambda: not ready.exists())
        self.process.terminate()
        self.assertEqual(self.process.wait(timeout=5), 0)

    def test_continuous_heartbeats_survive_idle_window(self):
        marker = self.root / "pid"
        self.start_python(
            "import os,time\nfrom pathlib import Path\n"
            f"Path({str(marker)!r}).write_text(str(os.getpid()))\n"
            "while True:\n os.write(int(os.environ['C1_UI_HEARTBEAT_FD']), b'H')\n time.sleep(.05)\n")
        self.wait_for(lambda: marker.exists() and marker.stat().st_size > 0)
        child = int(marker.read_text())
        time.sleep(.9)
        os.kill(child, 0)
        self.process.terminate()
        self.assertEqual(self.process.wait(timeout=5), 0)

    def test_healthy_ui_reaps_adopted_exited_app(self):
        ui, descendant_file, release = (self.root / name for name in
                                         ("ui", "descendant", "release"))
        self.start_python(
            "import os,time\nfrom pathlib import Path\n"
            f"Path({str(ui)!r}).write_text(str(os.getpid()))\n"
            "child=os.fork()\n"
            "if child == 0:\n"
            " if os.fork() == 0:\n"
            "  os.setsid()\n"
            f"  Path({str(descendant_file)!r}).write_text(str(os.getpid()))\n"
            f"  while not Path({str(release)!r}).exists(): time.sleep(.01)\n"
            "  os._exit(75)\n"
            " os._exit(0)\n"
            "os.waitpid(child,0)\n"
            "while True:\n os.write(int(os.environ['C1_UI_HEARTBEAT_FD']),b'H')\n time.sleep(.05)\n")
        self.wait_for(lambda: descendant_file.exists() and descendant_file.stat().st_size > 0)
        descendant = int(descendant_file.read_text())
        ui_pid = int(ui.read_text())
        # Adoption alone must not terminate a still-running application.
        time.sleep(.2)
        self.assertTrue(Path(f"/proc/{descendant}").exists())
        release.touch()
        self.wait_for(lambda: not Path(f"/proc/{descendant}").exists())
        os.kill(ui_pid, 0)
        self.assertIsNone(self.process.poll())
        self.process.terminate()
        self.assertEqual(self.process.wait(timeout=5), 0)

    def test_crashed_ui_cleans_detached_lock_holder(self):
        import fcntl
        pidfile, lock = self.root / "descendant", self.root / "runlock"
        self.start_python(
            "import os,time,fcntl,signal\nfrom pathlib import Path\n"
            "child=os.fork()\n"
            "if child == 0:\n"
            " os.setsid()\n signal.signal(signal.SIGHUP,signal.SIG_IGN)\n"
            f" fd=os.open({str(lock)!r},os.O_CREAT|os.O_RDWR,0o600)\n"
            " fcntl.flock(fd,fcntl.LOCK_EX)\n"
            f" Path({str(pidfile)!r}).write_text(str(os.getpid()))\n time.sleep(60)\n"
            "else:\n"
            f" while not (Path({str(pidfile)!r}).exists() and Path({str(pidfile)!r}).stat().st_size > 0): time.sleep(.01)\n"
            " os._exit(75)\n")
        # Creation precedes write_text's write; wait for the PID, not just
        # the empty inode, before signalling or checking the descendant.
        self.wait_for(lambda: pidfile.exists() and pidfile.stat().st_size > 0)
        descendant = int(pidfile.read_text())
        self.assertEqual(self.process.wait(timeout=5), 75)
        with self.assertRaises(ProcessLookupError):
            os.kill(descendant, 0)
        with lock.open("r+") as stream:
            fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)

    def test_killed_ui_cleans_detached_lock_holder(self):
        pidfile, ui = self.root / "descendant", self.root / "ui"
        self.start_python(
            "import os,time,signal\nfrom pathlib import Path\n"
            f"Path({str(ui)!r}).write_text(str(os.getpid()))\n"
            "child=os.fork()\n"
            "if child == 0:\n"
            " os.setsid()\n signal.signal(signal.SIGHUP,signal.SIG_IGN)\n"
            f" Path({str(pidfile)!r}).write_text(str(os.getpid()))\n"
            " time.sleep(60)\n"
            "else:\n"
            " while True:\n  os.write(int(os.environ['C1_UI_HEARTBEAT_FD']),b'H')\n  time.sleep(.05)\n")
        # Creation precedes write_text's write; wait for the PID, not just
        # the empty inode, before signalling or checking the descendant.
        self.wait_for(lambda: pidfile.exists() and pidfile.stat().st_size > 0)
        descendant = int(pidfile.read_text())
        os.kill(int(ui.read_text()), signal.SIGKILL)
        self.wait_for(lambda: not Path(f"/proc/{descendant}").exists())
        self.process.terminate()
        self.assertEqual(self.process.wait(timeout=5), 0)

    def test_stopped_ui_is_detected_while_external_app_runs(self):
        ready, ui, app = self.root / "ready", self.root / "ui", self.root / "app"
        self.start_python(
            "import os,time\nfrom pathlib import Path\n"
            f"Path({str(ui)!r}).write_text(str(os.getpid()))\n"
            f"Path({str(ready)!r}).write_text('digest')\n"
            "if os.fork() == 0:\n os.setsid()\n"
            f" Path({str(app)!r}).write_text(str(os.getpid()))\n time.sleep(60)\n"
            "else:\n while True:\n  os.write(int(os.environ['C1_UI_HEARTBEAT_FD']),b'H')\n  time.sleep(.05)\n",
            "--ready-file", str(ready))
        self.wait_for(lambda: app.exists() and app.stat().st_size > 0)
        descendant = int(app.read_text())
        os.kill(int(ui.read_text()), signal.SIGSTOP)
        self.wait_for(lambda: not ready.exists())
        self.wait_for(lambda: not Path(f"/proc/{descendant}").exists())
        self.process.terminate()
        self.assertEqual(self.process.wait(timeout=5), 0)

    def test_cleanup_does_not_signal_unrelated_process(self):
        sentinel = subprocess.Popen(["sleep", "60"], start_new_session=True)
        try:
            self.start("exit 75\n")
            self.assertEqual(self.process.wait(timeout=5), 75)
            self.assertIsNone(sentinel.poll())
        finally:
            sentinel.kill()
            sentinel.wait(timeout=5)

    def test_upstream_progress_is_bound_to_ui_pid(self):
        import struct
        read_fd, write_fd = os.pipe()
        os.set_blocking(read_fd, False)
        try:
            self.start_python(
                "import os,time\nwhile True:\n os.write(int(os.environ['C1_UI_HEARTBEAT_FD']),b'H')\n time.sleep(.05)\n",
                env={"C1_SUPERVISOR_HEARTBEAT_FD": str(write_fd)}, pass_fds=(write_fd,))
            import select
            self.assertTrue(select.select([read_fd], [], [], 3)[0])
            timestamp, pid = struct.unpack("=qq", os.read(read_fd, 16))
            self.assertGreater(timestamp, 0)
            self.assertGreater(pid, 0)
            self.assertNotEqual(pid, self.process.pid)
            self.process.terminate()
            self.assertEqual(self.process.wait(timeout=5), 0)
        finally:
            os.close(read_fd)
            os.close(write_fd)

    def test_shutdown_exit_is_propagated_without_restart(self):
        calls, ready = self.root / "calls", self.root / "ready"
        self.start(f"echo started >> '{calls}'\necho ready > '{ready}'\nexit 76\n",
                   "--ready-file", str(ready))
        self.assertEqual(self.process.wait(timeout=5), 76)
        self.assertEqual(calls.read_text().splitlines(), ["started"])
        self.assertFalse(ready.exists())

    def test_stop_during_backoff_does_not_spawn_again(self):
        calls = self.root / "calls"
        self.start(f"echo started >> '{calls}'\nexit 1\n")
        self.wait_for(calls.exists)
        time.sleep(0.2)  # Real launcher has reaped the immediate failure.
        self.process.send_signal(signal.SIGTERM)
        self.assertEqual(self.process.wait(timeout=5), 0)
        self.assertEqual(calls.read_text().splitlines(), ["started"])

    def test_app_exit_clears_readiness_during_backoff(self):
        ready = self.root / "ready"
        exited = self.root / "exited"
        self.start(f"echo ready > '{ready}'\ntouch '{exited}'\nexit 1\n",
                   "--ready-file", str(ready))
        self.wait_for(exited.exists)
        self.wait_for(lambda: not ready.exists())
        self.process.send_signal(signal.SIGTERM)
        self.assertEqual(self.process.wait(timeout=5), 0)

    def test_stop_reaps_term_ignoring_child(self):
        started = self.root / "started"
        self.start(f"trap '' TERM\ntouch '{started}'\nwhile :; do :; done\n")
        self.wait_for(started.exists)
        self.process.send_signal(signal.SIGTERM)
        self.assertEqual(self.process.wait(timeout=5), 0)


if __name__ == "__main__":
    unittest.main()
