#!/bin/sh
set -eu
app=${1:-/usr/data/pinao-test}
"$app" --smoke-test &
pid=$!
trap 'kill "$pid" 2>/dev/null || true' EXIT INT TERM
sleep 2
printf 'PINAO_PROCESS\n'
cat /proc/$pid/status | grep -E 'Name:|VmRSS:|Threads:'
printf '\nPCM_PARAMETERS\n'
cat /proc/asound/card0/pcm0p/sub0/hw_params
printf '\nPCM_STATUS\n'
cat /proc/asound/card0/pcm0p/sub0/status
printf '\nAPLAY_PROCESS\n'
ps | grep /usr/bin/aplay | grep -v grep
wait "$pid"
trap - EXIT INT TERM
printf '\nPINAO_SMOKE_OK\n'
printf '\nROOT_MOUNT\n'
awk '$2 == "/" { print }' /proc/mounts
