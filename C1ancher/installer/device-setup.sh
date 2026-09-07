#!/bin/sh
# Trusted-host installer helper. The host authenticates this script and payload
# before execution. No runtime alternate-root or test bypass is provided.
set -eu
PATH=/bin:/sbin:/usr/bin:/usr/sbin
export PATH
umask 077
DATA=/usr/data
STORAGE=/storage
FACTORY=/usr/bin/d261
INIT=/etc/init.d/S80app
DAEMON=/etc/app_daemon
PROFILE=/etc/profile
PROFILE_D=/etc/profile.d
STATE=/usr/data/c1/installer
LOCK=/usr/data/c1/installer.lock
MOUNTS=/proc/mounts
PROC=/proc
ROOT_MOUNT=/
FACTORY_INIT_SHA256=d35cdaa670636511c04d70b5e21b61e1938d2e93b33ae9379a01df1f340cafc2
FACTORY_DAEMON_SHA256=ceb56ddf2ff3c10f7c4c2cd6216b298da1cea799ca7170322f8229d5e9af6ee7
root_touched=0
locked=0
chain_stopped=0

fail() { echo "C1SETUP_ERROR $*" >&2; exit 1; }
hash_file() { sum=$(sha256sum "$1") || fail "hash-read-failed:$1"; printf '%s\n' "${sum%% *}"; }
# Reject symlinks in every ancestor, traversal, whitespace, and special files.
safe_path() {
    case "$1" in /*) ;; *) fail path-not-absolute ;; esac
    case "$1" in *[!A-Za-z0-9_./+-]*|*/../*|*/./*|*/..|*/.) fail unsafe-path ;; esac
    checked=$1
    while [ "$checked" != / ]; do
        [ ! -L "$checked" ] || fail "symlink:$checked"
        [ ! -e "$checked" ] || [ -f "$checked" ] || [ -d "$checked" ] || fail "special-file:$checked"
        checked=${checked%/*}; [ -n "$checked" ] || checked=/
    done
}
regular() {
    safe_path "$1"
    [ -f "$1" ] && [ "$(stat -c %h "$1")" = 1 ] || fail "not-single-link-file:$1"
}
require_hash() { regular "$1"; [ "$(hash_file "$1")" = "$2" ] || fail "hash-rejected:$1"; }
mount_has() {
    awk -v target="$1" -v flag="$2" '$2==target {n=split($4,a,","); for(i=1;i<=n;i++) if(a[i]==flag) ok=1} END {exit !ok}' "$MOUNTS"
}
root_ro() { mount_has "$ROOT_MOUNT" ro; }
remount() { /bin/mount -o "remount,$1" "$ROOT_MOUNT"; }
root_rw() {
    root_touched=1
    remount rw || fail root-remount-rw-failed
    mount_has "$ROOT_MOUNT" rw || fail root-not-rw
}
close_root() {
    sync
    remount ro || fail root-remount-ro-failed
    root_ro || fail root-not-ro
    root_touched=0
}
cleanup() {
    status=$?
    set +e
    trap - 0 1 2 15
    if [ "$root_touched" = 1 ]; then
        sync
        if ! remount ro || ! root_ro; then echo 'C1SETUP_ERROR root-remount-ro-failed' >&2; status=1; fi
    fi
    # Never start a chain while root is writable, including failure paths.
    if [ "$chain_stopped" = 1 ] && root_ro; then
        start_chain || { echo 'C1SETUP_ERROR chain-restart-failed' >&2; status=1; }
    fi
    if [ "$locked" = 1 ]; then rm -rf "$LOCK" || status=1; fi
    if [ "$status" = 0 ]; then echo "C1SETUP_OK $action";
    else echo "C1SETUP_ERROR command-failed:${action:-unknown}:exit-$status" >&2; fi
    exit "$status"
}
trap cleanup 0
trap 'exit 130' 2
trap 'exit 129' 1
trap 'exit 143' 15

# Do not traverse additional mounts when deleting the factory tree.
no_submounts() {
    awk -v p="$1" '$2==p || index($2,p"/")==1 {bad=1} END {exit bad}' "$MOUNTS" || fail nested-mount
}
# Filename-independent scan for the default no-backup operation.
# Physical traversal never follows symlinks; names are passed as argv, not
# parsed from text. Failed checks produce a nonempty rejection result.
scan_factory_for_removal() {
    safe_path "$FACTORY"
    no_submounts "$FACTORY"
    [ -e "$FACTORY" ] || return 0
    [ -d "$FACTORY" ] || fail factory-tree-not-directory
    # A nonempty result is enough to reject links/special files; filenames are
    # never parsed, so Chinese names, spaces and embedded newlines are safe.
    find "$FACTORY" ! -type d ! -type f -print -quit >"$LOCK/tree-unsafe" || fail tree-enumeration-failed
    [ ! -s "$LOCK/tree-unsafe" ] || fail tree-symlink-special-or-hardlink
    # BusyBox 1.36.1 may lose an earlier -exec {} + batch failure. Use the
    # per-file predicate instead: ! turns every failed check (including exec
    # failure) into a printed path and -quit. We test only whether output exists,
    # never parse filenames or rely on find propagating the child's exit code.
    find "$FACTORY" -type f ! -exec sh -c '
        links=$(stat -c %h "$1") || exit 1
        [ "$links" = 1 ]
    ' c1-link-check {} \; -print -quit >"$LOCK/tree-links" || fail tree-enumeration-failed
    [ ! -s "$LOCK/tree-links" ] || fail tree-symlink-special-or-hardlink
}
check_tree_scan_tools() {
    # Fail during preflight, not after core installation, if find/stat cannot
    # evaluate the needed predicates. Both positive and negative controls use
    # only our own fixed single-link flag, with no device application execution.
    find "$LOCK/flag" ! -type d ! -type f -print -quit >"$LOCK/tool-types" || fail tree-scan-tools-unsupported
    [ ! -s "$LOCK/tool-types" ] || fail tree-scan-tools-unsupported
    probe_links=$(stat -c %h "$LOCK/flag") || fail tree-scan-tools-unsupported
    [ "$probe_links" = 1 ] || fail tree-scan-tools-unsupported
    find "$LOCK/flag" ! -exec sh -c 'links=$(stat -c %h "$1") || exit 1; [ "$links" = 1 ]' c1-link-check {} \; -print -quit >"$LOCK/tool-positive" || fail tree-scan-tools-unsupported
    [ ! -s "$LOCK/tool-positive" ] || fail tree-scan-tools-unsupported
    probe_failure=$(find "$LOCK/flag" ! -exec false {} \; -print -quit) || fail tree-scan-tools-unsupported
    [ "$probe_failure" = "$LOCK/flag" ] || fail tree-scan-child-errors-not-detected
}
free_kb() { df -Pk "$1" | awk 'NR==2 {print $4}'; }
require_space() {
    available=$(free_kb "$1")
    case "$available" in ''|*[!0-9]*) fail invalid-free-space ;; esac
    [ "$available" -ge "$2" ] || fail "insufficient-space:$1"
}
copy_atomic() (
    source=$1; target=$2; mode=$3
    regular "$source"; safe_path "$target"; safe_path "$target.new"
    [ ! -e "$target.new" ] || fail interrupted-file-copy
    [ ! -e "$target" ] || regular "$target"
    cp "$source" "$target.new"
    chmod "$mode" "$target.new"
    chown 0:0 "$target.new"
    require_hash "$target.new" "$(hash_file "$source")"
    mv -f "$target.new" "$target"
)
write_flag() {
    safe_path "$STATE/$1"
    if [ -e "$STATE/$1" ]; then require_hash "$STATE/$1" "$(hash_file "$LOCK/flag")"; return; fi
    copy_atomic "$LOCK/flag" "$STATE/$1" 600
    sync
}

write_init() {
    cat >"$LOCK/S80app" <<'C1_INIT'
#!/bin/sh
# C1 installer startup: only the enrolled bootstrap owns restart policy.
find_bootstrap() {
    for entry in /proc/[0-9]*/cmdline; do
        [ -r "$entry" ] || continue
        command=$(tr '\000' ' ' <"$entry" 2>/dev/null) || continue
        case "$command" in
            '/bin/sh /etc/app_daemon '|'/etc/app_daemon ')
                pid=${entry#/proc/}; echo "${pid%/cmdline}" ;;
        esac
    done
}
case "${1:-}" in
    start)
        [ -n "$(find_bootstrap)" ] || /bin/busybox start-stop-daemon -S -b -x /etc/app_daemon
        ;;
    stop)
        for pid in $(find_bootstrap); do kill -TERM "$pid" || exit 1; done
        ;;
    restart|reload) "$0" stop && sleep 2 && "$0" start ;;
    *) exit 64 ;;
