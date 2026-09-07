#!/bin/sh
set -eu

storage_root=/storage/c1/update/enrollment
core_root=/usr/data/c1/core
state_root=/usr/data/c1/update/state
staging_root=/storage/c1/update/staging
slot_root=/etc/c1updater
key_target=$slot_root/core.ed25519.pub
slot_file=/usr/data/c1/update/updater-slot
update_root=/usr/data/c1/update
marker=/usr/data/c1/update/enrolled.v1
root_target=/etc/app_daemon
data_recovery=/usr/data/c1/recovery/core-enrollment
storage_recovery=/storage/c1/recovery/core-enrollment
bin_root=/usr/data/c1/bin
root_writable=0
chain_stopped=0

fail() {
    echo "core enrollment failed: $1" >&2
    exit 1
}

hash_file() {
    sha256sum "$1" | while IFS=' ' read -r digest rest; do
        printf '%s\n' "$digest"
        break
    done
}

require_hash() {
    [ -f "$1" ] && [ ! -L "$1" ] || fail "missing regular file: $1"
    actual=$(hash_file "$1")
    [ "$actual" = "$2" ] || fail "file digest rejected: $1"
}

require_root_metadata() {
    path=$1
    permissions=$2
    metadata=$(ls -ldn "$path")
    set -- $metadata
    [ "$1" = "$permissions" ] && [ "$3" = 0 ] && [ "$4" = 0 ] ||
        fail "file ownership or mode rejected: $path"
}

manifest_value() {
    line=$1
    tag=$2
    value=$(sed -n "${line}s/^${tag}$(printf '\t')//p" "$bundle/bootstrap.v1")
    echo "$value" | grep -Eq '^[0-9a-f]{64}$' || fail "bootstrap manifest field rejected: $tag"
    printf '%s' "$value"
}

root_is_read_only() {
    while IFS=' ' read -r device mountpoint filesystem options rest; do
        [ "$mountpoint" = / ] || continue
        case ",$options," in *,ro,*) return 0 ;; *) return 1 ;; esac
    done </proc/mounts
    return 1
}

remount_root_rw() {
    /bin/mount -o remount,rw / 2>/dev/null || true
    root_is_read_only && fail 'root filesystem did not become writable'
    root_writable=1
}

remount_root_ro() {
    [ "$root_writable" -eq 1 ] || return 0
    sync
    /bin/mount -o remount,ro / 2>/dev/null || true
    root_is_read_only || fail 'root filesystem did not return to read-only'
    root_writable=0
}

startup_script_pids() {
    # Script executables resolve to BusyBox in /proc/*/exe. Match only these
    # fixed, argument-free entry points, never arbitrary shells or applications.
    for entry in /proc/[0-9]*/cmdline; do
        [ -r "$entry" ] || continue
        command=$(tr '\000' ' ' <"$entry" 2>/dev/null) || continue
        case "$command" in
            '/bin/sh /etc/app_daemon '|'/etc/app_daemon '|\
            '/bin/sh /etc/c1updater/enrollment-startup '|'/etc/c1updater/enrollment-startup ')
                process=${entry%/cmdline}
                printf '%s\n' "${process##*/}"
                ;;
        esac
    done
}

app_daemon_running() {
    [ -z "$(startup_script_pids)" ] || return 0
    for executable in /proc/[0-9]*/exe; do
        [ -L "$executable" ] || continue
        resolved=$(readlink "$executable" 2>/dev/null || true)
        case "$resolved" in
            /usr/bin/d261/mpenMain|\
            /usr/data/c1/bin/app_daemon|\
            "$core_root"/releases/*/artifacts/C1ancher|\
            "$core_root"/releases/*/artifacts/c1pkg|\
            "$core_root"/releases/*/artifacts/C1ancher-launcher|\
            "$slot_root"/recovery-verifier|\
            "$slot_root"/slot-a/c1updater|\
            "$slot_root"/slot-b/c1updater|\
            "$core_root"/releases/*/artifacts/c1updater) return 0 ;;
        esac
    done
    return 1
}

