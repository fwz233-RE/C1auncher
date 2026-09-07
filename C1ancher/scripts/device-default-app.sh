#!/bin/sh
set -eu

target=/etc/app_daemon
root_backup=/etc/app_daemon.c1-original
data_dir=/usr/data/c1/recovery/default-app
storage_dir=/storage/c1/recovery/default-app
data_backup=$data_dir/app_daemon.original
storage_backup=$storage_dir/app_daemon.original
bin_dir=/usr/data/c1/bin
app=$bin_dir/C1ancher
legacy_app=$bin_dir/c1-app
launcher=$bin_dir/app_daemon
enabled=/usr/data/c1/enabled
original_removed=/usr/data/c1/original-software-removed
auto_suspend_disabled=/usr/data/c1/disable-auto-suspend
core_enrolled=/usr/data/c1/update/enrolled.v1
factory_app_dir=/usr/bin/d261
factory_init_script=/etc/init.d/S80app
staged_shim=/dev/shm/C1ancher-daemon.shim
staged_app=/dev/shm/C1ancher.install
staged_launcher=/dev/shm/C1ancher-launcher.install
pkg=$bin_dir/c1pkg
staged_pkg=/dev/shm/c1pkg.install
pkg_state_dir=/usr/data/c1/pkg
repository_public_key=$pkg_state_dir/repository.ed25519.pub
staged_repository_public_key=/dev/shm/c1pkg-repository-key.install
legacy_ssh_run_dir=/run/c1/ssh
legacy_ssh_dir=/usr/data/c1/ssh
neofetch_dir=/usr/data/c1/neofetch
neofetch_command=$bin_dir/neofetch
staged_neofetch_command=/dev/shm/c1-neofetch.install
staged_neofetch_upstream=/dev/shm/c1-neofetch-upstream.install
staged_neofetch_config=/dev/shm/c1-neofetch-config.install
staged_neofetch_logo=/dev/shm/c1-neofetch-logo.install
staged_neofetch_license=/dev/shm/c1-neofetch-license.install
root_writable=0
rollback_needed=0
original_stopped=0

hash_file() {
    sha256sum "$1" | while IFS=' ' read -r value remainder; do
        printf '%s\n' "$value"
        break
    done
}

require_hash() {
    path=$1
    expected=$2
    [ -f "$path" ] || {
        echo "missing file: $path" >&2
        return 1
    }
    actual=$(hash_file "$path")
    [ "$actual" = "$expected" ] || {
        echo "hash mismatch: $path expected=$expected actual=$actual" >&2
        return 1
    }
}

root_is_read_only() {
    while IFS=' ' read -r device mountpoint filesystem options remainder; do
        [ "$mountpoint" = / ] || continue
        case ",$options," in
            *,ro,*) return 0 ;;
            *) return 1 ;;
        esac
    done < /proc/mounts
    return 1
}

remount_root_rw() {
    /bin/mount -o remount,rw / 2>/dev/null || true
    if root_is_read_only; then
        echo 'root filesystem did not become writable' >&2
        return 1
    fi
    root_writable=1
}

remount_root_ro() {
    if [ "$root_writable" -eq 1 ]; then
        sync
        /bin/mount -o remount,ro / 2>/dev/null || true
        if ! root_is_read_only; then
            echo 'root filesystem did not return to read-only' >&2
            return 1
        fi
        root_writable=0
    fi
}

app_daemon_running() {
    for cmdline in /proc/[0-9]*/cmdline; do
        [ -r "$cmdline" ] || continue
        command_line=$(tr '\000' ' ' < "$cmdline")
        case "$command_line" in
            *"/etc/app_daemon"*|*"/usr/data/c1/bin/app_daemon"*) return 0 ;;
        esac
    done
    return 1
}

stop_application_chain() {
    original_stopped=1
    /etc/init.d/S80app stop || true
    count=0
    while [ "$count" -lt 10 ] && { app_daemon_running || pidof C1ancher >/dev/null 2>&1 || pidof c1-app >/dev/null 2>&1; }; do
        sleep 1
        count=$((count + 1))
    done
    ! app_daemon_running
    ! pidof C1ancher >/dev/null 2>&1
    ! pidof c1-app >/dev/null 2>&1
    original_stopped=1
}

