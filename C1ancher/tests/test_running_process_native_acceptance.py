"""End-to-end native /proc snapshot -> production C# analyzer; WSL only, no devices."""
import os
from pathlib import Path
import subprocess
import unittest

import test_installer_process_probe as native
from compiled_process_probe import compiled_probe

ROOT = Path(__file__).resolve().parents[1]
DOTNET = '/mnt/c/Program Files/dotnet/dotnet.exe'
ASSEMBLY = r'D:\c1slim\C1ancher\installer\tests\bin\Release\net8.0\Installer.OfflineTests.dll'

@unittest.skipUnless(os.name == 'posix' and Path(DOTNET).is_file(), 'Requires WSL and built offline test assembly')
class NativeProcessAcceptance(unittest.TestCase):
    start_tree = native.InstallerProcessProbeTests.start_tree
    stop_tree = native.InstallerProcessProbeTests.stop_tree

    @classmethod
    def setUpClass(cls):
        native.InstallerProcessProbeTests.setUpClass.__func__(cls)
        old = cls.launcher
        cls.launcher = cls.ui.with_name('C1ancher-launcher')
        old.rename(cls.launcher)
        cls.core = cls.releases.parent
        (cls.core / 'current').symlink_to('releases/fixture')
        cls.command = compiled_probe().replace('/usr/data/c1/core', str(cls.core))

    def analyze(self, expected=0, wrong_hash=False):
        output = subprocess.run(['/bin/sh', '-c', self.command], capture_output=True, text=True, timeout=10)
        self.assertEqual(output.returncode, 0, output.stderr)
        # Map only private fixture root to the production root for the strict C# path validator.
        snapshot = output.stdout.replace(str(self.core), '/usr/data/c1/core')
        result = subprocess.run([DOTNET, ASSEMBLY, '--analyze-running-process-snapshot', '-',
                                 '0' * 64 if wrong_hash else self.expected, self.expected],
                                input=snapshot, capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=15)
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr + snapshot)

    def test_one_main_with_no_worker(self):
        self.start_tree(0)
        self.analyze()

    def test_terminal_worker_shares_exe_but_is_not_second_main(self):
        self.start_tree(1)
        self.analyze()

    def test_two_legitimate_workers(self):
        self.start_tree(2)
        self.analyze()

    def test_two_independent_trees_rejected(self):
        self.start_tree(1)
        self.start_tree(0)
        self.analyze(expected=1)

    def test_wrong_hash_rejected(self):
        self.start_tree(1)
        self.analyze(expected=1, wrong_hash=True)

    def test_absent_main_rejected(self):
        self.analyze(expected=1)

if __name__ == '__main__':
    unittest.main()