stop_chain() {
    chain_stopped=1
    # Give the bootstrap a chance to reap its temporary startup child before
    # the factory init script uses SIGKILL on the legacy process names.
    for process in $(startup_script_pids); do
        kill -TERM "$process" 2>/dev/null || true
    done
    count=0
    # Legacy run-logged can consume its full 6s TERM + 2s KILL budget on
    # kernels without proc task children. Wait with margin, still fail closed
    # before any root-side write if the old bootstrap has not exited.
    while [ "$count" -lt 20 ] && [ -n "$(startup_script_pids)" ]; do sleep 1; count=$((count + 1)); done
    [ -z "$(startup_script_pids)" ] || fail 'startup scripts did not stop'
    /etc/init.d/S80app stop 2>/dev/null || true
    count=0
    while [ "$count" -lt 20 ] && app_daemon_running; do sleep 1; count=$((count + 1)); done
    app_daemon_running && fail 'application chain did not stop'
    return 0
}

start_chain() {
    if ! app_daemon_running; then
        /bin/busybox start-stop-daemon -S -b -x /etc/app_daemon
    fi
    chain_stopped=0
}

cleanup() {
    status=$?
    trap - 0 1 2 15
    if ! root_is_read_only; then root_writable=1; fi
    [ "$root_writable" -eq 0 ] || remount_root_ro || true
    if [ "$chain_stopped" -eq 1 ]; then start_chain || true; fi
    exit "$status"
}
trap cleanup 0 1 2 15

verify_file_set() {
    [ -d "$bundle" ] && [ ! -L "$bundle" ] || fail 'bundle root rejected'
    [ "$(find "$bundle" -type f | wc -l)" -eq 13 ] || fail 'bundle file count rejected'
    [ "$(find "$bundle" -type d | wc -l)" -eq 3 ] || fail 'bundle directory count rejected'
    [ -z "$(find "$bundle" ! -type f ! -type d -print -quit)" ] || fail 'bundle contains links or special files'
    for relative in \
        bootstrap.v1 bootstrap.v1.sig core.ed25519.pem core.ed25519.pub \
        app-daemon-bootstrap.sh device-core-enroll.sh enroll.sh \
        release/manifest.v1 release/manifest.v1.sig \
        release/artifacts/C1ancher release/artifacts/c1pkg \
        release/artifacts/C1ancher-launcher release/artifacts/c1updater; do
        [ -f "$bundle/$relative" ] && [ ! -L "$bundle/$relative" ] || fail "bundle member missing: $relative"
    done
}

