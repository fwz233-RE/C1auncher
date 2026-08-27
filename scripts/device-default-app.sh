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
factory_app_dir=/usr/bin/d261
factory_init_script=/etc/init.d/S80app
staged_shim=/dev/shm/C1ancher-daemon.shim
staged_app=/dev/shm/C1ancher.install
staged_launcher=/dev/shm/C1ancher-launcher.install
staged_host_key=/dev/shm/c1-ssh-host-key.install
ssh_dir=/usr/data/c1/ssh
ssh_host_key=$ssh_dir/ssh_host_ed25519_key
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

install_host_key() {
    expected=$1

    mkdir -p "$ssh_dir"
    chmod 700 "$ssh_dir"
    if [ -f "$ssh_host_key" ]; then
        chmod 600 "$ssh_host_key"
        return
    fi
    install_binary "$staged_host_key" "$ssh_host_key" "$expected"
    chmod 600 "$ssh_host_key"
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
    host_key_hash=$6
    neofetch_command_hash=$7
    neofetch_upstream_hash=$8
    neofetch_config_hash=$9
    shift 9
    neofetch_license_hash=$1
    neofetch_logo_hash=$2
    temporary=$directory/manifest.txt.new.$$
    {
        echo 'feature=default-C1ancher'
        echo "original_app_daemon_sha256=$original_hash"
        echo "shim_sha256=$shim_hash"
        echo "c1ancher_sha256=$app_hash"
        echo "c1ancher_launcher_sha256=$launcher_hash"
        echo "ssh_host_key_sha256=$host_key_hash"
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

install_default_app() {
    original_hash=$1
    shim_hash=$2
    app_hash=$3
    launcher_hash=$4
    host_key_hash=$5
    neofetch_command_hash=$6
    neofetch_upstream_hash=$7
    neofetch_config_hash=$8
    neofetch_license_hash=$9
    shift 9
    neofetch_logo_hash=$1
    previous_shim_hash=$2
    current_hash=$(hash_file "$target")

    require_hash "$staged_shim" "$shim_hash"
    require_hash "$staged_app" "$app_hash"
    require_hash "$staged_launcher" "$launcher_hash"
    if [ ! -f "$ssh_host_key" ]; then
        require_hash "$staged_host_key" "$host_key_hash"
    fi
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
        install_host_key "$host_key_hash"
    else
        [ "$current_hash" = "$original_hash" ] || [ "$current_hash" = "$previous_shim_hash" ] || {
            echo "refusing install: unexpected current hash $current_hash" >&2
            return 1
        }
        install_binary "$staged_app" "$app" "$app_hash"
        install_binary "$staged_launcher" "$launcher" "$launcher_hash"
        install_host_key "$host_key_hash"
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
    install_neofetch "$neofetch_command_hash" "$neofetch_upstream_hash" "$neofetch_config_hash" "$neofetch_license_hash" "$neofetch_logo_hash"
    verify_neofetch "$neofetch_command_hash" "$neofetch_upstream_hash" "$neofetch_config_hash" "$neofetch_license_hash" "$neofetch_logo_hash"
    require_hash "$ssh_host_key" "$host_key_hash"
    cp "$0" "$data_dir/device-default-app.sh"
    cp "$0" "$storage_dir/device-default-app.sh"
    chmod 700 "$data_dir/device-default-app.sh" "$storage_dir/device-default-app.sh"
    write_manifest "$data_dir" "$original_hash" "$shim_hash" "$app_hash" "$launcher_hash" "$host_key_hash" "$neofetch_command_hash" "$neofetch_upstream_hash" "$neofetch_config_hash" "$neofetch_license_hash" "$neofetch_logo_hash"
    write_manifest "$storage_dir" "$original_hash" "$shim_hash" "$app_hash" "$launcher_hash" "$host_key_hash" "$neofetch_command_hash" "$neofetch_upstream_hash" "$neofetch_config_hash" "$neofetch_license_hash" "$neofetch_logo_hash"
    temporary=$enabled.new.$$
    echo 'enabled' > "$temporary"
    chmod 600 "$temporary"
    mv "$temporary" "$enabled"
    sync
    root_is_read_only
    rollback_needed=0
    start_application_chain
    echo 'default C1ancher, app_daemon, Neofetch, and SSH host identity installed'
}

verify_default_app() {
    original_hash=$1
    shim_hash=$2
    app_hash=$3
    launcher_hash=$4
    host_key_hash=$5
    neofetch_command_hash=$6
    neofetch_upstream_hash=$7
    neofetch_config_hash=$8
    neofetch_license_hash=$9
    shift 9
    neofetch_logo_hash=$1

    require_hash "$target" "$shim_hash"
    require_hash "$root_backup" "$original_hash"
    require_hash "$data_backup" "$original_hash"
    require_hash "$storage_backup" "$original_hash"
    require_hash "$app" "$app_hash"
    [ ! -e "$legacy_app" ]
    require_hash "$launcher" "$launcher_hash"
    require_hash "$ssh_host_key" "$host_key_hash"
    verify_neofetch "$neofetch_command_hash" "$neofetch_upstream_hash" "$neofetch_config_hash" "$neofetch_license_hash" "$neofetch_logo_hash"
    [ -f "$enabled" ]
    if [ -f "$original_removed" ]; then
        [ ! -e "$factory_app_dir" ]
        ! grep -q 'mpenMain\|/usr/bin/d261\|force-original' "$factory_init_script"
    fi
    root_is_read_only
    echo 'default C1ancher, app_daemon, and Neofetch persistent files verified'
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
host_key_hash=${6:-}
neofetch_command_hash=${7:-}
neofetch_upstream_hash=${8:-}
neofetch_config_hash=${9:-}
neofetch_license_hash=${10:-}
neofetch_logo_hash=${11:-}
previous_shim_hash=${12:-}
[ -n "$original_hash" ] && [ -n "$shim_hash" ] && [ -n "$app_hash" ] && [ -n "$launcher_hash" ] && [ -n "$host_key_hash" ] && [ -n "$neofetch_command_hash" ] && [ -n "$neofetch_upstream_hash" ] && [ -n "$neofetch_config_hash" ] && [ -n "$neofetch_license_hash" ] && [ -n "$neofetch_logo_hash" ] && [ -n "$previous_shim_hash" ] || {
    echo "usage: $0 {install|verify|remove-original} ORIGINAL_HASH SHIM_HASH APP_HASH LAUNCHER_HASH HOST_KEY_HASH NEOFETCH_COMMAND_HASH NEOFETCH_UPSTREAM_HASH NEOFETCH_CONFIG_HASH NEOFETCH_LICENSE_HASH NEOFETCH_LOGO_HASH PREVIOUS_SHIM_HASH" >&2
    exit 64
}

case "$action" in
    install) install_default_app "$original_hash" "$shim_hash" "$app_hash" "$launcher_hash" "$host_key_hash" "$neofetch_command_hash" "$neofetch_upstream_hash" "$neofetch_config_hash" "$neofetch_license_hash" "$neofetch_logo_hash" "$previous_shim_hash" ;;
    verify) verify_default_app "$original_hash" "$shim_hash" "$app_hash" "$launcher_hash" "$host_key_hash" "$neofetch_command_hash" "$neofetch_upstream_hash" "$neofetch_config_hash" "$neofetch_license_hash" "$neofetch_logo_hash" ;;
    remove-original) remove_original_software "$original_hash" "$shim_hash" "$app_hash" "$launcher_hash" ;;
    *)
        echo "usage: $0 {install|verify|remove-original} ORIGINAL_HASH SHIM_HASH APP_HASH LAUNCHER_HASH HOST_KEY_HASH NEOFETCH_COMMAND_HASH NEOFETCH_UPSTREAM_HASH NEOFETCH_CONFIG_HASH NEOFETCH_LICENSE_HASH NEOFETCH_LOGO_HASH PREVIOUS_SHIM_HASH" >&2
        exit 64
        ;;
esac

trap - 0 1 2 15
exit 0