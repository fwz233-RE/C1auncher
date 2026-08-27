#!/bin/sh
set -eu

process_count() {
    name=$1
    pids=$(pidof "$name" 2>/dev/null || true)
    if [ -z "$pids" ]; then
        echo 0
    else
        echo "$pids" | wc -w
    fi
}

wait_for_replacement() {
    old_pid=$1
    count=0
    while [ "$count" -lt 15 ]; do
        new_pid=$(pidof C1ancher 2>/dev/null || true)
        if [ -n "$new_pid" ] && [ "$new_pid" != "$old_pid" ]; then
            echo "$new_pid"
            return 0
        fi
        sleep 1
        count=$((count + 1))
    done
    return 1
}

crash_restart() {
    [ "$(process_count app_daemon)" -eq 1 ]
    [ "$(process_count C1ancher)" -eq 1 ]
    [ "$(process_count mpenMain)" -eq 0 ]

    attempt=1
    while [ "$attempt" -le 5 ]; do
        old_pid=$(pidof C1ancher)
        kill -KILL "$old_pid"
        new_pid=$(wait_for_replacement "$old_pid")
        [ -n "$new_pid" ]
        [ "$(process_count app_daemon)" -eq 1 ]
        [ "$(process_count C1ancher)" -eq 1 ]
        [ "$(process_count mpenMain)" -eq 0 ]
        echo "restart_$attempt=$old_pid->$new_pid"
        attempt=$((attempt + 1))
    done
    echo 'repeated C1ancher crash restart verified'
}

case "${1:-}" in
    crash-restart) crash_restart ;;
    *)
        echo "usage: $0 crash-restart" >&2
        exit 64
        ;;
esac