verify_bundle() {
    bundle=$1
    verify_file_set
    [ "$(wc -c <"$bundle/bootstrap.v1.sig")" -eq 64 ] || fail 'bootstrap signature size rejected'
    [ "$(wc -c <"$bundle/core.ed25519.pub")" -eq 32 ] || fail 'core public key size rejected'
    # On enrolled devices use the existing protected verifier, not bundle code,
    # to authenticate the maintenance payload against the unchanged trust key.
    if [ -f "$key_target" ]; then
        [ "$(hash_file "$key_target")" = "$(hash_file "$bundle/core.ed25519.pub")" ] || fail 'maintenance cannot replace the trust key'
        trusted_verifier=$slot_root/recovery-verifier
        [ -x "$trusted_verifier" ] || trusted_verifier=$slot_root/slot-a/c1updater
        [ -x "$trusted_verifier" ] || trusted_verifier=$slot_root/slot-b/c1updater
        "$trusted_verifier" verify-signature "$bundle/bootstrap.v1" "$bundle/bootstrap.v1.sig" \
            "$key_target" >/dev/null || fail 'trusted maintenance signature rejected'
    fi
    # Initial enrollment is a trusted-host maintenance operation: the host must
    # independently verify bootstrap signature AND every signed member hash
    # before this script (or its bundled verifier) is allowed to execute.
    verifier=/dev/shm/c1-enrollment-verifier.$$
    cp "$bundle/release/artifacts/c1updater" "$verifier"
    chmod 700 "$verifier"
    "$verifier" --self-test || fail 'bootstrap verifier self-test failed'
    "$verifier" verify-signature "$bundle/bootstrap.v1" "$bundle/bootstrap.v1.sig" \
        "$bundle/core.ed25519.pub" >/dev/null || fail 'bootstrap signature rejected'
    [ "$(sed -n '1p' "$bundle/bootstrap.v1")" = 'C1CORE-BOOTSTRAP 1' ] || fail 'bootstrap header rejected'
    [ "$(sed -n '2p' "$bundle/bootstrap.v1")" = "V$(printf '\t')1.1.0" ] || fail 'bootstrap version rejected'
    lines=$(wc -l <"$bundle/bootstrap.v1")
    [ "$lines" -ge 11 ] && [ "$lines" -le 26 ] || fail 'bootstrap record count rejected'
    [ -z "$(sed -n '11,$p' "$bundle/bootstrap.v1" | grep -Ev "^H$(printf '\t')[0-9a-f]{64}$" || true)" ] || fail 'bootstrap baseline records rejected'

    key_hash=$(manifest_value 3 K)
    pem_hash=$(manifest_value 4 P)
    bootstrap_hash=$(manifest_value 5 B)
    device_hash=$(manifest_value 6 D)
    launch_hash=$(manifest_value 7 L)
    manifest_hash=$(manifest_value 8 M)
    signature_hash=$(manifest_value 9 G)
    updater_hash=$(manifest_value 10 U)
    require_hash "$bundle/core.ed25519.pub" "$key_hash"
    require_hash "$bundle/core.ed25519.pem" "$pem_hash"
    require_hash "$bundle/app-daemon-bootstrap.sh" "$bootstrap_hash"
    require_hash "$bundle/device-core-enroll.sh" "$device_hash"
    require_hash "$bundle/enroll.sh" "$launch_hash"
    require_hash "$bundle/release/manifest.v1" "$manifest_hash"
    require_hash "$bundle/release/manifest.v1.sig" "$signature_hash"
    require_hash "$bundle/release/artifacts/c1updater" "$updater_hash"
    "$verifier" verify-manifest "$bundle/release/manifest.v1" \
        "$bundle/release/manifest.v1.sig" "$bundle/core.ed25519.pub" >/dev/null ||
        fail 'core release manifest rejected'
    rm -f "$verifier"
}

baseline_allowed() {
    value=$1
    [ "$value" = "$bootstrap_hash" ] && return 0
    sed -n '11,$s/^H	//p' "$bundle/bootstrap.v1" | grep -Fqx "$value"
}

copy_atomic() {
    source=$1
    destination=$2
    expected=$3
    mode=$4
    temporary=$destination.new.$$
    require_hash "$source" "$expected"
    rm -f "$temporary"
    cp "$source" "$temporary"
    chmod "$mode" "$temporary"
    chown 0:0 "$temporary"
    require_hash "$temporary" "$expected"
    mv -f "$temporary" "$destination"
}

backup_once() {
    source=$1
    destination=$2
    expected=$3
    if [ -e "$destination" ]; then require_hash "$destination" "$expected"; return; fi
    copy_atomic "$source" "$destination" "$expected" 700
}