start_application_chain() {
    if ! app_daemon_running; then
        /bin/busybox start-stop-daemon -S -b -x /etc/app_daemon
    fi
    original_stopped=0
}

cleanup() {
    status=$?
    trap - 0 1 2 15
    if [ "$status" -ne 0 ] && [ "$rollback_needed" -eq 1 ] && [ -f "$root_backup" ]; then
        if root_is_read_only; then
            /bin/mount -o remount,rw / 2>/dev/null || true
        fi
        if ! root_is_read_only; then
            root_writable=1
            cp "$root_backup" "$target" || true
            chmod 755 "$target" || true
            sync || true
        fi
    fi
    if ! root_is_read_only; then
        root_writable=1
    fi
    if [ "$root_writable" -eq 1 ]; then
        remount_root_ro || true
    fi
    if [ "$original_stopped" -eq 1 ]; then
        start_application_chain || true
    fi
    exit "$status"
}

ensure_backup() {
    destination=$1
    expected=$2
    if [ -e "$destination" ]; then
        require_hash "$destination" "$expected"
        return
    fi
    temporary=$destination.new.$$
    cp "$target" "$temporary"
    chmod 755 "$temporary"
    require_hash "$temporary" "$expected"
    mv "$temporary" "$destination"
    sync
}

install_binary() {
    source=$1
    destination=$2
    expected=$3
    temporary=$destination.new.$$

    require_hash "$source" "$expected"
    cp "$source" "$temporary"
    chmod 700 "$temporary"
    require_hash "$temporary" "$expected"
    mv "$temporary" "$destination"
}

cleanup_legacy_ssh() {
    if [ -r "$legacy_ssh_run_dir/sshd.pid" ]; then
        legacy_pid=$(cat "$legacy_ssh_run_dir/sshd.pid" 2>/dev/null || true)
        case "$legacy_pid" in
            ''|*[!0-9]*) legacy_pid= ;;
        esac
        if [ -n "$legacy_pid" ] && [ "$legacy_pid" -gt 1 ] && [ -r "/proc/$legacy_pid/cmdline" ]; then
            legacy_cmdline=$(tr '\000' ' ' < "/proc/$legacy_pid/cmdline")
            case "$legacy_cmdline" in
                *"/usr/sbin/sshd"*"/run/c1/ssh/sshd_config"*)
                    kill "$legacy_pid" 2>/dev/null || true
                    count=0
                    while [ "$count" -lt 30 ] && kill -0 "$legacy_pid" 2>/dev/null; do
                        sleep 1
                        count=$((count + 1))
                    done
                    kill -9 "$legacy_pid" 2>/dev/null || true
                    ;;
            esac
        fi
    fi
    if [ -f "$legacy_ssh_run_dir/shadow" ] && [ -e /etc/shadow ]; then
        run_shadow_id=$(stat -c '%d:%i' "$legacy_ssh_run_dir/shadow" 2>/dev/null || true)
        etc_shadow_id=$(stat -c '%d:%i' /etc/shadow 2>/dev/null || true)
        if [ -n "$run_shadow_id" ] && [ "$run_shadow_id" = "$etc_shadow_id" ]; then
            umount /etc/shadow 2>/dev/null || umount -l /etc/shadow 2>/dev/null || return 1
        fi
    fi
    rm -rf "$legacy_ssh_run_dir" "$legacy_ssh_dir"
}

install_package_manager() {
    pkg_hash=$1
    public_key_hash=$2

    mkdir -p "$bin_dir" "$pkg_state_dir"
    chmod 700 "$pkg_state_dir"
    install_binary "$staged_pkg" "$pkg" "$pkg_hash"
    install_binary "$staged_repository_public_key" "$repository_public_key" "$public_key_hash"
    chmod 700 "$pkg"
    chmod 600 "$repository_public_key"
}

verify_package_manager() {
    pkg_hash=$1
    public_key_hash=$2

    require_hash "$pkg" "$pkg_hash"
    require_hash "$repository_public_key" "$public_key_hash"
    [ "$(wc -c < "$repository_public_key")" -eq 32 ]
    "$pkg" --help >/dev/null
}