esac
C1_INIT
}
check_init() {
    regular "$INIT"
    init_hash=$(hash_file "$INIT")
    [ "$init_hash" = "$FACTORY_INIT_SHA256" ] || [ "$init_hash" = "$(hash_file "$LOCK/S80app")" ] || fail unknown-S80app-sha256
}
check_enrollment() {
    marker=$DATA/c1/update/enrolled.v1
    regular "$marker"
    [ "$(sed -n '1p' "$marker")" = 'C1CORE-ENROLLED 1' ] || fail enrollment-marker-invalid
    bootstrap_hash=$(sed -n 's/^bootstrap_sha256=//p' "$marker")
    verifier_hash=$(sed -n 's/^recovery_verifier_sha256=//p' "$marker")
    key_hash=$(sed -n 's/^key_sha256=//p' "$marker")
    for digest in "$bootstrap_hash" "$verifier_hash" "$key_hash"; do
        printf '%s\n' "$digest" | grep -Eq '^[0-9a-f]{64}$' || fail enrollment-hash-invalid
    done
    require_hash "$DAEMON" "$bootstrap_hash"
    require_hash /etc/c1updater/recovery-verifier "$verifier_hash"
    require_hash /etc/c1updater/core.ed25519.pub "$key_hash"
    regular /etc/c1updater/bootstrap.version
    [ "$(cat /etc/c1updater/bootstrap.version)" = "1.1.0 $bootstrap_hash" ] || fail bootstrap-version-invalid
    /etc/c1updater/recovery-verifier verify-current "$DATA/c1/core" /etc/c1updater/core.ed25519.pub >/dev/null || fail enrolled-core-invalid
    enrolled_state=$(/etc/c1updater/recovery-verifier state "$DATA/c1/update/state") || fail enrolled-state-invalid
    printf '%s\n' "$enrolled_state" | grep -Eq '(^|[[:space:]])phase=(confirmed|idle)([[:space:]]|$)' || fail enrolled-state-invalid
}
preflight() {
    [ "$(id -u)" = 0 ] || fail root-required
    root_ro || fail root-must-enter-read-only
    mount_has "$STORAGE" rw || fail storage-must-be-mounted-rw
    mount_has "$DATA" rw || fail data-must-be-mounted-rw
    for path in "$DATA" "$STORAGE" "$FACTORY" "$INIT" "$DAEMON" "$STATE" "$LOCK" "$PROFILE" "$PROFILE_D" "$DATA/c1/bin" "$DATA/c1/neofetch" "$STORAGE/mtp/pic" "$STORAGE/mtp/Pic" "$STORAGE/mtp/Music" "$STORAGE/mtp/Book"; do safe_path "$path"; done
    regular "$INIT"; regular "$DAEMON"; regular "$PROFILE"
    require_space "$DATA" 65536
    require_space "$STORAGE" 16384
    mkdir -p "$DATA/c1"
    mkdir "$LOCK" 2>/dev/null || fail installer-busy-or-stale-lock
    locked=1
    mkdir -p "$STATE"
    chmod 700 "$STATE" "$LOCK"
    printf 'C1SETUP 1\n' >"$LOCK/flag"
    check_tree_scan_tools
    write_init
    check_init
    if [ "$(hash_file "$DAEMON")" != "$FACTORY_DAEMON_SHA256" ]; then check_enrollment; fi
}