# Temporary authorization for the approved old startup only. This is not a
# factory-application backup and is never used after an enrollment marker exists.
verify_enrollment_startup() {
    startup=$slot_root/enrollment-startup
    startup_record=$slot_root/enrollment-startup.sha256
    [ -d "$slot_root" ] && [ ! -L "$slot_root" ] || fail 'startup recovery directory rejected'
    [ "$(stat -c '%a:%u:%g' "$slot_root")" = 700:0:0 ] || fail 'startup recovery directory metadata rejected'
    [ -f "$startup" ] && [ ! -L "$startup" ] &&
        [ "$(stat -c '%a:%u:%g:%h' "$startup")" = 700:0:0:1 ] || fail 'startup recovery metadata rejected'
    [ -f "$startup_record" ] && [ ! -L "$startup_record" ] &&
        [ "$(stat -c '%a:%u:%g:%h:%s' "$startup_record")" = 600:0:0:1:65 ] || fail 'startup recovery record rejected'
    startup_hash=$(cat "$startup_record")
    echo "$startup_hash" | grep -Eq '^[0-9a-f]{64}$' || fail 'startup recovery digest rejected'
    # It must be an explicitly signed old baseline, not the new bootstrap.
    sed -n '11,$s/^H\t//p' "$bundle/bootstrap.v1" | grep -Fqx "$startup_hash" ||
        fail 'startup recovery baseline not approved'
    require_hash "$startup" "$startup_hash"
}

install_enrollment_startup() {
    [ ! -e "$marker" ] && [ ! -L "$marker" ] || fail 'startup recovery forbidden after enrollment'
    if [ "$current_hash" = "$bootstrap_hash" ]; then
        verify_enrollment_startup
        return
    fi
    copy_atomic "$root_target" "$slot_root/enrollment-startup" "$current_hash" 700
    printf '%s\n' "$current_hash" >"$slot_root/enrollment-startup.sha256.new.$$"
    chmod 600 "$slot_root/enrollment-startup.sha256.new.$$"
    chown 0:0 "$slot_root/enrollment-startup.sha256.new.$$"
    mv -f "$slot_root/enrollment-startup.sha256.new.$$" "$slot_root/enrollment-startup.sha256"
    verify_enrollment_startup
    # Make recovery durable BEFORE replacing the only startup entry point.
    sync
}

replace_compatibility_link() {
    name=$1
    target=$2
    path=$bin_root/$name
    temporary=$bin_root/.$name.new.$$
    rm -f "$temporary"
    ln -s "$target" "$temporary"
    mv -f "$temporary" "$path"
}

stage_bundle() {
    source=$1
    apply_kind=${2:-apply}
    verify_bundle "$source"
    mkdir -p "$storage_root"
    chmod 700 "$storage_root"
    destination=$storage_root/bundle
    draft=$storage_root/.bundle.new.$$
    rm -rf "$draft"
    mkdir -p "$draft/release/artifacts"
    chmod 700 "$draft" "$draft/release" "$draft/release/artifacts"
    for relative in \
        bootstrap.v1 bootstrap.v1.sig core.ed25519.pem core.ed25519.pub \
        app-daemon-bootstrap.sh device-core-enroll.sh enroll.sh \
        release/manifest.v1 release/manifest.v1.sig \
        release/artifacts/C1ancher release/artifacts/c1pkg \
        release/artifacts/C1ancher-launcher release/artifacts/c1updater; do
        cp "$source/$relative" "$draft/$relative"
        chmod 600 "$draft/$relative"
    done
    chmod 700 "$draft/device-core-enroll.sh" "$draft/enroll.sh"
    sync
    rm -rf "$destination.old"
    [ ! -e "$destination" ] || mv "$destination" "$destination.old"
    mv "$draft" "$destination"
    sync
    rm -rf "$destination.old"
    verify_bundle "$destination"
    apply_log=$storage_root/apply.log
    apply_pid=$storage_root/apply.pid
    if [ -e "$apply_pid" ] || [ -L "$apply_pid" ]; then
        [ -f "$apply_pid" ] && [ ! -L "$apply_pid" ] || fail 'apply pid metadata rejected'
        apply_process=$(sed -n '1p' "$apply_pid")
        case "$apply_process" in ''|*[!0-9]*) fail 'apply pid rejected' ;; esac
        kill -0 "$apply_process" 2>/dev/null && fail 'core enrollment is already applying'
        rm -f "$apply_pid"
    fi
    : >"$apply_log"
    chmod 600 "$apply_log"
    /bin/busybox start-stop-daemon -S -b -m -p "$apply_pid" -x /bin/sh -- -c \
        "'$destination/device-core-enroll.sh' '$apply_kind-logged' '$destination' '$apply_log'; status=\$?; rm -f '$apply_pid'; exit \$status"
    echo 'core enrollment accepted; installation continues in background'
}