install_neofetch() {
    command_hash=$1
    upstream_hash=$2
    config_hash=$3
    license_hash=$4
    logo_hash=$5

    mkdir -p "$neofetch_dir" "$bin_dir"
    install_binary "$staged_neofetch_command" "$neofetch_command" "$command_hash"
    install_binary "$staged_neofetch_upstream" "$neofetch_dir/neofetch.upstream" "$upstream_hash"
    install_binary "$staged_neofetch_config" "$neofetch_dir/c1-config.conf" "$config_hash"
    install_binary "$staged_neofetch_license" "$neofetch_dir/LICENSE.md" "$license_hash"
    install_binary "$staged_neofetch_logo" "$neofetch_dir/c1-logo.txt" "$logo_hash"
    chmod 700 "$neofetch_command" "$neofetch_dir/neofetch.upstream"
    chmod 644 "$neofetch_dir/c1-config.conf" "$neofetch_dir/LICENSE.md" "$neofetch_dir/c1-logo.txt"
}

verify_neofetch() {
    command_hash=$1
    upstream_hash=$2
    config_hash=$3
    license_hash=$4
    logo_hash=$5

    require_hash "$neofetch_command" "$command_hash"
    require_hash "$neofetch_dir/neofetch.upstream" "$upstream_hash"
    require_hash "$neofetch_dir/c1-config.conf" "$config_hash"
    require_hash "$neofetch_dir/LICENSE.md" "$license_hash"
    require_hash "$neofetch_dir/c1-logo.txt" "$logo_hash"
    output=$(TERM=xterm-256color COLUMNS=49 LINES=19 "$neofetch_command" --stdout 2>&1)
    case "$output" in
        *"C1-Slim MP-D261"*) ;;
        *)
            echo 'C1 Neofetch smoke test failed' >&2
            return 1
            ;;
    esac
}

write_manifest() {
    directory=$1
    original_hash=$2
    shim_hash=$3
    app_hash=$4
    launcher_hash=$5
    pkg_hash=$6
    public_key_hash=$7
    neofetch_command_hash=$8
    neofetch_upstream_hash=$9
    shift 9
    neofetch_config_hash=$1
    neofetch_license_hash=$2
    neofetch_logo_hash=$3
    temporary=$directory/manifest.txt.new.$$
    {
        echo 'feature=default-C1ancher-app-store'
        echo "original_app_daemon_sha256=$original_hash"
        echo "shim_sha256=$shim_hash"
        echo "c1ancher_sha256=$app_hash"
        echo "c1ancher_launcher_sha256=$launcher_hash"
        echo "c1pkg_sha256=$pkg_hash"
        echo "repository_public_key_sha256=$public_key_hash"
        echo "neofetch_command_sha256=$neofetch_command_hash"
        echo "neofetch_upstream_sha256=$neofetch_upstream_hash"
        echo "neofetch_config_sha256=$neofetch_config_hash"
        echo "neofetch_license_sha256=$neofetch_license_hash"
        echo "neofetch_logo_sha256=$neofetch_logo_hash"
        echo 'restart_policy=always-C1ancher'
        echo 'original_software=/usr/bin/d261 (removed separately)'
    } > "$temporary"
    chmod 600 "$temporary"
    mv "$temporary" "$directory/manifest.txt"
    sync
}

# Keep this hardware policy aligned with the runtime and EXE device helper.
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

