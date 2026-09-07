#!/bin/sh

# Boot-time verification tools must come from the protected root filesystem.
PATH=/bin:/sbin:/usr/bin:/usr/sbin
export PATH

SLOT_ROOT=/etc/c1updater
SLOT_FILE=/usr/data/c1/update/updater-slot
UPDATE_ROOT=/usr/data/c1/update
STATE_ROOT=/usr/data/c1/update/state
CORE_ROOT=/usr/data/c1/core
KEY=/etc/c1updater/core.ed25519.pub
READY_FILE=/usr/data/c1/update/ready
LAUNCHER_PATH="$CORE_ROOT/current/artifacts/C1ancher-launcher"
LOG_DIR=/storage/c1/update/log
LOG_FILE=$LOG_DIR/supervisor.log
LOG_LIMIT=65536
FATAL=71
SLOT_SWITCH=72
TRANSIENT=75
# Installed only by trusted enrollment/maintenance, never by a core release.
RECOVERY_VERIFIER=/etc/c1updater/recovery-verifier
CRASH_LIMIT=3
SWITCH_LIMIT=3
child_pid=

run_logged() {
    # Only the enrolled, independent helper may collect logs. Old/missing or
    # unexecutable helpers must not prevent a healthy slot from starting.
    # Never interpret the wrapped command's exit code as a helper failure.
    if [ -f "$RECOVERY_VERIFIER" ] && [ ! -L "$RECOVERY_VERIFIER" ] &&
       [ -x "$RECOVERY_VERIFIER" ] &&
       [ "$("$RECOVERY_VERIFIER" --log-version 2>/dev/null)" = C1RUNLOG-1 ]; then
        run_child "$RECOVERY_VERIFIER" run-logged "$LOG_FILE" "$LOG_LIMIT" 4 "$@"
    else
        # Inherit the original service output; do not open an unbounded file
        # when runtime collection is unavailable.
        run_child "$@"
    fi
}

root_mount_mode() {
    while IFS=' ' read -r device mountpoint filesystem options remainder; do
        [ "$mountpoint" = / ] || continue
        case ",$options," in
            *,ro,*) printf '%s\n' ro; return 0 ;;
            *,rw,*) printf '%s\n' rw; return 0 ;;
            *) return 1 ;;
        esac
    done </proc/mounts
    return 1
}

root_is_read_only() {
    [ "$(root_mount_mode 2>/dev/null)" = ro ]
}

remount_root_rw() {
    mode=$(root_mount_mode 2>/dev/null) || mode=unknown
    [ "$mode" = rw ] && return 0
    /bin/mount -o remount,rw / 2>/dev/null || true
    [ "$(root_mount_mode 2>/dev/null)" = rw ]
}

remount_root_ro() {
    mode=$(root_mount_mode 2>/dev/null) || mode=unknown
    [ "$mode" = ro ] && return 0
    sync
    /bin/mount -o remount,ro / 2>/dev/null || true
    [ "$(root_mount_mode 2>/dev/null)" = ro ]
}

ensure_root_ro() {
    root_is_read_only || remount_root_ro
}

monotonic_seconds() {
    IFS=' ' read -r elapsed ignored </proc/uptime || return 1
    printf '%s\n' "${elapsed%%.*}"
}

increase_delay() {
    [ "$delay" -ge 30 ] || delay=$((delay * 2))
    [ "$delay" -le 30 ] || delay=30
}

read_slot() {
    slot=a
    if [ -r "$SLOT_FILE" ]; then
        IFS= read -r value < "$SLOT_FILE" || value=
        case "$value" in
            a|b) slot=$value ;;
        esac
    fi
}

# Never exec the supervisor: this shell must retain the remount, stop and
# failover loop, including when a generation updater exits 71/72/75.
run_child() {
    "$@" &
    child_pid=$!
    wait "$child_pid"
    child_status=$?
    child_pid=
    return "$child_status"
}

stop_bootstrap() {
    trap '' HUP INT TERM
    if [ -n "$child_pid" ]; then
        kill -TERM "$child_pid" 2>/dev/null || true
        wait "$child_pid" 2>/dev/null || true
    fi
    exit 0
}

record_slot() {
    # Pointer persistence is best effort for emergency failover. In particular,
    # an ENOEXEC/missing active updater cannot prevent trying the other slot.
    [ -x "$RECOVERY_VERIFIER" ] || return 1
    run_child "$RECOVERY_VERIFIER" activate-updater-slot "$UPDATE_ROOT" "$1"
}

run_selected() {
    case "$selection" in
        a|b)
            updater="$SLOT_ROOT/slot-$selection/c1updater"
            [ -f "$updater" ] && [ ! -L "$updater" ] && [ -x "$updater" ] || return "$FATAL"
            run_logged "$updater" supervise "$STATE_ROOT" "$CORE_ROOT" "$KEY" "$READY_FILE" "$LAUNCHER_PATH"
            ;;
        previous|current)
            [ -f "$RECOVERY_VERIFIER" ] && [ ! -L "$RECOVERY_VERIFIER" ] && [ -x "$RECOVERY_VERIFIER" ] || return "$FATAL"
            ensure_root_ro || return "$FATAL"
            run_logged "$RECOVERY_VERIFIER" recovery-supervise "$STATE_ROOT" "$CORE_ROOT" "$KEY" \
                "$selection" "$READY_FILE" "$LAUNCHER_PATH"
            ;;
    esac
}

