"""Native Linux /proc regression for installer executable-count verification.

Runs only a tiny host-compiled fixture in a private temporary release tree.
No device access, production binaries, enrollment, or installation is involved.
The old shell rule is retained here deliberately, with only its release root
redirected into the fixture directory; these tests document its false rejection.
"""
import hashlib
import os
from pathlib import Path
import select
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest


FIXTURE_SOURCE = r'''
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t stopping;
static void stop(int number) { (void)number; stopping = 1; }
static void report(const char *role)
{
    if (dprintf(STDOUT_FILENO, "%s %ld %ld\n", role,
                (long)getpid(), (long)getppid()) < 0) _exit(91);
}
static void hold(void)
{
    char byte;
    while (!stopping && read(STDIN_FILENO, &byte, 1) > 0) {}
}
static int reap(pid_t child)
{
    int status;
    pid_t result;
    do { result = waitpid(child, &status, 0); }
    while (result < 0 && errno == EINTR);
    return result == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
int main(int argc, char **argv)
{
    struct sigaction action;
    pid_t children[8];
    int count = 0, okay = 1;
    memset(&action, 0, sizeof(action));
    action.sa_handler = stop;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGTERM, &action, NULL) ||
        sigaction(SIGALRM, &action, NULL)) return 90;
    alarm(20); /* A bounded lifetime even if the Python runner disappears. */
    if (argc == 4 && strcmp(argv[1], "launcher") == 0) {
        report("launcher");
        children[count] = fork();
        if (children[count] < 0) return 92;
        if (children[count] == 0) {
            execl(argv[2], argv[2], "ui", argv[3], (char *)NULL);
            _exit(93);
        }
        ++count;
    } else if (argc == 3 && strcmp(argv[1], "ui") == 0) {
        int workers = atoi(argv[2]);
        if (workers < 0 || workers > 8) return 94;
        report("ui");
        while (count < workers) {
            children[count] = fork();
            if (children[count] < 0) { okay = 0; break; }
            if (children[count] == 0) {
                alarm(20); /* fork does not inherit an alarm timer. */
                report("worker");
                hold(); /* Deliberately no exec: same /proc/PID/exe as UI. */
                _exit(0);
            }
            ++count;
        }
    } else return 95;
    hold(); /* Closing fixture stdin broadcasts EOF to this entire tree. */
    for (int i = 0; i < count; ++i) {
        (void)kill(children[i], SIGTERM); /* Only our own unreaped children. */
        if (!reap(children[i])) okay = 0;
    }
    return okay ? 0 : 96;
}
'''


# Same loop and output as the old VerifyRunningAsync probe. The parameter is
# the sole intentional difference: no writes to /usr/data or other real roots.
OLD_PROBE = r'''for p in /proc/[0-9]*/exe; do
    target=$(readlink "$p" 2>/dev/null) || continue
    case "$target" in "$1"/*/artifacts/C1ancher) sha256sum "$p";; esac
done'''


def read_process(pid):
    """Read actual kernel metadata, including names containing spaces or ')'."""
    stat = Path(f"/proc/{pid}/stat").read_text()
    fields = stat[stat.rfind(")") + 2:].split()
    return {
        "pid": pid,
        "ppid": int(fields[1]),
        "exe": os.readlink(f"/proc/{pid}/exe"),
    }


def old_rule_accepts(output, expected):
    lines = [line for line in output.split("\n") if line]
    return len(lines) == 1 and lines[0].startswith(expected + " ")


@unittest.skipUnless(sys.platform.startswith("linux") and Path("/proc/self/exe").exists(),
                     "requires native Linux /proc (use WSL on Windows)")
class InstallerProcessProbeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if shutil.which("cc") is None or shutil.which("sha256sum") is None:
            raise unittest.SkipTest("requires host cc and sha256sum")
        cls.directory = tempfile.TemporaryDirectory(prefix="c1-process-probe-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.root = Path(cls.directory.name)
        cls.releases = cls.root / "usr/data/c1/core/releases"
        cls.ui = cls.releases / "fixture/artifacts/C1ancher"
        cls.ui.parent.mkdir(parents=True)
        cls.launcher = cls.root / "C1ancher-launcher"
        source = cls.root / "fixture.c"
        source.write_text(FIXTURE_SOURCE)
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Wpedantic",
                        "-Werror", str(source), "-o", str(cls.launcher)],
                       check=True, capture_output=True, text=True, timeout=30)
        shutil.copy2(cls.launcher, cls.ui)
        cls.expected = hashlib.sha256(cls.ui.read_bytes()).hexdigest()

    def start_tree(self, workers, ui=None):
        ui = ui or self.ui
        process = subprocess.Popen([str(self.launcher), "launcher", str(ui), str(workers)],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, start_new_session=True)
        records = []
        self.addCleanup(self.stop_tree, process, records)
        output = b""
        deadline = time.monotonic() + 5
        while output.count(b"\n") < workers + 2:
            remaining = deadline - time.monotonic()
            self.assertGreater(remaining, 0, "native fixture readiness timed out")
            self.assertTrue(select.select([process.stdout], [], [], remaining)[0],
                            "native fixture readiness timed out")
            chunk = os.read(process.stdout.fileno(), 4096)
            self.assertTrue(chunk, "native fixture exited before readiness")
            output += chunk
        for line in output.decode().splitlines():
            role, pid, ppid = line.split()
            record = read_process(int(pid))
            self.assertEqual(record["ppid"], int(ppid))
            records.append(dict(record, role=role))
        self.assertEqual(len(records), workers + 2)
        launcher = next(record for record in records if record["role"] == "launcher")
        app = next(record for record in records if record["role"] == "ui")
        self.assertEqual(launcher["pid"], process.pid)
        self.assertEqual(launcher["exe"], str(self.launcher))
        self.assertEqual(app["ppid"], launcher["pid"])
        self.assertEqual(app["exe"], str(ui))
        for worker in (record for record in records if record["role"] == "worker"):
            self.assertEqual(worker["ppid"], app["pid"])
            self.assertEqual(worker["exe"], app["exe"])
        if os.environ.get("C1_PROCESS_PROBE_VERBOSE") == "1":
            for record in sorted(records, key=lambda row: row["pid"]):
                print("native {role}: PID={pid} PPID={ppid} exe={exe}".format(**record),
                      flush=True)
        return records

    def stop_tree(self, process, records):
        # EOF is received by every fixture process; the UI reaps its workers,
        # and the launcher reaps its UI before Python reaps the launcher.
        if process.stdin is not None:
            process.stdin.close()
            process.stdin = None
        try:
            _, stderr = process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            # This session/process group belongs exclusively to this fixture.
            os.killpg(process.pid, signal.SIGTERM)
            try:
                _, stderr = process.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.communicate(timeout=5)
                self.fail("native fixture did not stop cooperatively")
        self.assertEqual(process.returncode, 0, stderr.decode())
        for record in records:
            self.assertFalse(Path(f"/proc/{record['pid']}").exists(),
                             f"fixture PID {record['pid']} was not reaped")

    def probe(self):
        result = subprocess.run(["/bin/sh", "-c", OLD_PROBE, "probe", str(self.releases)],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout

    def assert_matches(self, output, records):
        expected_pids = {row["pid"] for row in records if row["role"] != "launcher"}
        actual_pids = set()
        for line in output.splitlines():
            digest, path = line.split()
            self.assertEqual(digest, self.expected)
            actual_pids.add(int(Path(path).parent.name))
        self.assertEqual(actual_pids, expected_pids)
        self.assertEqual(len(output.splitlines()), len(expected_pids))

    def test_old_rule_accepts_one_ui_without_fork_workers(self):
        records = self.start_tree(0)
        output = self.probe()
        self.assert_matches(output, records)
        self.assertTrue(old_rule_accepts(output, self.expected))

    def test_old_rule_falsely_rejects_ui_with_terminal_supervisor(self):
        records = self.start_tree(1)
        output = self.probe()
        self.assert_matches(output, records)
        self.assertEqual(len(output.splitlines()), 2)
        self.assertFalse(old_rule_accepts(output, self.expected))

    def test_old_rule_falsely_rejects_ui_with_two_fork_workers(self):
        records = self.start_tree(2)
        output = self.probe()
        self.assert_matches(output, records)
        self.assertEqual(len(output.splitlines()), 3)
        self.assertFalse(old_rule_accepts(output, self.expected))

    def test_same_basename_outside_release_tree_is_excluded(self):
        records = self.start_tree(0)
        unrelated = self.root / "unrelated/C1ancher"
        unrelated.parent.mkdir(exist_ok=True)
        shutil.copy2(self.ui, unrelated)
        self.start_tree(1, ui=unrelated)
        output = self.probe()
        self.assert_matches(output, records)
        self.assertTrue(old_rule_accepts(output, self.expected))

    def test_old_rule_rejects_hash_mismatch(self):
        self.start_tree(0)
        self.assertFalse(old_rule_accepts(self.probe(), "0" * 64))

    def test_old_rule_rejects_absent_ui(self):
        output = self.probe()
        self.assertEqual(output, "")
        self.assertFalse(old_rule_accepts(output, self.expected))


if __name__ == "__main__":
    unittest.main(verbosity=2)
