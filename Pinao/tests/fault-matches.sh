#!/bin/sh
printf 'FAULT LOG MATCHES\n'
dmesg | grep -iE 'out of memory|killed process|oom|segfault|panic|BUG:|hung|epaper|underrun|pinao' | tail -70
logread 2>/dev/null | grep -iE 'out of memory|killed process|oom|segfault|panic|pinao' | tail -40
printf '\nCORE RECENT EVENTS\n'
tail -200 /storage/c1/update/log/supervisor.log | grep -v 'runtime_stats' | tail -25
printf '\nDISPLAY CONTROL\n'
ls /sys/devices/platform/e0266a128/epaper
printf '\nUSER SONG SETTINGS (NO NOTES)\n'
head -7 /storage/c1/pinao/song.json
