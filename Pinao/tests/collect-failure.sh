#!/bin/sh
# Read-only snapshot. No restart, signal, cache clearing or user song content.
printf 'UPTIME\n'; uptime
printf '\nMEMINFO\n'; cat /proc/meminfo
printf '\nVM COUNTERS\n'; grep -E 'oom|allocstall|pgmajfault' /proc/vmstat
printf '\nKERNEL LOG\n'; dmesg | tail -160
printf '\nSYSTEM LOG\n'; logread 2>/dev/null | tail -100
printf '\nCORE LOG\n'; tail -80 /storage/c1/update/log/supervisor.log
printf '\nLEASE\n'; ls -l /dev/shm/c1ancher-external-app.*; cat /dev/shm/c1ancher-external-app.mode
printf '\nCORE PROCESSES\n'
for pid in 3746 3747 3748; do
 printf 'PID %s\n' "$pid"; cat /proc/$pid/status; cat /proc/$pid/wchan; ls -l /proc/$pid/fd 2>/dev/null
 done
printf '\nPCM\n'; cat /proc/asound/card0/pcm0p/sub0/status
printf '\nFILESYSTEM SPACE\n'; df -k /storage /usr/data /dev/shm
printf '\nPINAO FILE METADATA\n'; ls -l /storage/c1/pinao /storage/c1/pinao/exports 2>/dev/null
printf '\nEPAPER\n'; cat /sys/devices/platform/e0266a128/epaper/fast_refresh_only
