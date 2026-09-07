#!/bin/sh
# Background CHECK ONLY. Installing/restarting always remains a user action.
set -u
state=/usr/data/c1/pkg
check_once() {
    [ -x /usr/data/c1/bin/c1pkg ] && [ -s "$state/repository.url" ] || return 0
    temporary="$state/.last-check.$$"
    if /usr/data/c1/bin/c1pkg check-updates > "$temporary" 2>&1; then
        printf '\nchecked_at=%s\nstatus=ok\n' "$(date +%s)" >> "$temporary"
    else
        printf '\nchecked_at=%s\nstatus=offline_or_failed\n' "$(date +%s)" >> "$temporary"
    fi
    chmod 600 "$temporary"
    mv -f "$temporary" "$state/last-check"
}
if [ "${1:-}" = once ]; then check_once; exit $?; fi
# Each device chooses a different startup delay; a shared firmware image does
# not embed a shared device identity or cause twenty simultaneous requests.
while :; do
    if [ -r /proc/sys/kernel/random/uuid ]; then
        seed=$(cksum < /proc/sys/kernel/random/uuid | cut -d ' ' -f 1)
    else
        seed=$(date +%s)
    fi
    sleep "$((seed % 121))"
    check_once
    sleep 21600
done
