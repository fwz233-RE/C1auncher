#!/usr/bin/env python3
"""Optional offline integration against a host wpa_supplicant (driver=none).

Usage: python3 tests/test_wifi_wpa_protocol.py --supplicant /path/to/wpa_supplicant
The test creates an unprivileged user/network namespace with only loopback;
no physical interface, real SSID, DHCP or device config is used. The
production encoder is compiled in a temporary directory; only synthetic test
credentials are sent to a private host daemon. No credentials are printed.
"""
import argparse
import hashlib
import random
import sys
from pathlib import Path
import socket
import subprocess
import tempfile
import time


ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--supplicant', required=True, type=Path)
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--isolated', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if not args.isolated:
        subprocess.run(['unshare', '--user', '--map-root-user', '--net', sys.executable,
                        str(Path(__file__).resolve()), '--supplicant', str(args.supplicant.resolve()),
                        '--cc', args.cc, '--isolated'], check=True)
        return
    assert socket.if_nameindex() == [(1, 'lo')], 'Only isolated loopback is permitted'
    with tempfile.TemporaryDirectory(prefix='c1-wifi-protocol-') as temporary:
        root = Path(temporary)
        source = root / 'encoder.c'
        source.write_text('''#include "services/wifi.c"
int main(int argc, char **argv) {
    char encoded[65];
    if (argc != 3 || !derive_wpa_psk(argv[1], argv[2], encoded)) return 1;
    fputs(encoded, stdout);
    secure_clear(encoded, sizeof(encoded));
    return 0;
}
''')
        encoder = root / 'encoder'
        subprocess.run([args.cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-I' + str(ROOT / 'src'), str(source), '-o', str(encoder)], check=True)
        control = root / 'control'
        config = root / 'wpa.conf'
        config.write_text(f'ctrl_interface={control}\nupdate_config=1\nap_scan=0\n')
        config.chmod(0o600)
        process = subprocess.Popen([str(args.supplicant.resolve()), '-Dnone', '-ilo',
                                    '-c', str(config)], stdin=subprocess.DEVNULL,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        client = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        client.settimeout(3)
        try:
            deadline = time.monotonic() + 5
            endpoint = control / 'lo'
            while not endpoint.exists():
                if process.poll() is not None:
                    # Startup precedes all synthetic credential commands.
                    startup = process.communicate()[0].decode(errors='replace')
                    raise RuntimeError('Test supplicant startup failed: ' + startup)
                if time.monotonic() >= deadline:
                    raise RuntimeError('Test supplicant unavailable; driver=none is required')
                time.sleep(0.02)
            client.bind(str(root / 'client'))
            client.connect(str(endpoint))

            def command(text):
                client.send(text.encode('ascii'))
                return client.recv(4096).decode('ascii').rstrip('\n')

            assert command('PING') == 'PONG'
            # Reproduce the old quoting bug against the real parser using a
            # synthetic credential: backslashes are NOT escape syntax here.
            old_password = 'slash\\pass'
            network = command('ADD_NETWORK')
            assert command(f'SET_NETWORK {network} ssid 63312d73796e746865746963') == 'OK'
            old_encoded = '"' + old_password.replace('\\', '\\\\').replace('"', '\\"') + '"'
            assert command(f'SET_NETWORK {network} psk {old_encoded}') == 'OK'
            assert command('SAVE_CONFIG') == 'OK'
            assert ('\tpsk=' + old_encoded + '\n').encode() in config.read_bytes()
            assert ('\tpsk="' + old_password + '"\n').encode() not in config.read_bytes()
            assert command(f'REMOVE_NETWORK {network}') == 'OK'
            # Merely removing escaping still leaves wpa's text-config comment
            # parser: an embedded quote before '#' cannot roundtrip on reload.
            network = command('ADD_NETWORK')
            assert command(f'SET_NETWORK {network} ssid 63312d73796e746865746963') == 'OK'
            assert command(f'SET_NETWORK {network} psk "quote\"#pass"') == 'OK'
            assert command('SAVE_CONFIG') == 'OK'
            # Some newer daemons may fix their text parser; both outcomes are
            # valid here. The derived-key cases below MUST always roundtrip.
            legacy_reload = command('RECONFIGURE')
            assert legacy_reload in ('FAIL', 'OK')
            assert command(f'REMOVE_NETWORK {network if legacy_reload == "FAIL" else "0"}') == 'OK'
            cases = ['plain123', 'quote"pass', 'slash\\pass', ' both " \\ ',
                     'tail123\\', '\\' * 63, '"' * 63, 'quote"#pass', 'end1234"']
            rng = random.Random(20260919)
            cases += [''.join(chr(rng.randrange(32, 127)) for _ in range(size)) for size in range(8, 64)]
            for case_index, password in enumerate(cases):
                ssid = ['c1-synthetic', 'A' * 32, '网络测试', ' with spaces '][case_index % 4]
                network = command('ADD_NETWORK')
                assert network.isdecimal()
                assert command(f'SET_NETWORK {network} ssid {ssid.encode().hex()}') == 'OK'
                assert command(f'SET_NETWORK {network} key_mgmt WPA-PSK') == 'OK'
                encoded = subprocess.run([str(encoder), ssid, password], check=True,
                                         capture_output=True, text=True).stdout
                reference = hashlib.pbkdf2_hmac('sha1', password.encode('ascii'), ssid.encode(), 4096, 32).hex()
                assert encoded == reference, f'KDF mismatch in synthetic case {case_index}'
                assert command(f'SET_NETWORK {network} psk {encoded}') == 'OK'
                assert command(f'GET_NETWORK {network} psk') == '*'
                assert command('SAVE_CONFIG') == 'OK'
                # The real parser/writer must preserve the derived key through
                # both save and reload. Never include contents in failures.
                expected = ('\tpsk=' + reference + '\n').encode('ascii')
                if expected not in config.read_bytes():
                    raise AssertionError('Supplicant changed synthetic credential bytes')
                assert command('RECONFIGURE') == 'OK', f'Reload rejected case {case_index}'
                assert command('SAVE_CONFIG') == 'OK'
                if expected not in config.read_bytes():
                    raise AssertionError('Reload changed synthetic credential bytes')
                # Reload renumbers the sole retained network to zero.
                assert command('REMOVE_NETWORK 0') == 'OK'
            print(f'Wi-Fi real wpa protocol passed ({len(cases)} synthetic passphrases; no hardware; '
                  f'legacy quote/hash reload rejected={legacy_reload == "FAIL"})')
        finally:
            client.close()
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


if __name__ == '__main__':
    main()