verify_activation_identity() {
    expected_phase=$1
    release_sequence=$(sed -n '2s/^S\t//p' "$bundle/release/manifest.v1")
    release_version=$(sed -n '3s/^V\t//p' "$bundle/release/manifest.v1")
    release_epoch=$(sed -n '4s/^E\t//p' "$bundle/release/manifest.v1")
    expected_candidate=releases/$release_sequence-$release_version
    observed_state=$("$slot_root/recovery-verifier" state "$state_root") || fail 'activation state rejected'
    case "$observed_state" in
        "state: generation="*" phase=$expected_phase sequence=$release_sequence security_epoch=$release_epoch release=$release_version") ;;
        *) fail 'activation identity differs from signed enrollment' ;;
    esac
    require_hash "$core_root/$expected_candidate/manifest.v1" "$manifest_hash"
    for pointer in current previous; do
        if [ -e "$core_root/$pointer" ] || [ -L "$core_root/$pointer" ]; then
            [ -L "$core_root/$pointer" ] &&
                [ "$(readlink "$core_root/$pointer")" = "$expected_candidate" ] || fail 'activation pointer identity rejected'
        elif [ "$expected_phase" = confirmed ]; then
            fail 'confirmed activation pointer is missing'
        fi
    done
}

apply_bundle() {
    bundle=$1
    verify_bundle "$bundle"
    current_hash=$(hash_file "$root_target")
    baseline_allowed "$current_hash" || fail "device baseline not approved: $current_hash"
    already_active=0
    if [ "$current_hash" = "$bootstrap_hash" ] && [ -x "$slot_root/slot-a/c1updater" ] &&
       [ -L "$core_root/current" ] && [ -L "$core_root/previous" ] &&
       "$slot_root/slot-a/c1updater" state "$state_root" 2>/dev/null | grep -q 'phase=confirmed' &&
       "$slot_root/slot-a/c1updater" verify-manifest "$core_root/current/manifest.v1" \
           "$core_root/current/manifest.v1.sig" "$key_target" >/dev/null 2>&1; then
        already_active=1
    fi
    resume_activation=0
    if [ "$already_active" -eq 0 ] && [ "$current_hash" = "$bootstrap_hash" ] &&
       [ ! -e "$marker" ] && [ ! -L "$marker" ]; then
        verify_enrollment_startup
        if "$slot_root/recovery-verifier" state "$state_root" | grep -q 'phase=pending-boot '; then
            # bootstrap-activate itself validates the signed candidate and each
            # existing pointer. Do not call prepare-local in PENDING_BOOT.
            verify_activation_identity pending-boot
            resume_activation=1
        fi
    fi
    if [ "$already_active" -eq 1 ]; then verify_activation_identity confirmed; fi
    for pointer in current previous; do
        if [ -e "$core_root/$pointer" ] || [ -L "$core_root/$pointer" ]; then
            [ "$already_active" -eq 1 ] || [ "$resume_activation" -eq 1 ] ||
                fail 'existing enrollment requires trusted maintenance, not bootstrap activation'
        fi
    done
    if [ -L "$core_root/current" ]; then
        require_hash "$core_root/current/manifest.v1" "$manifest_hash"
    fi
    mkdir -p "$data_recovery/bin" "$storage_recovery/bin" "$bin_root" \
        "$core_root" "$state_root" "$staging_root" /usr/data/c1/update
    chmod 700 "$data_recovery" "$data_recovery/bin" "$storage_recovery" \
        "$storage_recovery/bin" "$bin_root" "$core_root" "$update_root" "$state_root" "$staging_root"
    chown 0:0 "$data_recovery" "$data_recovery/bin" "$storage_recovery" \
        "$storage_recovery/bin" "$bin_root" "$core_root" "$update_root" "$state_root" "$staging_root"

    if [ "$current_hash" != "$bootstrap_hash" ]; then
        recovery_hash=$current_hash
        backup_once "$root_target" "$data_recovery/app_daemon.pre-enrollment" "$recovery_hash"
        backup_once "$root_target" "$storage_recovery/app_daemon.pre-enrollment" "$recovery_hash"
        for name in C1ancher c1pkg app_daemon c1updater; do
            path=$bin_root/$name
            [ -f "$path" ] && [ ! -L "$path" ] || continue
            old_hash=$(hash_file "$path")
            backup_once "$path" "$data_recovery/bin/$name" "$old_hash"
            backup_once "$path" "$storage_recovery/bin/$name" "$old_hash"
        done
    fi

    verifier=/dev/shm/c1-enrollment-apply.$$
    cp "$bundle/release/artifacts/c1updater" "$verifier"
    chmod 700 "$verifier"
    # Compatibility is checked against the ACTUAL installed bootstrap below,
    # never a spoofed capability, altered release minimum, or verifier bypass.
    stop_chain
    remount_root_rw
    mkdir -p "$slot_root/slot-a" "$slot_root/slot-b"
    chmod 700 "$slot_root" "$slot_root/slot-a" "$slot_root/slot-b"
    chown 0:0 "$slot_root" "$slot_root/slot-a" "$slot_root/slot-b"
    copy_atomic "$bundle/release/artifacts/c1updater" "$slot_root/slot-a/c1updater" "$updater_hash" 700
    copy_atomic "$bundle/release/artifacts/c1updater" "$slot_root/slot-b/c1updater" "$updater_hash" 700
    "$slot_root/slot-a/c1updater" --self-test
    "$slot_root/slot-b/c1updater" --self-test
    copy_atomic "$bundle/release/artifacts/c1updater" "$slot_root/recovery-verifier" "$updater_hash" 700
    [ "$("$slot_root/recovery-verifier" --recovery-version)" = 'C1RECOVERY-VERIFIER 1.1.0' ] || fail 'recovery verifier capability rejected'
    # Existing-key verification may never fall back to unauthenticated bundle
    # code. Make a working protected verifier durable before installing the key.
    sync
    copy_atomic "$bundle/core.ed25519.pub" "$key_target" "$key_hash" 600
    if [ "$already_active" -eq 0 ]; then install_enrollment_startup; fi
    copy_atomic "$bundle/app-daemon-bootstrap.sh" "$root_target" "$bootstrap_hash" 755
    printf '1.1.0 %s\n' "$bootstrap_hash" >"$slot_root/bootstrap.version.new.$$"
    chmod 600 "$slot_root/bootstrap.version.new.$$"
    chown 0:0 "$slot_root/bootstrap.version.new.$$"
    mv -f "$slot_root/bootstrap.version.new.$$" "$slot_root/bootstrap.version"
    remount_root_ro

    if [ "$already_active" -eq 0 ] && [ "$resume_activation" -eq 0 ]; then
        "$verifier" prepare-local "$bundle/release" "$staging_root" "$core_root" "$state_root" \
            "$bundle/core.ed25519.pub" >/dev/null
    fi
    "$verifier" activate-updater-slot "$update_root" a || fail 'updater slot pointer commit failed'
    if [ "$already_active" -eq 0 ]; then
        "$verifier" bootstrap-activate "$state_root" "$core_root" "$key_target"
    fi
    "$verifier" state "$state_root" | grep -q 'phase=confirmed'
    require_hash "$core_root/current/manifest.v1" "$manifest_hash"
    "$verifier" verify-current "$core_root" "$key_target" >/dev/null
    "$verifier" verify-manifest "$core_root/current/manifest.v1" \
        "$core_root/current/manifest.v1.sig" "$key_target" >/dev/null
    # Keep original compatibility paths intact until signed activation succeeds,
    # so an interrupted first enrollment can still use the approved old startup.
    replace_compatibility_link C1ancher ../core/current/artifacts/C1ancher
    replace_compatibility_link c1pkg ../core/current/artifacts/c1pkg
    replace_compatibility_link app_daemon ../core/current/artifacts/C1ancher-launcher
    replace_compatibility_link c1updater ../core/current/artifacts/c1updater
    {
        echo 'C1CORE-ENROLLED 1'
        echo "bootstrap_sha256=$bootstrap_hash"
        echo "key_sha256=$key_hash"
        echo "manifest_sha256=$manifest_hash"
        echo "recovery_verifier_sha256=$updater_hash"
        echo 'bootstrap_version=1.1.0'
    } >"$marker.new.$$"
    chmod 600 "$marker.new.$$"
    mv -f "$marker.new.$$" "$marker"
    sync
    # Durable signed core + completion marker disable startup recovery first.
    # Then remove only the temporary root-side startup, never historical backups.
    remount_root_rw
    rm -f "$slot_root/enrollment-startup.sha256" "$slot_root/enrollment-startup"
    remount_root_ro
    rm -f "$verifier"
    start_chain
    echo 'core enrollment completed'
}