install_prepared_slot() {
    [ -x "$RECOVERY_VERIFIER" ] || return "$FATAL"
    case "$slot" in a) other=b ;; *) other=a ;; esac
    remount_root_rw || return "$FATAL"
    run_child "$RECOVERY_VERIFIER" install-prepared-slot "$SLOT_ROOT" "$slot" "$CORE_ROOT" "$STATE_ROOT" "$KEY"
    install_status=$?
    remount_root_ro || return "$FATAL"
    [ "$install_status" -eq "$TRANSIENT" ] && return "$TRANSIENT"
    [ "$install_status" -eq 0 ] || return "$FATAL"
    record_slot "$other" || return "$TRANSIENT"
    slot=$other
    selection=$slot
    return 0
}

trap stop_bootstrap HUP INT TERM
trap 'ensure_root_ro >/dev/null 2>&1 || true' EXIT

# A trusted first enrollment may be interrupted after installing this real
# bootstrap but before prepare/activation completes. The protected, temporary
# fallback is only the approved pre-enrollment startup script, never a copy of
# factory learning software. It is removed before enrollment can succeed.
# A completed enrollment can never take this path, even if its core is broken.
first_enrollment_fallback() {
    fallback=$SLOT_ROOT/enrollment-startup
    record=$SLOT_ROOT/enrollment-startup.sha256
    [ -e "$record" ] || [ -L "$record" ] || return 1
    [ ! -e "$UPDATE_ROOT/enrolled.v1" ] && [ ! -L "$UPDATE_ROOT/enrolled.v1" ] || return 1
    ensure_root_ro || return "$FATAL"
    for entry in "$SLOT_ROOT" "$fallback" "$record"; do
        [ ! -L "$entry" ] || return "$FATAL"
    done
    [ -d "$SLOT_ROOT" ] && [ -f "$fallback" ] && [ -f "$record" ] || return "$FATAL"
    [ "$(stat -c '%a:%u:%g' "$SLOT_ROOT")" = 700:0:0 ] || return "$FATAL"
    [ "$(stat -c '%a:%u:%g:%h' "$fallback")" = 700:0:0:1 ] || return "$FATAL"
    [ "$(stat -c '%a:%u:%g:%h:%s' "$record")" = 600:0:0:1:65 ] || return "$FATAL"
    expected=$(cat "$record") || return "$FATAL"
    [ "${#expected}" -eq 64 ] || return "$FATAL"
    case "$expected" in *[!0-9a-f]*) return "$FATAL" ;; esac
    actual=$(sha256sum "$fallback") || return "$FATAL"
    [ "${actual%% *}" = "$expected" ] || return "$FATAL"
    run_child "$fallback"
    return 0
}
first_enrollment_fallback
fallback_status=$?
case "$fallback_status" in
    0) exit 0 ;;
    1) ;; # No initial-enrollment authorization: use signed core recovery only.
    *) exit "$FATAL" ;;
esac

read_slot
selection=$slot
first_slot=$slot
case "$slot" in a) second_slot=b ;; *) second_slot=a ;; esac
stage=0
crashes=0
switches=0
delay=1
while :; do
    started=$(monotonic_seconds) || started=0
    run_selected
    status=$?
    finished=$(monotonic_seconds) || finished=$started
    # Switch bounds apply to rapid failure loops, not a fourth legitimate
    # update after hours of otherwise healthy supervision.
    if [ "$finished" -ge "$started" ] && [ "$((finished - started))" -ge 60 ]; then
        switches=0
        delay=1
    fi
    case "$status" in
        0) ensure_root_ro || exit "$FATAL"; exit 0 ;;
        72)
            switches=$((switches + 1))
            if [ "$switches" -le "$SWITCH_LIMIT" ]; then
                install_prepared_slot
                status=$?
                [ "$status" -ne "$TRANSIENT" ] || switches=$((switches - 1))
                if [ "$status" -eq 0 ]; then
                    crashes=0
                    delay=1
                    continue
                fi
            else
                status=$FATAL
            fi
            ;;
    esac
    if [ "$status" -eq "$TRANSIENT" ]; then
        # Busy is not evidence of slot corruption. A prepare may contain four
        # 600s downloads and retries; wait without any failover deadline. The
        # background sleep is cancellable by the ordinary service stop trap.
        run_child sleep "$delay"
        increase_delay
        continue
    fi
    if [ "$status" -ne "$FATAL" ]; then
        crashes=$((crashes + 1))
        if [ "$crashes" -lt "$CRASH_LIMIT" ]; then
            run_child sleep "$delay"
            increase_delay
            continue
        fi
    fi
    crashes=0
    switches=0
    stage=$((stage + 1))
    case "$stage" in
        1) selection=$second_slot; slot=$selection; record_slot "$slot" >/dev/null 2>&1 || true ;;
        2) selection=previous ;;
        3) selection=current ;;
        *)
            run_child sleep 30
            stage=0
            selection=$first_slot
            slot=$selection
            ;;
    esac
    delay=1
done