validate_auto_suspend_path() {
    checked=$auto_suspend_disabled
    while [ "$checked" != / ]; do
        [ ! -L "$checked" ] || { echo "unsafe automatic suspend path: $checked" >&2; return 1; }
        checked=${checked%/*}; [ -n "$checked" ] || checked=/
    done
    if [ -e "$auto_suspend_disabled" ]; then
        [ -f "$auto_suspend_disabled" ] && [ "$(stat -c %h "$auto_suspend_disabled")" = 1 ] || {
            echo 'automatic suspend marker must be a single-link regular file' >&2
            return 1
        }
    fi
}

validate_auto_suspend_request() {
    validate_auto_suspend_path || return 1
    case "$1" in
        enabled)
            auto_suspend_supported || {
                echo 'automatic suspend requires supported C1-Slim hardware, enabled gpio_keys wakeup, and writable mem suspend' >&2
                return 1
            } ;;
        default|disabled) ;;
        *) echo "invalid automatic suspend mode: $1" >&2; return 1 ;;
    esac
}

configure_auto_suspend() {
    mode=$1
    validate_auto_suspend_request "$mode" || return 1
    if [ "$mode" = default ]; then
        # Unsupported hardware always fails closed, including updates/retries.
        # On supported hardware preserve every existing disable preference.
        if ! auto_suspend_supported; then
            mode=disabled
        elif [ -e "$auto_suspend_disabled" ] || [ -e "$enabled" ] || [ -e "$core_enrolled" ]; then
            echo 'automatic_suspend=preserved'
            return 0
        else
            mode=enabled
        fi
    fi
    case "$mode" in
        enabled) rm -f "$auto_suspend_disabled" ;;
        disabled)
            # Do not replace an existing user/legacy marker even on explicit disable.
            if [ ! -e "$auto_suspend_disabled" ]; then
                temporary=$auto_suspend_disabled.new.$$
                (set -C; : > "$temporary") || return 1
                chmod 600 "$temporary"
                mv "$temporary" "$auto_suspend_disabled"
            fi ;;
    esac
    sync
    echo "automatic_suspend=$mode"
}

verify_auto_suspend() {
    mode=$1
    validate_auto_suspend_request "$mode" || return 1
    case "$mode" in
        enabled) [ ! -e "$auto_suspend_disabled" ] ;;
        disabled) [ -f "$auto_suspend_disabled" ] ;;
        default)
            # Verification observes, never resolves a default by changing files.
            [ -d /usr/data/c1 ] && {
                [ -f "$auto_suspend_disabled" ] || auto_suspend_supported
            } ;;
        *) return 1 ;;
    esac
}

install_default_app() {
    # Reject an unsafe marker or unsupported explicit enable before any install
    # mutation; configure_auto_suspend rechecks immediately before applying it.
    validate_auto_suspend_request "${13:-default}" || return 1
    original_hash=$1
    shim_hash=$2
    app_hash=$3
    launcher_hash=$4
    pkg_hash=$5
    public_key_hash=$6
    neofetch_command_hash=$7
    neofetch_upstream_hash=$8
    neofetch_config_hash=$9
    shift 9
    neofetch_license_hash=$1
    neofetch_logo_hash=$2
    previous_shim_hash=$3
    auto_suspend_mode=$4
    current_hash=$(hash_file "$target")

    require_hash "$staged_shim" "$shim_hash"
    require_hash "$staged_app" "$app_hash"
    require_hash "$staged_launcher" "$launcher_hash"
    require_hash "$staged_pkg" "$pkg_hash"
    require_hash "$staged_repository_public_key" "$public_key_hash"
    require_hash "$staged_neofetch_command" "$neofetch_command_hash"
    require_hash "$staged_neofetch_upstream" "$neofetch_upstream_hash"
    require_hash "$staged_neofetch_config" "$neofetch_config_hash"
    require_hash "$staged_neofetch_license" "$neofetch_license_hash"
    require_hash "$staged_neofetch_logo" "$neofetch_logo_hash"
    mkdir -p "$data_dir" "$storage_dir" "$bin_dir"
    ensure_backup "$data_backup" "$original_hash"
    ensure_backup "$storage_backup" "$original_hash"

    if [ "$current_hash" = "$shim_hash" ]; then
        require_hash "$root_backup" "$original_hash"
        stop_application_chain
        install_binary "$staged_app" "$app" "$app_hash"
        install_binary "$staged_launcher" "$launcher" "$launcher_hash"
        install_package_manager "$pkg_hash" "$public_key_hash"
    else
        [ "$current_hash" = "$original_hash" ] || [ "$current_hash" = "$previous_shim_hash" ] || {
            echo "refusing install: unexpected current hash $current_hash" >&2
            return 1
        }
        install_binary "$staged_app" "$app" "$app_hash"
        install_binary "$staged_launcher" "$launcher" "$launcher_hash"
        install_package_manager "$pkg_hash" "$public_key_hash"
        stop_application_chain
        remount_root_rw
        ensure_backup "$root_backup" "$original_hash"
        rollback_needed=1
        temporary=$target.c1-new.$$
        cp "$staged_shim" "$temporary"
        chmod 755 "$temporary"
        require_hash "$temporary" "$shim_hash"
        mv "$temporary" "$target"
        require_hash "$target" "$shim_hash"
        remount_root_ro
    fi

    rm -f "$legacy_app"
    cleanup_legacy_ssh
    verify_package_manager "$pkg_hash" "$public_key_hash"
    install_neofetch "$neofetch_command_hash" "$neofetch_upstream_hash" "$neofetch_config_hash" "$neofetch_license_hash" "$neofetch_logo_hash"
    verify_neofetch "$neofetch_command_hash" "$neofetch_upstream_hash" "$neofetch_config_hash" "$neofetch_license_hash" "$neofetch_logo_hash"
    cp "$0" "$data_dir/device-default-app.sh"
    cp "$0" "$storage_dir/device-default-app.sh"
    chmod 700 "$data_dir/device-default-app.sh" "$storage_dir/device-default-app.sh"
    write_manifest "$data_dir" "$original_hash" "$shim_hash" "$app_hash" "$launcher_hash" "$pkg_hash" "$public_key_hash" "$neofetch_command_hash" "$neofetch_upstream_hash" "$neofetch_config_hash" "$neofetch_license_hash" "$neofetch_logo_hash"
    write_manifest "$storage_dir" "$original_hash" "$shim_hash" "$app_hash" "$launcher_hash" "$pkg_hash" "$public_key_hash" "$neofetch_command_hash" "$neofetch_upstream_hash" "$neofetch_config_hash" "$neofetch_license_hash" "$neofetch_logo_hash"
    configure_auto_suspend "$auto_suspend_mode"
    temporary=$enabled.new.$$
    echo 'enabled' > "$temporary"
    chmod 600 "$temporary"
    mv "$temporary" "$enabled"
    sync
    root_is_read_only
    rollback_needed=0
    start_application_chain
    echo 'default C1ancher, app_daemon, Neofetch, and c1pkg installed'
}

verify_default_app() {
    original_hash=$1
    shim_hash=$2
    app_hash=$3
    launcher_hash=$4
    pkg_hash=$5
    public_key_hash=$6
    neofetch_command_hash=$7
    neofetch_upstream_hash=$8
    neofetch_config_hash=$9
    shift 9
    neofetch_license_hash=$1
    neofetch_logo_hash=$2
    auto_suspend_mode=$3

    require_hash "$target" "$shim_hash"
    require_hash "$root_backup" "$original_hash"
    require_hash "$data_backup" "$original_hash"
    require_hash "$storage_backup" "$original_hash"
    require_hash "$app" "$app_hash"
    [ ! -e "$legacy_app" ]
    require_hash "$launcher" "$launcher_hash"
    verify_package_manager "$pkg_hash" "$public_key_hash"
    [ ! -e "$legacy_ssh_run_dir" ]
    [ ! -e "$legacy_ssh_dir" ]
    verify_neofetch "$neofetch_command_hash" "$neofetch_upstream_hash" "$neofetch_config_hash" "$neofetch_license_hash" "$neofetch_logo_hash"
    verify_auto_suspend "$auto_suspend_mode"
    [ -f "$enabled" ]
    if [ -f "$original_removed" ]; then
        [ ! -e "$factory_app_dir" ]
        ! grep -q 'mpenMain\|/usr/bin/d261\|force-original' "$factory_init_script"
    fi
    root_is_read_only
    echo 'default C1ancher, app_daemon, Neofetch, and c1pkg persistent files verified'
}

remove_original_software() {
    original_hash=$1
    shim_hash=$2
    app_hash=$3
    launcher_hash=$4

    require_hash "$target" "$shim_hash"
    require_hash "$root_backup" "$original_hash"
    require_hash "$data_backup" "$original_hash"
    require_hash "$storage_backup" "$original_hash"
    require_hash "$app" "$app_hash"
    require_hash "$launcher" "$launcher_hash"
    root_is_read_only

    remount_root_rw
    rm -rf "$factory_app_dir"
    [ ! -e "$factory_app_dir" ]
    temporary=$factory_init_script.c1-new.$$
    sed '/^[[:space:]]*killall -9 mpenMain[[:space:]]*$/d' "$factory_init_script" > "$temporary"
    chmod 755 "$temporary"
    ! grep -q 'mpenMain\|/usr/bin/d261\|force-original' "$temporary"
    mv "$temporary" "$factory_init_script"
    remount_root_ro

    cp "$0" "$data_dir/device-default-app.sh"
    cp "$0" "$storage_dir/device-default-app.sh"
    chmod 700 "$data_dir/device-default-app.sh" "$storage_dir/device-default-app.sh"
    temporary=$original_removed.new.$$
    echo 'removed' > "$temporary"
    chmod 600 "$temporary"
    mv "$temporary" "$original_removed"
    sync
    root_is_read_only
    echo 'original software directory /usr/bin/d261 removed'
}

trap cleanup 0 1 2 15

action=${1:-}
original_hash=${2:-}
shim_hash=${3:-}
app_hash=${4:-}
launcher_hash=${5:-}
pkg_hash=${6:-}
public_key_hash=${7:-}
neofetch_command_hash=${8:-}
neofetch_upstream_hash=${9:-}
neofetch_config_hash=${10:-}
neofetch_license_hash=${11:-}
neofetch_logo_hash=${12:-}
previous_shim_hash=${13:-}
auto_suspend_mode=${14:-default}
[ -n "$original_hash" ] && [ -n "$shim_hash" ] && [ -n "$app_hash" ] && [ -n "$launcher_hash" ] && [ -n "$pkg_hash" ] && [ -n "$public_key_hash" ] && [ -n "$neofetch_command_hash" ] && [ -n "$neofetch_upstream_hash" ] && [ -n "$neofetch_config_hash" ] && [ -n "$neofetch_license_hash" ] && [ -n "$neofetch_logo_hash" ] && [ -n "$previous_shim_hash" ] && [ -n "$auto_suspend_mode" ] || {
    echo "usage: $0 {install|verify|remove-original} ORIGINAL_HASH SHIM_HASH APP_HASH LAUNCHER_HASH C1PKG_HASH REPOSITORY_PUBLIC_KEY_HASH NEOFETCH_COMMAND_HASH NEOFETCH_UPSTREAM_HASH NEOFETCH_CONFIG_HASH NEOFETCH_LICENSE_HASH NEOFETCH_LOGO_HASH PREVIOUS_SHIM_HASH [default|enabled|disabled]" >&2
    exit 64
}
case "$auto_suspend_mode" in default|enabled|disabled) ;; *) echo "invalid automatic suspend mode: $auto_suspend_mode" >&2; exit 64 ;; esac

case "$action" in
    install) install_default_app "$original_hash" "$shim_hash" "$app_hash" "$launcher_hash" "$pkg_hash" "$public_key_hash" "$neofetch_command_hash" "$neofetch_upstream_hash" "$neofetch_config_hash" "$neofetch_license_hash" "$neofetch_logo_hash" "$previous_shim_hash" "$auto_suspend_mode" ;;
    verify) verify_default_app "$original_hash" "$shim_hash" "$app_hash" "$launcher_hash" "$pkg_hash" "$public_key_hash" "$neofetch_command_hash" "$neofetch_upstream_hash" "$neofetch_config_hash" "$neofetch_license_hash" "$neofetch_logo_hash" "$auto_suspend_mode" ;;
    remove-original) remove_original_software "$original_hash" "$shim_hash" "$app_hash" "$launcher_hash" ;;
    *)
        echo "usage: $0 {install|verify|remove-original} ORIGINAL_HASH SHIM_HASH APP_HASH LAUNCHER_HASH C1PKG_HASH REPOSITORY_PUBLIC_KEY_HASH NEOFETCH_COMMAND_HASH NEOFETCH_UPSTREAM_HASH NEOFETCH_CONFIG_HASH NEOFETCH_LICENSE_HASH NEOFETCH_LOGO_HASH PREVIOUS_SHIM_HASH [default|enabled|disabled]" >&2
        exit 64
        ;;
esac

trap - 0 1 2 15
exit 0