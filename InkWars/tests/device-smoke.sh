#!/bin/sh
# C1ancher MUST remain running: stopping it would hide display ownership bugs.
# No reboot, refresh counter writes, force-kill, or application mode overrides.
set -eu
cd /usr/data/inkwars-test
./test_game-mips
./test_ui-mips /usr/data/inkwars-test/previews
printf 'launcher_state='; pidof C1ancher || true
printf 'refresh_before='; cat /sys/devices/platform/e0266a128/epaper/refresh_cnt
# Logical input replay through the real UI and display lease; not a hand test.
./inkwars --save /usr/data/inkwars-test/smoke.sav --replay osssssoddwoobsssso --frames /usr/data/inkwars-test/display-frames
cat /usr/data/inkwars/last-run.log
./inkwars --save /usr/data/inkwars-test/smoke.sav --replay so --frames /usr/data/inkwars-test/reload-frames
cat /usr/data/inkwars/last-run.log
printf 'refresh_after='; cat /sys/devices/platform/e0266a128/epaper/refresh_cnt
printf 'fast_only='; cat /sys/devices/platform/e0266a128/epaper/fast_refresh_only
printf 'save='; wc -c /usr/data/inkwars-test/smoke.sav
printf 'DEVICE_SMOKE_PASS\n'
