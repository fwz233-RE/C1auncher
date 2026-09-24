#!/usr/bin/env python3
"""Validate locally by default; --install explicitly stages an enrolled device.

Never remounts root, changes trust keys, stops applications, or reboots. Run this
script from the trusted source checkout, not from an untrusted downloaded bundle.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shlex
import stat
import struct
import subprocess
import tarfile
import tempfile
import uuid

ABI = 'mips32r2-little-o32-hard-float-double-static'
COMPONENTS = [('c1ancher', 'C1ancher'), ('c1pkg', 'c1pkg'),
              ('launcher', 'C1ancher-launcher'), ('updater', 'c1updater')]
CORE_ROOT = '/usr/data/c1/core'
STATE_ROOT = '/usr/data/c1/update/state'
CORE_KEY = '/etc/c1updater/core.ed25519.pub'
APP_KEY = '/usr/data/c1/pkg/repository.ed25519.pub'
VERIFIER = '/etc/c1updater/recovery-verifier'
UPDATER = '/usr/data/c1/bin/c1updater'
SPKI = bytes.fromhex('302a300506032b6570032100')


def regular(path, limit):
    path = Path(path)
    for ancestor in (path, *path.parents):
        info = ancestor.lstat()
        if stat.S_ISLNK(info.st_mode) or getattr(info, 'st_file_attributes', 0) & 0x400:
            raise ValueError(f'link/reparse path rejected: {ancestor}')
    info = path.lstat()
    if not stat.S_ISREG(info.st_mode) or info.st_size > limit or info.st_nlink != 1:
        raise ValueError(f'non-regular, hard-linked, or oversized file: {path}')
    data = path.read_bytes()
    if len(data) > limit:
        raise ValueError(f'file grew past limit: {path}')
    return data


def digest(data):
    return hashlib.sha256(data).hexdigest()


def verify_signature(data, signature, raw_key, openssl):
    if len(raw_key) != 32 or len(signature) != 64:
        raise ValueError('Ed25519 key/signature must be 32/64 bytes')
    with tempfile.TemporaryDirectory(prefix='c1-desktop-verify-') as directory:
        root = Path(directory)
        (root / 'key.der').write_bytes(SPKI + raw_key)
        (root / 'data').write_bytes(data)
        (root / 'sig').write_bytes(signature)
        result = subprocess.run([openssl, 'pkeyutl', '-verify', '-rawin', '-pubin',
                                 '-keyform', 'DER', '-inkey', str(root / 'key.der'),
                                 '-in', str(root / 'data'), '-sigfile', str(root / 'sig')],
                                capture_output=True, timeout=15)
        if result.returncode:
            raise ValueError('Ed25519 signature verification failed')


def records(data):
    if not data.endswith(b'\n') or any(c not in (9, 10) and not 32 <= c < 127 for c in data):
        raise ValueError('manifest must be LF-terminated protocol ASCII')
    return [line.split('\t') for line in data.decode('ascii').splitlines()]


def token(value):
    return re.fullmatch(r'[A-Za-z0-9](?:[A-Za-z0-9._+-]{0,62}[A-Za-z0-9])?', value) is not None


def positive(value):
    return re.fullmatch(r'[1-9][0-9]{0,19}', value) and int(value) <= 2**64 - 1


def mips_static(data):
    if len(data) < 52 or data[:7] != b'\x7fELF\x01\x01\x01':
        raise ValueError('expected MIPS little-endian ELF32')
    header = struct.unpack_from('<HHIIIIIHHHHHH', data, 16)
    elf_type, machine, version, _, phoff, _, flags, ehsize, phsize, phnum, *_ = header
    if (elf_type != 2 or machine != 8 or version != 1 or ehsize != 52 or phsize != 32
            or not 1 <= phnum <= 32 or phoff < 52 or phoff + phsize * phnum > len(data)
            or flags & 0xf0000000 != 0x70000000 or flags & 0xf000 != 0x1000):
        raise ValueError('expected MIPS32r2 o32 executable')
    load = abi = False
    for i in range(phnum):
        kind, offset, _, _, size, *_ = struct.unpack_from('<IIIIIIII', data, phoff + i * phsize)
        if kind in (2, 3) or offset + size > len(data):
            raise ValueError('dynamic or malformed MIPS executable')
        load |= kind == 1
        if kind == 0x70000003:
            if size != 24 or data[offset:offset + 8] != bytes((0, 0, 32, 2, 1, 1, 0, 1)):
                raise ValueError('expected MIPS32r2 hard-float double FP32 ABI')
            abi = True
    if not load or not abi:
        raise ValueError('missing MIPS load segment or ABI flags')


def validate_bundle(bundle, core_key, app_key, openssl='openssl'):
    bundle = Path(bundle).absolute()
    manifest = regular(bundle / 'core/manifest.v1', 65536)
    verify_signature(manifest, regular(bundle / 'core/manifest.v1.sig', 64), core_key, openssl)
    lines = records(manifest)
    if len(lines) != 14 or lines[0] != ['C1CORE-MANIFEST 1']:
        raise ValueError('unexpected core manifest format')
    for line, tag in zip(lines[1:10], 'SVETBUCRD'):
        if len(line) != 2 or line[0] != tag or not token(line[1]):
            raise ValueError('invalid core metadata')
    for i in (1, 3, 9):
        if not positive(lines[i][1]):
            raise ValueError('invalid core numeric metadata')
    if lines[4][1] != ABI or lines[7][1] != 'c1-core-v1':
        raise ValueError('unsupported core compatibility/ABI')
    files = {}
    core_bytes = 0
    for line, (role, name) in zip(lines[10:], COMPONENTS):
        if (len(line) != 6 or line[:3] != ['F', role, 'artifacts/' + name]
                or line[5] != '700' or not re.fullmatch('[0-9a-f]{64}', line[3])
                or not positive(line[4])):
            raise ValueError('invalid core component record')
        path = 'core/artifacts/' + name
        data = regular(bundle / path, 32 * 1024 * 1024)
        if len(data) != int(line[4]) or digest(data) != line[3]:
            raise ValueError('core artifact digest/size mismatch: ' + name)
        mips_static(data)
        files[path] = line[3]
        core_bytes += len(data)
    if core_bytes > 96 * 1024 * 1024:
        raise ValueError('core payload exceeds limit')
    files['core/manifest.v1'] = digest(manifest)
    files['core/manifest.v1.sig'] = digest(regular(bundle / 'core/manifest.v1.sig', 64))
    index = regular(bundle / 'apps/index.v1', 256 * 1024)
    signature = regular(bundle / 'apps/index.v1.sig', 64)
    verify_signature(index, signature, app_key, openssl)
    lines_app = records(index)
    if (len(lines_app) != 3 or lines_app[0] != ['C1PKG-INDEX 1']
            or len(lines_app[1]) != 2 or lines_app[1][0] != 'S' or not positive(lines_app[1][1])):
        raise ValueError('expected a single-package offline IME index')
    package = lines_app[2]
    if (len(package) != 8 or package[:2] != ['P', 'c1-ime']
            or not re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+', package[2])
            or package[7] != 'bin/c1-ime-service' or not positive(package[6])
            or not re.fullmatch('[0-9a-f]{64}', package[5])):
        raise ValueError('invalid IME package identity')
    archive = PurePosixPath(package[4])
    if (archive.is_absolute() or str(archive) != package[4] or '..' in archive.parts
            or not re.fullmatch(r'packages/[A-Za-z0-9._+/-]+\.tar\.gz', package[4])):
        raise ValueError('unsafe archive path')
    archive_path = 'apps/' + package[4]
    data = regular(bundle / archive_path, 32 * 1024 * 1024)
    if len(data) != int(package[6]) or digest(data) != package[5]:
        raise ValueError('IME archive digest/size mismatch')
    # No extraction on the host. Reject special files and traversal before any
    # device writes; c1pkg repeats its canonical checks before extraction there.
    total = 0
    entries = set()
    ime_files = {}
    with tarfile.open(bundle / archive_path, 'r:gz') as tar:
        for member in tar:
            path = PurePosixPath(member.name)
            if (len(entries) >= 512 or member.name in entries or not member.isfile()
                    or path.is_absolute() or str(path) != member.name or '..' in path.parts
                    or not re.fullmatch(r'[A-Za-z0-9_./+-]+', member.name)
                    or member.name != 'manifest.v1' and not member.name.startswith('payload/')
                    or not 0 <= member.size <= 16 * 1024 * 1024):
                raise ValueError('unsafe member in IME archive')
            entries.add(member.name)
            total += member.size
            if total > 64 * 1024 * 1024:
                raise ValueError('IME archive unpacked size limit')
            if member.name.startswith('payload/'):
                ime_files[member.name[len('payload/'):]] = digest(tar.extractfile(member).read(16 * 1024 * 1024 + 1))
        required = {'manifest.v1', 'payload/bin/c1-ime-service',
                    'payload/share/rime-data/default.yaml', 'payload/share/rime-data/luna_pinyin_simp.schema.yaml'}
        required.update('payload/share/rime-data/build/' + name for name in (
            'default.yaml', 'luna_pinyin_simp.schema.yaml', 'luna_pinyin_simp.prism.bin',
            'luna_pinyin.table.bin', 'luna_pinyin.reverse.bin'))
        required.update('payload/share/rime-data/opencc/' + name for name in (
            't2s_full.json', 'TSCharacters.ocd2', 'TSPhrases.ocd2', 'variants.txt',
            'variants_ext.txt', 'variants_jp.txt'))
        if not required <= entries:
            raise ValueError('IME package lacks target-precompiled runtime data')
        expected = ('C1PKG-PACKAGE 1\nid\tc1-ime\nversion\t' + package[2] + '\nentry\tbin/c1-ime-service\n').encode()
        if tar.extractfile('manifest.v1').read(4096) != expected:
            raise ValueError('IME archive metadata differs from signed index')
        mips_static(tar.extractfile('payload/bin/c1-ime-service').read(16 * 1024 * 1024 + 1))
        if not tar.getmember('payload/bin/c1-ime-service').mode & 0o111:
            raise ValueError('IME service is not executable')
    files['apps/index.v1'] = digest(index)
    files['apps/index.v1.sig'] = digest(signature)
    files[archive_path] = digest(data)
    return {'core_sequence': int(lines[1][1]), 'core_version': lines[2][1],
            'core_digest': digest(manifest), 'core_bytes': core_bytes,
            'ime_version': package[2], 'ime_unpacked_bytes': total, 'ime_files': ime_files, 'files': files,
            'device_tested': False}


class Device:
    def __init__(self, adb, serial):
        self.adb, self.serial = adb, serial

    def run(self, *arguments, timeout=120):
        result = subprocess.run([self.adb, '-s', self.serial, *map(str, arguments)],
                                capture_output=True, timeout=timeout)
        if result.returncode:
            raise RuntimeError('ADB operation failed: ' + result.stderr.decode(errors='replace')[:500])
        return result.stdout.decode('utf-8', errors='replace').replace('\r', '').strip()

    def shell(self, command, timeout=120):
        # Old adbd does not reliably propagate the remote exit status.
        marker = '__C1_DESKTOP_RC='
        output = self.run('shell', '(set -e; ' + command + '); rc=$?; echo ' + marker + '$rc', timeout=timeout)
        match = re.search(r'(?:^|\n)' + marker + r'(\d+)\s*$', output)
        if not match or match[1] != '0':
            raise RuntimeError('Device operation rejected: ' + output[-1200:])
        return output[:match.start()].strip()


def install_bundle(bundle, report, core_key, app_key, adb, serial, backup):
    if not re.fullmatch(r'[A-Za-z0-9_.:-]+', serial):
        raise ValueError('explicit safe device serial required')
    device = Device(adb, serial)
    if device.shell('id -u') != '0':
        raise ValueError('root ADB required')
    # Read-only checks first. Trust is compared, never replaced from the bundle.
    for path, key in ((CORE_KEY, core_key), (APP_KEY, app_key)):
        observed = device.shell('sha256sum ' + path).split()[0]
        if observed != digest(key):
            raise ValueError('device trust root differs from supplied trusted public key')
    device.shell(VERIFIER + ' verify-current ' + CORE_ROOT + ' ' + CORE_KEY)
    device.shell("awk '$2 == \"/\" && $4 ~ /(^|,)ro(,|$)/ { ok=1 } END { exit !ok }' /proc/mounts")
    device.shell("awk '$2 == \"/storage\" { ok=1 } END { exit !ok }' /proc/mounts")
    state = device.shell(UPDATER + ' state ' + STATE_ROOT)
    phase = re.search(r'\bphase=([a-z_]+)\b', state)
    if not phase or phase[1] not in ('confirmed', 'idle'):
        raise ValueError('device must be idle or confirmed, not a pending transaction')
    # A completed rollback intentionally leaves phase=idle and retains the
    # rejected sequence as the anti-rollback floor. Never force-confirm it.
    # verify-current above authenticates the retained runnable generation;
    # unfinished rollback/boot/restart markers still block an installation.
    for marker in (STATE_ROOT + '/rollback.v1', STATE_ROOT + '/pending-boots.v1',
                   '/usr/data/c1/update/restart-request'):
        device.shell('test ! -e ' + marker + ' && test ! -L ' + marker)
    sequence = re.search(r'\bsequence=(\d+)\b', state)
    if not sequence or report['core_sequence'] <= int(sequence[1]):
        raise ValueError('new core sequence must exceed device state')
    # Leave rollback generations intact and reserve 4 MiB on the small data FS.
    for path, needed in (('/usr/data', report['core_bytes'] + 4 * 1024 * 1024),
                         ('/storage', 160 * 1024 * 1024 + report['ime_unpacked_bytes'])):
        text = device.shell('df -Pk ' + path)
        available = int(text.splitlines()[-1].split()[-3]) * 1024
        if available < needed:
            raise ValueError(f'insufficient free space on {path}; no automatic cleanup')
    backup = Path(backup).absolute()
    backup.mkdir(parents=True, exist_ok=False)
    # Preserve core rollback evidence, startup, settings and user dictionaries.
    # Large media on /storage is untouched and must be backed up by its owner.
    device.run('pull', '/etc/app_daemon', backup / 'app_daemon')
    device.run('pull', '/usr/data/c1', backup / 'c1-data', timeout=600)
    if device.shell('if test -e /storage/c1/ime; then echo present; else echo absent; fi') == 'present':
        device.run('pull', '/storage/c1/ime', backup / 'ime-user-data', timeout=600)
    (backup / 'before.txt').write_text(state + '\n', encoding='utf-8')
    remote = '/storage/c1/desktop-install-' + uuid.uuid4().hex[:16]
    device.shell('umask 077; mkdir ' + remote + '; mkdir ' + remote + '/core ' + remote + '/core/artifacts ' + remote + '/apps')
    # This directory is retained on failure for inspection, never auto-deleted
    # together with an uncertain partially committed install or core generation.
    for relative, expected_hash in report['files'].items():
        local = Path(bundle) / relative
        if digest(regular(local, 32 * 1024 * 1024)) != expected_hash:
            raise ValueError('bundle changed since validation')
        destination = remote + '/' + relative
        device.shell('mkdir -p ' + shlex.quote(str(PurePosixPath(destination).parent)))
        device.run('push', local, destination)
        # Windows adb can preserve permissive source mode bits despite remote
        # umask. Narrow only our newly uploaded file before the fixed verifier
        # or c1pkg opens it; never relax their secure-file checks.
        device.shell('chmod 600 ' + shlex.quote(destination))
        if device.shell('sha256sum ' + shlex.quote(destination)).split()[0] != expected_hash:
            raise ValueError('ADB transfer digest mismatch')
    device.shell(VERIFIER + ' verify-manifest ' + remote + '/core/manifest.v1 ' + remote + '/core/manifest.v1.sig ' + CORE_KEY)
    # Only now execute c1pkg whose digest was bound by the device-verified core
    # manifest. The old core remains active throughout runtime installation.
    pkg = remote + '/core/artifacts/c1pkg'
    device.shell('chmod 700 ' + pkg)
    help_text = device.shell(pkg + ' --help')
    if 'install-local' not in help_text:
        raise ValueError('signed c1pkg does not support offline installation')
    device.shell(pkg + ' --key ' + APP_KEY + ' install-local ' + remote + '/apps c1-ime', timeout=600)
    # An already-current install is a valid c1pkg no-op, not an integrity repair.
    # Independently verify the installed runtime before allowing core preparation.
    target = device.shell('readlink /storage/c1/apps/c1-ime/current')
    if target != 'versions/' + report['ime_version']:
        raise ValueError('installed IME version does not match the signed bundle')
    installed = '/storage/c1/apps/c1-ime/' + target
    for relative, expected_hash in report['ime_files'].items():
        path = shlex.quote(installed + '/' + relative)
        observed = device.shell('test -f ' + path + ' && test ! -L ' + path + ' && sha256sum ' + path).split()[0]
        if observed != expected_hash:
            raise ValueError('installed IME integrity check failed; core was not prepared')
    # Preparing last prevents a desktop upgrade with a missing runtime package.
    result = device.shell(UPDATER + ' prepare-local ' + remote + '/core /storage/c1/update/staging '
                          + CORE_ROOT + ' ' + STATE_ROOT + ' ' + CORE_KEY, timeout=600)
    (backup / 'prepared.json').write_text(json.dumps(dict(report, remote_staging=remote, result=result),
                                                     ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(result)
    print('Runtime installed; signed core prepared. Device has NOT been rebooted.')
    print('Save work, then use the device core-update confirmation screen to restart.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle', required=True, type=Path)
    parser.add_argument('--core-key', required=True, type=Path, help='trusted external raw public key (32 bytes)')
    parser.add_argument('--app-key', required=True, type=Path, help='trusted external raw public key (32 bytes)')
    parser.add_argument('--openssl', default='openssl')
    parser.add_argument('--install', action='store_true', help='explicitly install runtime and prepare core; never reboot')
    parser.add_argument('--adb')
    parser.add_argument('--serial')
    parser.add_argument('--backup', type=Path)
    args = parser.parse_args()
    if args.install and not all((args.adb, args.serial, args.backup)):
        parser.error('--install requires --adb, --serial and a new --backup directory')
    try:
        core_key, app_key = regular(args.core_key, 32), regular(args.app_key, 32)
        report = validate_bundle(args.bundle, core_key, app_key, args.openssl)
        print(json.dumps(report, ensure_ascii=False, indent=2))
        if args.install:
            install_bundle(args.bundle, report, core_key, app_key, args.adb, args.serial, args.backup)
        else:
            print('Local signatures and payloads verified. No ADB/device operations performed.')
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError, tarfile.TarError) as error:
        parser.exit(1, f'BLOCKED: {error}\n')


if __name__ == '__main__':
    main()