# Keep this hardware policy aligned with the runtime and default-app helper.
# This capability check is not proof of physical USB resume; release acceptance
# must validate the exact core shipped alongside this helper before delivery.
auto_suspend_supported() {
    [ -r /proc/cpuinfo ] &&
    awk -F ':' '$1 ~ /^[ \t]*machine[ \t]*$/ {
        value=$2; sub(/^[ \t]+/, "", value); sub(/[ \t]+$/, "", value)
        count++; if (NF != 2 || value != "ingenic,halley6_v20") bad=1
    } END { exit !(count == 1 && !bad) }' /proc/cpuinfo &&
    [ -d /sys/devices/platform/mpenbatt ] &&
    [ -r /sys/devices/platform/gpio_keys/power/wakeup ] &&
    [ "$(cat /sys/devices/platform/gpio_keys/power/wakeup)" = enabled ] &&
    [ -r /sys/power/state ] && [ -w /sys/power/state ] &&
    grep -Eq '(^|[[:space:]])mem([[:space:]]|$)' /sys/power/state
}

check_suspend_marker() {
    safe_path "$DATA/c1/disable-auto-suspend"
    # Marker contents have never had a format: empty, legacy and user-created
    # regular files are valid preferences. Links and special files are not.
    if [ -e "$DATA/c1/disable-auto-suspend" ]; then regular "$DATA/c1/disable-auto-suspend"; fi
}
suspend_core() {
    # Explicit GUI completion policy only. Bind the trusted current executable
    # to this host-validated payload; an older signed core is not sufficient.
    for digest in "$1" "$2"; do
        printf '%s\n' "$digest" | grep -Eq '^[0-9a-f]{64}$' || fail suspend-digest-invalid
    done
    check_suspend_marker
    if ! auto_suspend_supported; then
        # Fail closed even if capability changes after prepare. Never execute
        # an old or untrusted core merely to create the disabling preference.
        # Both actions fail closed on unsupported hardware. Verification may
        # disable for safety, but can never re-enable a lost preference.
        if [ ! -e "$DATA/c1/disable-auto-suspend" ]; then
            copy_atomic "$LOCK/flag" "$DATA/c1/disable-auto-suspend" 600
            sync || fail automatic-suspend-sync-failed
        fi
        fail automatic-suspend-unsupported-hardware
    fi
    safe_path "$DATA/c1/core"
    [ -L "$DATA/c1/core/current" ] || fail enrolled-core-pointer-invalid
    suspend_target=$(readlink "$DATA/c1/core/current") || fail enrolled-core-pointer-invalid
    # Match the recovery verifier's relative releases/<single-name> pointer.
    # Do not safe_path current itself: that one symlink is intentional.
    case "$suspend_target" in releases/*) ;; *) fail enrolled-core-pointer-invalid ;; esac
    suspend_release=${suspend_target#releases/}
    case "$suspend_release" in ''|.*|*.|*[!A-Za-z0-9_.+-]*) fail enrolled-core-pointer-invalid ;; esac
    suspend_pkg=$DATA/c1/core/$suspend_target/artifacts/c1pkg
    regular "$suspend_pkg"
    [ -x "$suspend_pkg" ] || fail enrolled-c1pkg-not-executable
    check_enrollment
    suspend_manifest=$DATA/c1/core/$suspend_target/manifest.v1
    require_hash "$suspend_manifest" "$1"
    require_hash "$suspend_pkg" "$2"
    suspend_sequence=$(sed -n '2s/^S\t//p' "$suspend_manifest")
    suspend_version=$(sed -n '3s/^V\t//p' "$suspend_manifest")
    suspend_epoch=$(sed -n '4s/^E\t//p' "$suspend_manifest")
    # idle, pending and merely prefix-matching phases are never healthy enough
    # for this new preference policy. State identity must match the manifest.
    printf '%s\n' "$enrolled_state" | grep -Eq '^state: generation=[1-9][0-9]* phase=confirmed sequence=[1-9][0-9]* security_epoch=[1-9][0-9]* release=[^[:space:]]+$' || fail suspend-core-not-confirmed
    case "$enrolled_state" in
        "state: generation="*" phase=confirmed sequence=$suspend_sequence security_epoch=$suspend_epoch release=$suspend_version") ;;
        *) fail suspend-state-identity-mismatch ;;
    esac
    [ "$(readlink "$DATA/c1/core/current")" = "$suspend_target" ] || fail enrolled-core-pointer-changed
    wait_bootstrap || fail bootstrap-not-running
    check_suspend_marker
}
check_suspend_enabled() {
    # Compare the complete byte stream, not a substring or a command
    # substitution (which would discard trailing newlines and possibly NULs).
    "$suspend_pkg" power status >"$LOCK/suspend-status" || fail automatic-suspend-status-failed
    printf 'automatic suspend: enabled\n' >"$LOCK/suspend-expected"
    require_hash "$LOCK/suspend-status" "$(hash_file "$LOCK/suspend-expected")"
    check_suspend_marker
    [ ! -e "$DATA/c1/disable-auto-suspend" ] || fail automatic-suspend-still-disabled
    echo 'automatic_suspend=enabled verified=1'
}
enable_suspend() {
    suspend_core "$1" "$2"
    "$suspend_pkg" power enable >/dev/null || fail automatic-suspend-enable-failed
    sync || fail automatic-suspend-sync-failed
    check_suspend_enabled
}
verify_suspend() {
    # Never re-enable during verification; unsupported hardware may be disabled.
    suspend_core "$1" "$2"
    check_suspend_enabled
}

prepare() {
    # Staging only: preserve existing preferences until the authenticated,
    # running core is explicitly enabled by the installer's enable-suspend step.
    # The public media layout is available before any app is launched.
    mkdir -p "$STORAGE/mtp/Pic" "$STORAGE/mtp/Music" "$STORAGE/mtp/Book"
    safe_path "$DATA/c1/disable-auto-suspend"
    # Preserve every existing marker, including old C1SETUP and empty markers.
    # Its contents cannot prove whether a user subsequently chose disable.
    if [ -e "$DATA/c1/disable-auto-suspend" ]; then regular "$DATA/c1/disable-auto-suspend"; fi
    if [ -e "$STATE/prepared" ]; then require_hash "$STATE/prepared" "$(hash_file "$LOCK/flag")"; fi
    if ! auto_suspend_supported; then
        # Fail closed even on retries or upgrades with an absent disable marker.
        if [ ! -e "$DATA/c1/disable-auto-suspend" ]; then
            copy_atomic "$LOCK/flag" "$DATA/c1/disable-auto-suspend" 600
        fi
        echo 'automatic_suspend=disabled default=unsupported-hardware'
    elif [ ! -e "$STATE/prepared" ] && [ ! -e "$DATA/c1/update/enrolled.v1" ] &&
         [ ! -e "$DATA/c1/enabled" ] && [ ! -e "$DATA/c1/disable-auto-suspend" ]; then
        echo 'automatic_suspend=enabled default=supported-hardware'
    else
        echo 'automatic_suspend=preserved'
    fi
    write_flag prepared
}
bootstrap_pids() {
    for entry in "$PROC"/[0-9]*/cmdline; do
        [ -r "$entry" ] || continue
        command=$(tr '\000' ' ' <"$entry" 2>/dev/null) || continue
        case "$command" in '/bin/sh /etc/app_daemon '|'/etc/app_daemon ')
            pid=${entry%/cmdline}; echo "${pid##*/}" ;; esac
    done
}
stop_chain() {
    chain_stopped=1
    for pid in $(bootstrap_pids); do kill -TERM "$pid" || fail stop-bootstrap-failed; done
    count=0
    while [ -n "$(bootstrap_pids)" ] && [ "$count" -lt 20 ]; do sleep 1; count=$((count + 1)); done
    [ -z "$(bootstrap_pids)" ] || fail bootstrap-did-not-stop
    # Only stop processes whose executable is inside the exact vendor directory.
    for entry in "$PROC"/[0-9]*/exe; do
        resolved=$(readlink "$entry" 2>/dev/null || true)
        case "$resolved" in "$FACTORY"/*)
            pid=${entry%/exe}; pid=${pid##*/}
            kill -TERM "$pid" 2>/dev/null || true
            count=0
            while kill -0 "$pid" 2>/dev/null && [ "$count" -lt 10 ]; do sleep 1; count=$((count + 1)); done
            kill -0 "$pid" 2>/dev/null && fail factory-process-did-not-stop
            ;;
        esac
    done
    return 0
}
wait_bootstrap() {
    startup_wait=0
    while [ -z "$(bootstrap_pids)" ] && [ "$startup_wait" -lt 20 ]; do
        sleep 1
        startup_wait=$((startup_wait + 1))
    done
    [ -n "$(bootstrap_pids)" ]
}
start_chain() {
    root_ro || return 1
    [ -n "$(bootstrap_pids)" ] || /bin/busybox start-stop-daemon -S -b -x "$DAEMON" || return 1
    # BusyBox -b acknowledges backgrounding, not startup readiness.
    wait_bootstrap || return 1
    chain_stopped=0
}
start_core() {
    # User-triggered installer retry may find a confirmed core stopped. Validate
    # it before starting; do not reinstall generations or alter removal state.
    check_enrollment
    start_chain || fail bootstrap-start-failed
}
remove_factory() {
    # The signed core must be enrolled before direct factory removal.
    # Historical backups are neither required, changed nor discarded.
    check_init
    check_enrollment
    safe_path "$STATE/restored"
    [ ! -e "$STATE/restored" ] || fail factory-restored-use-a-new-reviewed-installation
    for flag in no-factory-backup removing removed; do
        safe_path "$STATE/$flag"
        if [ -e "$STATE/$flag" ]; then require_hash "$STATE/$flag" "$(hash_file "$LOCK/flag")"; fi
    done
    # A missing/partially removed tree with replacement init is accepted only
    # after our persistent, validated transaction markers have been written.
    if [ ! -e "$FACTORY" ] || [ "$(hash_file "$INIT")" = "$(hash_file "$LOCK/S80app")" ]; then
        require_hash "$STATE/no-factory-backup" "$(hash_file "$LOCK/flag")"
        require_hash "$STATE/removing" "$(hash_file "$LOCK/flag")"
    fi
    scan_factory_for_removal
    if [ ! -e "$FACTORY" ] && [ "$(hash_file "$INIT")" = "$(hash_file "$LOCK/S80app")" ]; then
        write_flag removed
        start_chain
        return
    fi
    stop_chain
    # Recheck all deletion boundaries after the process chain has stopped.
    check_init
    check_enrollment
    scan_factory_for_removal
    write_flag no-factory-backup
    write_flag removing
    root_rw
    copy_atomic "$LOCK/S80app" "$INIT" 755
    if [ -e "$FACTORY" ]; then rm -rf "$FACTORY"; fi
    [ ! -e "$FACTORY" ] || fail factory-removal-incomplete
    close_root
    write_flag removed
    start_chain
}