maintenance_bundle() {
    bundle=$1
    verify_bundle "$bundle"
    [ -f "$marker" ] && [ -f "$key_target" ] || fail 'maintenance requires existing enrollment'
    current_hash=$(hash_file "$root_target")
    baseline_allowed "$current_hash" || fail 'old bootstrap hash is not maintenance-approved'
    [ "$(hash_file "$key_target")" = "$key_hash" ] || fail 'maintenance cannot change trust key'
    # Preserve all generation pointers, state, pending counters and the lock.
    # This operation alone may repair an already deployed old /etc/app_daemon.
    stop_chain
    remount_root_rw
    copy_atomic "$bundle/release/artifacts/c1updater" "$slot_root/recovery-verifier" "$updater_hash" 700
    [ "$("$slot_root/recovery-verifier" --recovery-version)" = 'C1RECOVERY-VERIFIER 1.1.0' ] || fail 'recovery verifier capability rejected'
    copy_atomic "$bundle/release/artifacts/c1updater" "$slot_root/slot-a/c1updater" "$updater_hash" 700
    copy_atomic "$bundle/release/artifacts/c1updater" "$slot_root/slot-b/c1updater" "$updater_hash" 700
    copy_atomic "$bundle/app-daemon-bootstrap.sh" "$root_target" "$bootstrap_hash" 755
    printf '1.1.0 %s\n' "$bootstrap_hash" >"$slot_root/bootstrap.version.new.$$"
    chmod 600 "$slot_root/bootstrap.version.new.$$"
    chown 0:0 "$slot_root/bootstrap.version.new.$$"
    mv -f "$slot_root/bootstrap.version.new.$$" "$slot_root/bootstrap.version"
    remount_root_ro
    {
        echo 'C1CORE-ENROLLED 1'
        echo "bootstrap_sha256=$bootstrap_hash"
        echo "key_sha256=$key_hash"
        sed -n '/^manifest_sha256=/p' "$marker"
        echo "recovery_verifier_sha256=$updater_hash"
        echo 'bootstrap_version=1.1.0'
    } >"$marker.new.$$"
    chmod 600 "$marker.new.$$"
    mv -f "$marker.new.$$" "$marker"
    sync
    start_chain
    echo 'trusted core maintenance completed; core generations retained'
}

