#!/bin/sh
set -eu
printf 'INPUT DEVICES\n'
cat /proc/bus/input/devices
printf '\nAUDIO MIXER\n'
/usr/bin/amixer sget Speaker 2>/dev/null || true
printf '\nPOST-TEST PROCESSES\n'
ps | grep -E 'pinao|aplay|C1ancher' | grep -v grep