payload_check() (
    payload=$1
    safe_path "$payload"; [ -d "$payload" ] || fail payload-missing
    regular "$payload/SHA256SUMS"
    [ "$(wc -l <"$payload/SHA256SUMS")" = 6 ] || fail payload-manifest-line-count
    for name in neofetch neofetch.upstream c1-config.conf c1-logo.txt LICENSE.md wallpaper.raw; do
        digest=$(awk -v name="$name" '$0==$1"  "name {print $1}' "$payload/SHA256SUMS")
        printf '%s\n' "$digest" | grep -Eq '^[0-9a-f]{64}$' || fail payload-manifest-invalid
        require_hash "$payload/$name" "$digest"
    done
    [ "$(wc -c <"$payload/wallpaper.raw")" = 5624 ] || fail wallpaper-size-invalid
)
write_profile() {
    cat >"$LOCK/c1-path.sh" <<'C1_PROFILE'
# Installed by C1 device-setup. Preserve the caller's other PATH entries.
case ":$PATH:" in
    *:/usr/data/c1/bin:*) ;;
    *) PATH="/usr/data/c1/bin:$PATH" ;;
esac
export PATH
C1_PROFILE
}
accessories() {
    payload=$1
    payload_check "$payload"
    prepare
    [ -x /bin/bash ] || fail neofetch-requires-bash
    # Factory profile explicitly sources profile.d; never rewrite user profile.
    grep -Fq 'for i in /etc/profile.d/*.sh' "$PROFILE" || fail profile-does-not-source-profile-d
    grep -Fq '. $i' "$PROFILE" || fail profile-does-not-source-profile-d
    write_profile
    safe_path "$PROFILE_D/90-c1-path.sh"
    if [ -e "$PROFILE_D/90-c1-path.sh" ]; then require_hash "$PROFILE_D/90-c1-path.sh" "$(hash_file "$LOCK/c1-path.sh")"; fi
    mkdir -p "$DATA/c1/bin" "$DATA/c1/neofetch"
    copy_atomic "$payload/neofetch" "$DATA/c1/bin/neofetch" 700
    copy_atomic "$payload/neofetch.upstream" "$DATA/c1/neofetch/neofetch.upstream" 700
    for name in c1-config.conf c1-logo.txt LICENSE.md; do copy_atomic "$payload/$name" "$DATA/c1/neofetch/$name" 644; done
    safe_path "$STORAGE/mtp/pic/wallpaper.raw"
    safe_path "$STORAGE/mtp/Pic/wallpaper.raw"
    if [ ! -e "$STATE/wallpaper-handled" ]; then
        write_flag wallpaper-handled
    else require_hash "$STATE/wallpaper-handled" "$(hash_file "$LOCK/flag")"; fi
    if [ -e "$STORAGE/mtp/Pic/wallpaper.raw" ]; then
        regular "$STORAGE/mtp/Pic/wallpaper.raw"
    elif [ -e "$STORAGE/mtp/pic/wallpaper.raw" ]; then
        regular "$STORAGE/mtp/pic/wallpaper.raw"
        copy_atomic "$STORAGE/mtp/pic/wallpaper.raw" "$STORAGE/mtp/Pic/wallpaper.raw" 644
    else
        copy_atomic "$payload/wallpaper.raw" "$STORAGE/mtp/Pic/wallpaper.raw" 644
    fi
    if [ ! -e "$PROFILE_D/90-c1-path.sh" ]; then
        root_rw
        mkdir -p "$PROFILE_D"
        copy_atomic "$LOCK/c1-path.sh" "$PROFILE_D/90-c1-path.sh" 644
        close_root
    fi
    copy_atomic "$payload/SHA256SUMS" "$STATE/accessories.sha256" 600
    write_flag accessories
}
verify() {
    require_hash "$STATE/no-factory-backup" "$(hash_file "$LOCK/flag")"
    require_hash "$STATE/removing" "$(hash_file "$LOCK/flag")"
    check_enrollment
    require_hash "$STATE/removed" "$(hash_file "$LOCK/flag")"
    safe_path "$STATE/restored"
    [ ! -e "$STATE/restored" ] || fail factory-was-restored
    [ ! -e "$FACTORY" ] || fail factory-still-present
    require_hash "$INIT" "$(hash_file "$LOCK/S80app")"
    require_hash "$STATE/prepared" "$(hash_file "$LOCK/flag")"
    require_hash "$STATE/accessories" "$(hash_file "$LOCK/flag")"
    require_hash "$STATE/wallpaper-handled" "$(hash_file "$LOCK/flag")"
    regular "$STATE/accessories.sha256"
    for name in neofetch neofetch.upstream c1-config.conf c1-logo.txt LICENSE.md; do
        digest=$(awk -v name="$name" '$0==$1"  "name {print $1}' "$STATE/accessories.sha256")
        case "$name" in neofetch) target=$DATA/c1/bin/neofetch ;; *) target=$DATA/c1/neofetch/$name ;; esac
        require_hash "$target" "$digest"
    done
    write_profile
    require_hash "$PROFILE_D/90-c1-path.sh" "$(hash_file "$LOCK/c1-path.sh")"
    wait_bootstrap || fail bootstrap-not-running
}
action=${1:-}
case "$action" in
    preflight|prepare|start-core|remove-factory|verify) [ "$#" = 1 ] || fail usage ;;
    enable-suspend|verify-suspend) [ "$#" = 3 ] || fail usage ;;
    accessories) [ "$#" = 2 ] || fail usage ;;
    *) fail usage ;;
esac
preflight
case "$action" in
    preflight) ;;
    prepare) prepare ;;
    start-core) start_core ;;
    enable-suspend) enable_suspend "$2" "$3" ;;
    verify-suspend) verify_suspend "$2" "$3" ;;
    remove-factory) remove_factory ;;
    accessories) accessories "$2" ;;
    verify) verify ;;
esac
root_ro || fail root-must-exit-read-only
