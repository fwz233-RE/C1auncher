#!/usr/bin/env python3
"""Host-only bundle and staging-order regression; no actual ADB or real keys."""
import hashlib
from contextlib import redirect_stdout
import importlib.util
import io
import json
from pathlib import Path
import shutil
import subprocess
import struct
import tarfile
import tempfile
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[1] / 'scripts/prepare-desktop-install.py'
spec = importlib.util.spec_from_file_location('desktop_install', SCRIPT)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class BundleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('openssl'):
            raise RuntimeError('OpenSSL is required (never silently skip signature tests)')
        cls.keys = tempfile.TemporaryDirectory(prefix='c1-desktop-test-keys-')
        cls.key_root = Path(cls.keys.name)
        cls.raw = {}
        for name in ('core', 'app'):
            key = cls.key_root / (name + '.pem')
            subprocess.run(['openssl', 'genpkey', '-algorithm', 'ED25519', '-out', str(key)], check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            der = subprocess.check_output(['openssl', 'pkey', '-in', str(key), '-pubout', '-outform', 'DER'])
            cls.raw[name] = der[12:]
            (cls.key_root / (name + '.pub')).write_bytes(der[12:])

    @classmethod
    def tearDownClass(cls):
        cls.keys.cleanup()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='c1-desktop-bundle-test-')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.bundle = self.root / 'bundle'
        (self.bundle / 'core/artifacts').mkdir(parents=True)
        (self.bundle / 'apps/packages/c1-ime').mkdir(parents=True)
        elf = bytearray(140)
        elf[:7] = b'\x7fELF\x01\x01\x01'
        struct.pack_into('<HHIIIIIHHHHHH', elf, 16, 2, 8, 1, 0x400000, 52, 0, 0x70001007, 52, 32, 2, 0, 0, 0)
        struct.pack_into('<IIIIIIII', elf, 52, 1, 0, 0x400000, 0x400000, 140, 140, 5, 0x10000)
        struct.pack_into('<IIIIIIII', elf, 84, 0x70000003, 116, 0, 0, 24, 24, 4, 8)
        elf[116:124] = bytes((0, 0, 32, 2, 1, 1, 0, 1))
        elf = bytes(elf)
        self.core_lines = ['C1CORE-MANIFEST 1', 'S\t12', 'V\t2.0.0', 'E\t1', 'T\t' + module.ABI,
                           'B\t1.1.0', 'U\t1.1.0', 'C\tc1-core-v1', 'R\ttest-dirty', 'D\t1700000000']
        for role, name in module.COMPONENTS:
            (self.bundle / 'core/artifacts' / name).write_bytes(elf)
            self.core_lines.append(f'F\t{role}\tartifacts/{name}\t{module.digest(elf)}\t{len(elf)}\t700')
        self.sign_core()
        self.members = {
            'manifest.v1': b'C1PKG-PACKAGE 1\nid\tc1-ime\nversion\t0.1.0\nentry\tbin/c1-ime-service\n',
            'payload/bin/c1-ime-service': elf,
            'payload/share/rime-data/build/luna_pinyin.table.bin': b'target-test-table',
            'payload/share/rime-data/build/luna_pinyin_simp.prism.bin': b'target-test-prism',
            'payload/share/rime-data/build/luna_pinyin_simp.schema.yaml': b'schema: {}\n',
        }
        for name in ('default.yaml', 'luna_pinyin_simp.schema.yaml', 'build/default.yaml',
                     'build/luna_pinyin.reverse.bin', 'opencc/t2s_full.json', 'opencc/TSCharacters.ocd2',
                     'opencc/TSPhrases.ocd2', 'opencc/variants.txt', 'opencc/variants_ext.txt', 'opencc/variants_jp.txt'):
            self.members['payload/share/rime-data/' + name] = b'test-data'
        self.sign_app()

    def sign(self, relative, name):
        path = self.bundle / relative
        subprocess.run(['openssl', 'pkeyutl', '-sign', '-rawin', '-inkey', str(self.key_root / (name + '.pem')),
                        '-in', str(path), '-out', str(path) + '.sig'], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def sign_core(self):
        (self.bundle / 'core/manifest.v1').write_text('\n'.join(self.core_lines) + '\n', encoding='ascii')
        self.sign('core/manifest.v1', 'core')

    def sign_app(self, link=None):
        archive = self.bundle / 'apps/packages/c1-ime/0.1.0.tar.gz'
        with tarfile.open(archive, 'w:gz') as tar:
            for name, data in self.members.items():
                member = tarfile.TarInfo(name)
                member.size = len(data)
                member.mode = 0o755 if name.endswith('c1-ime-service') else 0o644
                tar.addfile(member, io.BytesIO(data))
            if link:
                member = tarfile.TarInfo('payload/link')
                member.type = tarfile.SYMTYPE
                member.linkname = link
                tar.addfile(member)
        data = archive.read_bytes()
        index = ('C1PKG-INDEX 1\nS\t12\nP\tc1-ime\t0.1.0\tChinese Input Service\t'
                 f'packages/c1-ime/0.1.0.tar.gz\t{module.digest(data)}\t{len(data)}\tbin/c1-ime-service\n')
        (self.bundle / 'apps/index.v1').write_text(index, encoding='ascii')
        self.sign('apps/index.v1', 'app')

    def validate(self):
        return module.validate_bundle(self.bundle, self.raw['core'], self.raw['app'])

    def test_valid_and_default_no_device(self):
        report = self.validate()
        self.assertFalse(report['device_tested'])
        self.assertEqual(report['ime_version'], '0.1.0')
        self.assertEqual(report['core_sequence'], 12)
        result = subprocess.run(['python3', str(SCRIPT), '--bundle', str(self.bundle),
                                 '--core-key', str(self.key_root / 'core.pub'),
                                 '--app-key', str(self.key_root / 'app.pub'), '--adb', '/DO-NOT-EXEC-ADB'],
                                capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('No ADB/device operations', result.stdout)

    def test_static_mips_abi_validation(self):
        elf = bytearray(self.members['payload/bin/c1-ime-service'])
        struct.pack_into('<I', elf, 36, 0x50001007)  # Wrong ISA architecture.
        with self.assertRaisesRegex(ValueError, 'MIPS32r2'):
            module.mips_static(elf)
        elf = bytearray(self.members['payload/bin/c1-ime-service'])
        struct.pack_into('<I', elf, 52, 3)  # PT_INTERP is not fully static.
        with self.assertRaisesRegex(ValueError, 'dynamic'):
            module.mips_static(elf)
        elf = bytearray(self.members['payload/bin/c1-ime-service'])
        elf[123] = 3  # Soft-float instead of double hard-float.
        with self.assertRaisesRegex(ValueError, 'hard-float'):
            module.mips_static(elf)

    def test_modified_core(self):
        (self.bundle / 'core/artifacts/C1ancher').write_bytes(b'tamper')
        with self.assertRaisesRegex(ValueError, 'digest/size'):
            self.validate()

    def test_wrong_key_and_signature(self):
        with self.assertRaisesRegex(ValueError, 'signature'):
            module.validate_bundle(self.bundle, self.raw['app'], self.raw['core'])
        (self.bundle / 'apps/index.v1.sig').write_bytes(bytes(64))
        with self.assertRaisesRegex(ValueError, 'signature'):
            self.validate()

    def test_signed_malformed_core(self):
        self.core_lines[4] = 'T\tx86_64'
        self.sign_core()
        with self.assertRaisesRegex(ValueError, 'ABI'):
            self.validate()

    def test_link_and_path_traversal(self):
        self.sign_app(link='/etc/app_daemon')
        with self.assertRaisesRegex(ValueError, 'unsafe member'):
            self.validate()
        self.members['payload/../../escape'] = b'x'
        self.sign_app()
        with self.assertRaisesRegex(ValueError, 'unsafe member'):
            self.validate()

    def test_missing_prebuilt_and_mismatched_identity(self):
        name = 'payload/share/rime-data/build/luna_pinyin.table.bin'
        old = self.members.pop(name)
        self.sign_app()
        with self.assertRaisesRegex(ValueError, 'precompiled'):
            self.validate()
        self.members[name] = old
        self.members['manifest.v1'] = b'wrong package'
        self.sign_app()
        with self.assertRaisesRegex(ValueError, 'metadata'):
            self.validate()

    def test_local_symlink_rejected(self):
        artifact = self.bundle / 'core/artifacts/c1pkg'
        content = artifact.read_bytes()
        artifact.unlink()
        source = self.root / 'other'
        source.write_bytes(content)
        artifact.symlink_to(source)
        with self.assertRaisesRegex(ValueError, 'link'):
            self.validate()

    def fake_device(self, fail_runtime=False, wrong_key=False, space_kib=1000000, state_sequence=8,
                    corrupt_runtime=False, state_phase='confirmed', pending_marker=False):
        outer = self
        class FakeDevice:
            def __init__(self, *args):
                self.commands = []
                self.uploads = {}
                self.protected = set()
                outer.fake = self

            def run(self, command, *args, **kwargs):
                self.commands.append((command, *map(str, args)))
                if command == 'push':
                    self.uploads[str(args[1])] = module.digest(Path(args[0]).read_bytes())
                return ''

            def shell(self, command, **kwargs):
                self.commands.append(command)
                if command == 'id -u': return '0'
                if command.startswith('sha256sum '):
                    path = command.split()[1]
                    keys = {module.CORE_KEY: module.digest(outer.raw['core']),
                            module.APP_KEY: module.digest(outer.raw['app'])}
                    value = keys.get(path) or self.uploads[path]
                    return ('0' * 64 if wrong_key else value) + '  ' + path
                if ' state ' in command:
                    return f'state: generation=1 phase={state_phase} sequence={state_sequence} security_epoch=1 release=1.3.6'
                if command.startswith('test ! -e ') and pending_marker:
                    raise RuntimeError('pending transaction marker')
                if command.startswith('df '):
                    return f'Filesystem 1024-blocks Used Available Capacity Mounted on\n/dev/test 9999999 1 {space_kib} 1% /storage'
                if command.startswith('readlink '): return 'versions/0.1.0'
                if command.startswith('test -f ') and ' && sha256sum ' in command:
                    path = command.rsplit('sha256sum ', 1)[1]
                    relative = path.split('/versions/0.1.0/', 1)[1]
                    return ('0' * 64 if corrupt_runtime else module.digest(outer.members['payload/' + relative])) + '  ' + path
                if command.startswith('chmod 600 '):
                    path = command.removeprefix('chmod 600 ')
                    outer.assertIn(path, self.uploads)
                    self.protected.add(path)
                if ' verify-manifest ' in command:
                    outer.assertEqual(self.protected, set(self.uploads),
                                      'every Windows upload must be made private before device verification')
                if command.endswith(' --help'): return 'install-local REPOSITORY_DIR ID'
                if ' install-local ' in command and fail_runtime:
                    raise RuntimeError('runtime rejected')
                if ' prepare-local ' in command: return 'phase=prepared release=2.0.0 sequence=12'
                return ''
        return FakeDevice

    def install(self, fake):
        # Device is a simulation: do not print simulated installation success as
        # if the host regression had really contacted or changed a device.
        with patch.object(module, 'Device', fake), redirect_stdout(io.StringIO()):
            module.install_bundle(self.bundle, self.validate(), self.raw['core'], self.raw['app'],
                                  'unused-adb', 'TEST-ONLY', self.root / 'backup')

    def test_runtime_before_prepare_and_never_reboot(self):
        self.install(self.fake_device())
        commands = [c for c in self.fake.commands if isinstance(c, str)]
        runtime = next(i for i, c in enumerate(commands) if ' install-local ' in c)
        prepare = next(i for i, c in enumerate(commands) if ' prepare-local ' in c)
        verify = next(i for i, c in enumerate(commands) if ' verify-manifest ' in c)
        execute = next(i for i, c in enumerate(commands) if c.endswith(' --help'))
        self.assertLess(runtime, prepare)
        self.assertLess(verify, execute)
        self.assertFalse(any(any(word in c for word in ('reboot', 'remount', 'kill ', 'rollback ')) for c in commands))
        saved = json.loads((self.root / 'backup/prepared.json').read_text())
        self.assertIn('phase=prepared', saved['result'])

    def test_runtime_failure_never_prepares_core(self):
        with self.assertRaisesRegex(RuntimeError, 'runtime rejected'):
            self.install(self.fake_device(fail_runtime=True))
        self.assertFalse(any(' prepare-local ' in c for c in self.fake.commands))

    def test_corrupt_current_runtime_never_prepares_core(self):
        with self.assertRaisesRegex(ValueError, 'integrity check'):
            self.install(self.fake_device(corrupt_runtime=True))
        self.assertFalse(any(' prepare-local ' in c for c in self.fake.commands))

    def test_completed_rollback_can_install_newer_sequence(self):
        self.install(self.fake_device(state_phase='idle', state_sequence=11))
        self.assertTrue(any(' prepare-local ' in c for c in self.fake.commands))
        self.assertFalse(any(' confirm ' in c for c in self.fake.commands))

    def test_pending_markers_rejected_before_writes(self):
        for phase in ('idle', 'confirmed'):
            with self.subTest(phase=phase), self.assertRaisesRegex(RuntimeError, 'pending transaction marker'):
                self.install(self.fake_device(state_phase=phase, pending_marker=True))
            self.assertFalse((self.root / 'backup').exists())
            self.assertFalse(any(isinstance(c, tuple) for c in self.fake.commands))

    def test_preflight_failure_has_no_writes(self):
        for options in ({'wrong_key': True}, {'space_kib': 1}, {'state_sequence': 15},
                        {'state_phase': 'prepared'}, {'state_phase': 'pending_boot'},
                        {'state_phase': 'downloading'}, {'state_phase': 'verified'}, {'state_phase': 'unknown'}):
            with self.subTest(options=options), self.assertRaises(ValueError):
                self.install(self.fake_device(**options))
            self.assertFalse((self.root / 'backup').exists())
            self.assertFalse(any(isinstance(c, tuple) for c in self.fake.commands))
            self.assertFalse(any(c.startswith('umask') for c in self.fake.commands))


if __name__ == '__main__':
    unittest.main()