verify_installation() {
    for pending in "$slot_root/enrollment-startup" "$slot_root/enrollment-startup.sha256"; do
        [ ! -e "$pending" ] && [ ! -L "$pending" ] || fail 'initial startup recovery cleanup incomplete'
    done
    [ -f "$marker" ] && [ ! -L "$marker" ] || fail 'enrollment marker is missing'
    require_hash "$key_target" "$(sed -n 's/^key_sha256=//p' "$marker")"
    require_root_metadata "$key_target" -rw-------
    require_root_metadata "$slot_root/slot-a/c1updater" -rwx------
    require_root_metadata "$slot_root/slot-b/c1updater" -rwx------
    require_root_metadata "$slot_root/recovery-verifier" -rwx------
    require_root_metadata "$slot_root/bootstrap.version" -rw-------
    require_hash "$slot_root/recovery-verifier" "$(sed -n 's/^recovery_verifier_sha256=//p' "$marker")"
    [ "$("$slot_root/recovery-verifier" --recovery-version)" = 'C1RECOVERY-VERIFIER 1.1.0' ] || fail 'recovery verifier capability rejected'
    [ "$(cat "$slot_root/bootstrap.version")" = "1.1.0 $(hash_file "$root_target")" ] || fail 'bootstrap capability binding rejected'
    [ -L "$core_root/current" ] && [ -L "$core_root/previous" ] || fail 'core generation pointers are missing'
    "$slot_root/slot-a/c1updater" --self-test
    "$slot_root/recovery-verifier" state "$state_root" | grep -Eq 'phase=(confirmed|idle)'
    "$slot_root/recovery-verifier" verify-current "$core_root" "$key_target" >/dev/null
    for artifact in C1ancher c1pkg C1ancher-launcher c1updater; do
        require_root_metadata "$core_root/current/artifacts/$artifact" -rwx------
    done
    require_hash "$root_target" "$(sed -n 's/^bootstrap_sha256=//p' "$marker")"
    require_root_metadata "$root_target" -rwxr-xr-x
    root_is_read_only || fail 'root filesystem is not read-only'
    echo 'core enrollment verified'
}

