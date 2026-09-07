#!/bin/sh
set -eu
work=$(mktemp -d /usr/data/pinao-validation-XXXXXX)
/usr/data/pinao-test --song "$work/song.json" >"$work/runtime.log" 2>&1 &
pid=$!
trap 'kill "$pid" 2>/dev/null || true' EXIT INT TERM
sleep 1
kill -0 "$pid" || { cat "$work/runtime.log"; printf 'VALIDATION_DIRECTORY=%s\n' "$work"; exit 1; }
/usr/data/pinao-input-test "$pid"
# HOME must end the app itself; timeout is a test failure, not a passed exit.
i=0
while kill -0 "$pid" 2>/dev/null; do
 i=$((i+1)); [ "$i" -lt 5 ] || { cat "$work/runtime.log"; echo HOME_EXIT_FAILED; exit 1; }
 sleep 1
done
wait "$pid"
trap - EXIT INT TERM
printf 'SAVED_SONG\n'
cat "$work/song.json"
printf '\nWAV_EXPORT\n'
ls -l "$work/exports/"*.wav
printf '\nRUNTIME_LOG\n'
cat "$work/runtime.log"
printf '\nPCM_AFTER_EXIT\n'
cat /proc/asound/card0/pcm0p/sub0/status
printf '\nPINAO_INPUT_SAVE_EXPORT_EXIT_OK\n'
printf 'VALIDATION_DIRECTORY=%s\n' "$work"
