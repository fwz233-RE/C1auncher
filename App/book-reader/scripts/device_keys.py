"""Inject a bounded key sequence into the connected development reader.
Use only while an isolated reader smoke test owns the display.
"""
import argparse
import struct
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument('--adb', required=True)
parser.add_argument('--serial', required=True)
parser.add_argument('codes', type=int, nargs='+')
args = parser.parse_args()
for code in args.codes:
    data = b''.join(struct.pack('<IIHHi', 0, 0, kind, key, value)
                    for kind, key, value in [(1, code, 1), (0, 0, 0), (1, code, 0), (0, 0, 0)])
    escaped = ''.join('\\%03o' % b for b in data)
    command = "printf '%s' > /dev/input/event0" % escaped
    subprocess.run([args.adb, '-s', args.serial, 'shell', command], check=True)
    time.sleep(2)
    result = subprocess.run([args.adb, '-s', args.serial, 'shell', 'pidof book-reader'], capture_output=True, text=True)
    print('key', code, 'reader PID:', result.stdout.strip(), flush=True)
    if not result.stdout.strip():
        raise SystemExit('Reader stopped after key %s' % code)
