#!/usr/bin/env python3
"""Maintenance probe: production USB lifecycle, manual reconnect, no recovery fallback.

Start requires an operator at the device. Collect is read-only on the device and
can be rerun after a delayed wake. No persistent power preferences are changed.
"""
import argparse
import hashlib
import json
from datetime import datetime, timezone
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--adb', required=True)
    parser.add_argument('--serial', required=True)
    parser.add_argument('--record', type=Path, required=True)
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument('--start', action='store_true')
    action.add_argument('--collect', action='store_true')
    parser.add_argument('--binary', type=Path)
    parser.add_argument('--confirm-suspend', action='store_true')
    args = parser.parse_args()

    def adb(*parts):
        result = subprocess.run([args.adb, '-s', args.serial, *parts],
                                capture_output=True, timeout=15)
        text = result.stdout.decode(errors='replace').replace('\r', '')
        if result.returncode:
            raise RuntimeError(text + result.stderr.decode(errors='replace'))
        return text.strip()

    def shell(command):
        return adb('shell', command)

    def save(name, value):
        (args.record / name).write_text(value + '\n', encoding='utf-8')

    if args.start:
        if not args.confirm_suspend or args.binary is None:
            parser.error('--start requires --confirm-suspend and --binary')
        binary = args.binary.resolve(strict=True)
        args.record.mkdir(parents=True, exist_ok=False)
        tag = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')
        remote = '/dev/shm/c1-manual-usb-' + tag
        if shell('id -u') != '0':
            raise RuntimeError('Root ADB required')
        if shell('cat /sys/devices/platform/gpio_keys/power/wakeup') != 'enabled':
            raise RuntimeError('Physical power-key wake is not enabled')
        baseline = dict(serial=args.serial, remote=remote,
                        boot=shell('cat /proc/sys/kernel/random/boot_id'),
                        success=int(shell('cat /sys/power/suspend_stats/success')),
                        binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                        preference=shell('if [ -e /usr/data/c1/disable-auto-suspend ]; then sha256sum /usr/data/c1/disable-auto-suspend; else echo absent; fi'))
        save('baseline.json', json.dumps(baseline, indent=2))
        save('processes-before.txt', shell('ps'))
        # No fallback, no host-ack timer, no full USB restart, no reboot.
        wrapper = '#!/bin/sh\nexec > ' + remote + '.wrapper.log 2>&1\nsleep 5\n' + remote + '.bin --suspend-confirmed > ' + remote + '.probe.log 2>&1\nresult=$?\necho helper_exit=$result\necho complete\nexit "$result"\n'
        script = args.record / 'wrapper.sh'
        script.write_bytes(wrapper.encode())
        adb('push', str(binary), remote + '.bin')
        adb('push', str(script), remote + '.sh')
        if shell('sha256sum ' + remote + '.bin').split()[0] != baseline['binary_sha256']:
            raise RuntimeError('Uploaded probe hash mismatch')
        if shell('chmod 700 ' + remote + '.bin && sh -n ' + remote + '.sh; echo result=$?') != 'result=0':
            raise RuntimeError('Probe permission/syntax preparation failed')
        result = shell('/bin/busybox start-stop-daemon -S -b -m -p ' + remote + '.pid -x /bin/sh -- ' + remote + '.sh; echo result=$?')
        if result != 'result=0':
            raise RuntimeError('Probe launch failed: ' + result)
        print('Scheduled deep suspend after 5 seconds. Short-press power to wake, then replug USB if needed. No fallback is installed.', flush=True)
        return

    baseline = json.loads((args.record / 'baseline.json').read_text(encoding='utf-8'))
    if baseline['serial'] != args.serial:
        raise RuntimeError('Record belongs to another device')
    remote = baseline['remote']
    # The record is local input, never interpolate arbitrary shell characters.
    import re
    if not re.fullmatch(r'/dev/shm/c1-manual-usb-[0-9]{8}T[0-9]{6}Z', remote):
        raise RuntimeError('Invalid probe path in record')
    log = shell('cat ' + remote + '.probe.log')
    wrapper = shell('cat ' + remote + '.wrapper.log')
    save('device-probe.log', log)
    save('wrapper.log', wrapper)
    save('kernel-after.log', shell('dmesg'))
    boot = shell('cat /proc/sys/kernel/random/boot_id')
    success = int(shell('cat /sys/power/suspend_stats/success'))
    preference = shell('if [ -e /usr/data/c1/disable-auto-suspend ]; then sha256sum /usr/data/c1/disable-auto-suspend; else echo absent; fi')
    summary = dict(boot_unchanged=boot == baseline['boot'], success_before=baseline['success'],
                   success_after=success, helper_ok='probe_result=ok' in log.splitlines(),
                   helper_exited='helper_exit=0' in wrapper.splitlines(),
                   preference_unchanged=preference == baseline['preference'],
                   fallback_configured=False, host_shell_connected=True,
                   manual_replug_observed='operator confirmation required',
                   automatic_reconnect_validated=False)
    summary['passed'] = (summary['boot_unchanged'] and success == baseline['success'] + 1
                         and summary['helper_ok'] and summary['helper_exited']
                         and summary['preference_unchanged'])
    save('summary.json', json.dumps(summary, indent=2))
    print(json.dumps(summary, indent=2))
    if not summary['passed']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
