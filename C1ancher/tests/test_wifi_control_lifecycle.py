#!/usr/bin/env python3
"""Exercise production Wi-Fi control transport/readiness against a private daemon.

The daemon models wpa's asynchronous INTERFACE_DISABLED and FAIL-BUSY replies.
It is not a radio/supplicant implementation. User, mount and network namespaces
isolate /run and expose only loopback. No real hardware, DHCP or credentials are
accessed. Cold module loading and address acquisition remain unit-test/target
acceptance responsibilities. Run: python3 tests/test_wifi_control_lifecycle.py
"""
import argparse
import heapq
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]

DRIVER = r'''#include "services/wifi.c"
#include <assert.h>
static bool foreign_dhcp;
static int stopped, cleared;
static bool exists(const char *path) { (void)path; return true; }
static bool unblocked(void) { return true; }
static bool enable(bool enabled) {
    char output[64];
    return real_command(enabled ? "TEST_ENABLE" : "TEST_DISABLE", output, sizeof(output), 500) &&
           reply_is(output, "OK");
}
static bool no_address(char *value, size_t size) { (void)size; value[0] = '\0'; return false; }
static bool available(void) { return !foreign_dhcp; }
static bool stop(void) { ++stopped; return true; }
static bool clear(void) { ++cleared; return true; }
int main(void) {
    c1_wifi_snapshot snapshot;
    hardware_io.exists = exists;
    hardware_io.unblock = unblocked;
    hardware_io.enable = enable;
    wifi_io.ipv4 = no_address;
    wifi_io.dhcp_available = available;
    wifi_io.stop_dhcp = stop;
    wifi_io.clear_address = clear;
    /* wifi_io.ready/command/events/observe remain the production functions. */
    assert(c1_wifi_read_snapshot(&snapshot) && snapshot.state == C1_WIFI_DISABLED);
    assert(c1_wifi_scan(&snapshot) == C1_STATUS_OK);
    assert(snapshot.network_count == 1 && strcmp(snapshot.networks[0].ssid, "fresh") == 0);
    assert(c1_wifi_disable(&snapshot) == C1_STATUS_OK);
    assert(snapshot.state == C1_WIFI_DISABLED && stopped == 1 && cleared == 1);
    /* Simulate a new UI's static state while retaining the managed daemon. */
    current_state = C1_WIFI_DISABLED;
    current_phase = C1_WIFI_PHASE_IDLE;
    explicitly_disabled = false;
    observation.valid = false;
    assert(c1_wifi_read_snapshot(&snapshot) && snapshot.state == C1_WIFI_DISABLED);
    assert(c1_wifi_scan(&snapshot) == C1_STATUS_OK);
    assert(snapshot.network_count == 1 && strcmp(snapshot.networks[0].ssid, "fresh") == 0);
    foreign_dhcp = true;
    assert(c1_wifi_disable(&snapshot) == C1_STATUS_UNAVAILABLE);
    assert(stopped == 1 && cleared == 1);
    puts("Wi-Fi isolated control lifecycle passed (real readiness/socket I/O; simulated radio/DHCP)");
    return 0;
}
'''


