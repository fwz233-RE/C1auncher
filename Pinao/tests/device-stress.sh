#!/bin/sh
set -eu
app=$1
mode=${2:-normal}
work=$(mktemp -d /usr/data/pinao-validation-XXXXXX)
printf 'WORK=%s\nAPP=%s\n' "$work" "$app"
"$app" --song "$work/song.json" >"$work/runtime.log" 2>&1 &
pid=$!
input=
trap 'kill "$pid" ${input:-} 2>/dev/null || true' EXIT INT TERM
sleep 1
if ! kill -0 "$pid"; then cat "$work/runtime.log";exit 1;fi
/usr/data/pinao-stress-test "$pid" "$mode" >"$work/input.log" 2>&1 &
input=$!
maxrss=0;minavail=999999;start=$(date +%s)
while kill -0 "$input" 2>/dev/null; do
 if ! kill -0 "$pid" 2>/dev/null;then cat "$work/runtime.log";cat "$work/input.log";echo EARLY_EXIT;exit 1;fi
 rss=$(awk '/VmRSS:/ {print $2}' /proc/$pid/status)
 available=$(awk '/MemAvailable:/ {print $2}' /proc/meminfo)
 [ "$rss" -le "$maxrss" ] || maxrss=$rss
 [ "$available" -ge "$minavail" ] || minavail=$available
 if [ "$available" -lt 10000 ];then echo LOW_MEMORY_ABORT;exit 1;fi
 printf 'SAMPLE %s rss_kb=%s available_kb=%s\n' "$(date +%s)" "$rss" "$available"
 sleep 1
done
wait "$input";input=
cat "$work/input.log"
i=0
while kill -0 "$pid" 2>/dev/null;do
 i=$((i+1));[ "$i" -lt 4 ] || { echo HOME_EXIT_FAILED;cat "$work/runtime.log";exit 1; }
 sleep 1
done
wait "$pid"
trap - EXIT INT TERM
printf 'PASS peak_sample_rss_kb=%s minimum_available_kb=%s elapsed_seconds=%s\n' "$maxrss" "$minavail" "$(( $(date +%s)-start ))"
cat "$work/runtime.log"
[ ! -f "$work/last-session.json" ] || cat "$work/last-session.json"
cat /proc/asound/card0/pcm0p/sub0/status
