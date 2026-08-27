#!/bin/sh
set -eu

done_flag=/dev/shm/C1ancher.done
guard_pid=/dev/shm/C1ancher-guard.pid
app_path=/dev/shm/C1ancher
control_path=/dev/shm/C1ancher-control.sh
led_state=/dev/shm/c1-led.state
audio_state=/dev/shm/c1-asound.state
display_state=/dev/shm/c1-display.state
audio_control_path=/dev/shm/c1-audio-control.sh

app_daemon_running() {
    for cmdline in /proc/[0-9]*/cmdline; do
        [ -r "$cmdline" ] || continue
        command_line=$(tr '\000' ' ' < "$cmdline")
        case "$command_line" in
            *"/etc/app_daemon"*) return 0 ;;
        esac
    done
    return 1
}

start_original() {
    if ! app_daemon_running; then
        /bin/busybox start-stop-daemon -S -b -x /etc/app_daemon
    fi
}

stop_guard() {
    if [ -f "$guard_pid" ]; then
        /bin/busybox start-stop-daemon -K -p "$guard_pid" -s TERM -o || true
        rm -f "$guard_pid"
    fi
}

led_snapshot() {
    : > "$led_state"
    for path in /sys/class/leds/led2 /sys/class/leds/led3 /sys/class/leds/led4 /sys/class/leds/led5; do
        [ -d "$path" ] || return 1
        name=${path##*/}
        IFS= read -r trigger_line < "$path/trigger"
        trigger=${trigger_line#*\[}
        trigger=${trigger%%\]*}
        IFS= read -r brightness < "$path/brightness"
        [ -n "$trigger" ] || return 1
        printf '%s|%s|%s\n' "$name" "$trigger" "$brightness" >> "$led_state"
    done
}

led_restore() {
    [ -f "$led_state" ] || return 0
    result=0
    while IFS='|' read -r name trigger brightness; do
        path=/sys/class/leds/$name
        printf '%s' none > "$path/trigger" || result=1
        printf '%s' "$brightness" > "$path/brightness" || result=1
        printf '%s' "$trigger" > "$path/trigger" || result=1
    done < "$led_state"
    rm -f "$led_state"
    return "$result"
}

audio_snapshot() {
    rm -f "$audio_state"
    /usr/sbin/alsactl -f "$audio_state" store 0
}

audio_restore() {
    [ -f "$audio_state" ] || return 0
    result=0
    /usr/sbin/alsactl -f "$audio_state" restore 0 || result=1
    rm -f "$audio_state"
    return "$result"
}

display_snapshot() {
    directory=/sys/devices/platform/e0266a128/epaper
    : > "$display_state"
    for attribute in refresh_max fast_refresh_only; do
        [ -r "$directory/$attribute" ] || return 1
        IFS= read -r value < "$directory/$attribute"
        printf '%s|%s\n' "$attribute" "$value" >> "$display_state"
    done
}

display_restore() {
    [ -f "$display_state" ] || return 0
    directory=/sys/devices/platform/e0266a128/epaper
    result=0
    while IFS='|' read -r attribute value; do
        printf '%s' "$value" > "$directory/$attribute" || result=1
    done < "$display_state"
    rm -f "$display_state"
    return "$result"
}

case "${1:-}" in
    start-app)
        start_original
        ;;
    stop-app)
        /etc/init.d/S80app stop
        ;;
    led-snapshot)
        led_snapshot
        ;;
    audio-snapshot)
        audio_snapshot
        ;;
    audio-restore)
        audio_restore
        ;;
    display-snapshot)
        display_snapshot
        ;;
    display-restore)
        display_restore
        ;;
    guard)
        delay=${2:?guard delay is required}
        stop_guard
        rm -f "$done_flag"
        /bin/busybox start-stop-daemon -S -b -m -p "$guard_pid" -x "$0" -- recover-after "$delay"
        ;;
    recover-after)
        delay=${2:?guard delay is required}
        sleep "$delay"
        if [ ! -e "$done_flag" ]; then
            led_restore || true
            audio_restore || true
            display_restore || true
            start_original
        fi
        rm -f "$guard_pid"
        ;;
    finish)
        touch "$done_flag"
        stop_guard
        killall -TERM C1ancher 2>/dev/null || true
        led_restore || true
        audio_restore || true
        display_restore || true
        start_original
        ;;
    input-owners)
        ls -l /proc/[0-9]*/fd/* 2>/dev/null | grep -E ' -> /dev/input/event[01]$' || true
        ;;
    cleanup)
        touch "$done_flag"
        stop_guard
        led_restore || true
        audio_restore || true
        display_restore || true
        rm -f "$app_path" "$done_flag" "$guard_pid" "$control_path" "$led_state" "$audio_state" "$display_state" "$audio_control_path"
        ;;
    *)
        echo "usage: $0 {start-app|stop-app|led-snapshot|audio-snapshot|audio-restore|display-snapshot|display-restore|guard SECONDS|recover-after SECONDS|finish|input-owners|cleanup}" >&2
        exit 64
        ;;
esac