class Daemon:
    def __init__(self):
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        directory = Path('/var/run/wpa_supplicant')
        directory.mkdir(parents=True, mode=0o700)
        self.socket.bind(str(directory / 'wlan0'))
        self.socket.settimeout(0.01)
        self.monitors = set()
        self.events = []
        self.enabled_at = float('inf')
        self.busy_left = 2
        self.scan_starts = 0
        self.disabled_scan_requests = 0
        self.disconnects = 0
        self.enables = 0
        self.disables = 0
        self.rows = ''
        self.running = True
        self.error = None
        self.thread = threading.Thread(target=self.serve, daemon=True)

    def serve(self):
        try:
            while self.running:
                now = time.monotonic()
                while self.events and self.events[0][0] <= now:
                    _, ssid, monitors = heapq.heappop(self.events)
                    self.rows = ssid
                    for address in monitors:
                        try:
                            self.socket.sendto(b'<3>CTRL-EVENT-SCAN-RESULTS\n', address)
                        except OSError:
                            self.monitors.discard(address)
                try:
                    data, address = self.socket.recvfrom(4096)
                except socket.timeout:
                    continue
                command = data.decode('ascii')
                response = self.handle(command, address)
                self.socket.sendto(response.encode('ascii'), address)
        except BaseException as error:
            self.error = error

    def handle(self, command, address):
        if command == 'PING':
            return 'PONG\n'
        if command == 'TEST_ENABLE':
            self.enables += 1
            self.enabled_at = time.monotonic() + 0.15
            return 'OK\n'
        if command == 'TEST_DISABLE':
            self.disables += 1
            self.enabled_at = float('inf')
            return 'OK\n'
        if command == 'STATUS':
            state = 'INTERFACE_DISABLED' if time.monotonic() < self.enabled_at else 'DISCONNECTED'
            return f'wpa_state={state}\n'
        if command == 'ATTACH':
            self.monitors.add(address)
            return 'OK\n'
        if command == 'BSS_FLUSH 0':
            self.rows = ''
            return 'OK\n'
        if command == 'SCAN':
            self.scan_starts += 1
            if time.monotonic() < self.enabled_at:
                self.disabled_scan_requests += 1
                return 'FAIL\n'
            busy = self.busy_left > 0
            if busy:
                self.busy_left -= 1
            heapq.heappush(self.events, (time.monotonic() + 0.03,
                                        'stale' if busy else 'fresh', tuple(self.monitors)))
            return 'FAIL-BUSY\n' if busy else 'OK\n'
        if command == 'LIST_NETWORKS':
            return 'network id / ssid / bssid / flags\n'
        if command == 'BSS FIRST':
            assert self.rows, 'BSS read preceded scan completion'
            return ('id=1\nbssid=02:00:00:00:00:01\nfreq=2412\nlevel=-40\nflags=[ESS]\n'
                    f'ssid={self.rows}\n')
        if command == 'BSS NEXT-1':
            return ''
        if command == 'DISCONNECT':
            self.disconnects += 1
            return 'OK\n'
        raise AssertionError(f'Unexpected noncredential command: {command}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--isolated', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if not args.isolated:
        environment = dict(os.environ)
        for kind in ('mnt', 'net'):
            environment['C1_WIFI_PARENT_' + kind] = os.readlink('/proc/self/ns/' + kind)
        subprocess.run(['unshare', '--user', '--map-root-user', '--mount', '--net',
                        sys.executable, str(Path(__file__).resolve()), '--cc', args.cc,
                        '--isolated'], check=True, env=environment)
        return
    assert socket.if_nameindex() == [(1, 'lo')], 'Only isolated loopback is permitted'
    # Never mount on the host namespace, even if --isolated was used manually.
    for kind in ('mnt', 'net'):
        assert os.environ['C1_WIFI_PARENT_' + kind] != os.readlink('/proc/self/ns/' + kind)
    subprocess.run(['mount', '-t', 'tmpfs', '-o', 'mode=0755', 'tmpfs', '/run'], check=True)
    with tempfile.TemporaryDirectory(prefix='c1-wifi-lifecycle-') as temporary:
        source = Path(temporary) / 'driver.c'
        binary = Path(temporary) / 'driver'
        source.write_text(DRIVER)
        subprocess.run([args.cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-I' + str(ROOT / 'src'), str(source), '-o', str(binary)], check=True)
        daemon = Daemon()
        daemon.thread.start()
        try:
            subprocess.run([str(binary)], check=True, timeout=15)
            assert daemon.error is None, 'Control daemon failed'
            assert daemon.enables == 2 and daemon.disables == 1
            assert daemon.scan_starts == 4 and daemon.disabled_scan_requests == 0
            assert daemon.disconnects == 1, 'Foreign DHCP refusal must precede DISCONNECT'
        finally:
            daemon.running = False
            daemon.thread.join(timeout=2)
            daemon.socket.close()


if __name__ == '__main__':
    main()
