#!/usr/bin/env python3
"""Fast preparation safety regressions; no devices, QEMU or build required."""
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

INTEGRATION = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(INTEGRATION))
spec = importlib.util.spec_from_file_location('prepare_runtime', INTEGRATION / 'prepare-runtime.py')
prepare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare)
from runtime_support import BUILD_FILES, OPENCC_FILES, check_elf, sha256


class PreparationSafetyTests(unittest.TestCase):
    def make_payload(self, root):
        names = ['bin/c1-ime-service', 'NOTICE.txt', 'share/rime-data/default.yaml',
                 'share/rime-data/luna_pinyin_simp.schema.yaml']
        names += ['share/rime-data/build/' + n for n in BUILD_FILES]
        names += ['share/rime-data/opencc/' + n for n in OPENCC_FILES]
        names += ['licenses/' + n for n in (*prepare.LICENSE_FILES, *prepare.SYSTEM_LICENSES,
                  'keysym.h-NOTICE.txt', 'keysymdef.h-NOTICE.txt', 'luna-pinyin-dictionary-NOTICE.txt')]
        for name in names:
            target = root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(b'test')
        return root

    def test_exact_allowlist(self):
        with tempfile.TemporaryDirectory() as temp:
            root = self.make_payload(Path(temp))
            prepare.validate_payload(root)
            self.assertEqual(len(list((root / 'bin').iterdir())), 1)

    def test_rejects_learned_database_tests_and_keys(self):
        for name in ('share/rime-data/luna_pinyin.userdb/LOG', 'bin/c1-ime-sdk-test',
                     'private.pem', 'C1ancher-server/hidden.c', 'share/rime-data/essay.txt',
                     'share/rime-data/luna_pinyin.dict.yaml'):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as temp:
                root = self.make_payload(Path(temp))
                path = root / name; path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b'not for runtime')
                with self.assertRaisesRegex(RuntimeError, 'allowlist'):
                    prepare.validate_payload(root)

    def test_missing_dictionary_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = self.make_payload(Path(temp))
            (root / 'share/rime-data/build/luna_pinyin.table.bin').unlink()
            with self.assertRaisesRegex(RuntimeError, 'allowlist'):
                prepare.validate_payload(root)

    def test_symlink_source_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'real').write_bytes(b'content')
            (root / 'link').symlink_to(root / 'real')
            with self.assertRaisesRegex(RuntimeError, 'regular'):
                prepare.copy_file(root / 'link', root / 'copy')

    def test_target_abi_and_static_hardening_checks(self):
        good = ('ELF32 little endian MIPS o32 mips32r2 Hard float CPR1 size: 32\n'
                ' GNU_STACK 0x0 0x0 0x0 0x0 0x0 RW 0x10\n GNU_RELRO\n')
        with patch('runtime_support.subprocess.check_output', return_value=good):
            check_elf(Path('unused'))
        for bad in (good.replace('MIPS', 'X86-64'), good.replace('RW ', 'RWE '),
                    good + '\nINTERP\n', good + '\n(NEEDED)\n', good.replace('Hard float', 'Soft float')):
            with self.subTest(bad=bad), patch('runtime_support.subprocess.check_output', return_value=bad):
                with self.assertRaises(RuntimeError):
                    check_elf(Path('unused'))

    def test_copy_preserves_bytes(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); (root / 'input').write_bytes(bytes(range(256)))
            prepare.copy_file(root / 'input', root / 'sub/output')
            self.assertEqual(sha256(root / 'input'), sha256(root / 'sub/output'))


if __name__ == '__main__':
    unittest.main()