uninstall() {
    [ -f "$marker" ] || fail 'device is not enrolled'
    restore_daemon=$data_recovery/app_daemon.pre-enrollment
    [ -f "$restore_daemon" ] && [ ! -L "$restore_daemon" ] || restore_daemon=$storage_recovery/app_daemon.pre-enrollment
    [ -f "$restore_daemon" ] && [ ! -L "$restore_daemon" ] || fail 'pre-enrollment root daemon backup is missing'
    stop_chain
    remount_root_rw
    original_hash=$(hash_file "$restore_daemon")
    copy_atomic "$restore_daemon" "$root_target" "$original_hash" 755
    rm -rf "$slot_root"
    remount_root_ro
    for name in C1ancher c1pkg app_daemon c1updater; do
        backup=$data_recovery/bin/$name
        [ -f "$backup" ] || backup=$storage_recovery/bin/$name
        if [ -f "$backup" ]; then
            old_hash=$(hash_file "$backup")
            copy_atomic "$backup" "$bin_root/$name" "$old_hash" 700
        else
            rm -f "$bin_root/$name"
        fi
    done
    rm -f "$marker" "$slot_file"
    sync
    start_chain
    echo 'core enrollment removed; signed generations retained for recovery'
}

action=${1:-}
bundle=${2:-}
case "$action" in
    install) [ -n "$bundle" ] || fail 'bundle path is required'; stage_bundle "$bundle" ;;
    maintenance-install) [ -n "$bundle" ] || fail 'bundle path is required'; stage_bundle "$bundle" maintenance ;;
    maintenance-logged)
        apply_log=${3:-}
        [ -n "$bundle" ] && [ -n "$apply_log" ] || fail 'bundle and log paths are required'
        maintenance_bundle "$bundle" >"$apply_log" 2>&1
        ;;
    maintenance) [ -n "$bundle" ] || fail 'bundle path is required'; maintenance_bundle "$bundle" ;;
    apply) [ -n "$bundle" ] || fail 'bundle path is required'; apply_bundle "$bundle" ;;
    apply-logged)
        apply_log=${3:-}
        [ -n "$bundle" ] && [ -n "$apply_log" ] || fail 'bundle and log paths are required'
        apply_bundle "$bundle" >"$apply_log" 2>&1
        ;;
    verify) verify_installation ;;
    uninstall) uninstall ;;
    *) echo "usage: $0 {install|apply BUNDLE|verify|uninstall}" >&2; exit 64 ;;
esac

trap - 0 1 2 15
exit 0