#!/bin/sh
set -eu

target=/etc/init.d/S90usb
root_backup=/etc/init.d/S90usb.c1-original
data_dir=/usr/data/c1/recovery/open-adb
storage_dir=/storage/c1/recovery/open-adb
data_backup=$data_dir/S90usb.original
storage_backup=$storage_dir/S90usb.original
staged_candidate=/dev/shm/c1-S90usb.open-root-adb
root_writable=0
rollback_needed=0

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

write_manifest() {
    directory=$1
    original_hash=$2
    installed_hash=$3
    temporary=$directory/manifest.txt.new.$$
    {
        echo 'feature=open-root-adb'
        echo "original_sha256=$original_hash"
        echo "installed_sha256=$installed_hash"
        echo 'authentication=none'
        echo 'usb_functions=adb,mtp'
        echo 'restore_command=sh device-open-adb.sh uninstall ORIGINAL_SHA256 INSTALLED_SHA256'
    } > "$temporary"
    chmod 600 "$temporary"
    mv "$temporary" "$directory/manifest.txt"
    sync
}

install_open_adb() {
    original_hash=$1
    installed_hash=$2
    current_hash=$(hash_file "$target")

    [ ! -e /adb_keys ] || {
        echo 'refusing install: /adb_keys exists' >&2
        return 1
    }
    [ ! -e /data/misc/adb/adb_keys ] || {
        echo 'refusing install: /data/misc/adb/adb_keys exists' >&2
        return 1
    }
    require_hash "$staged_candidate" "$installed_hash"

    mkdir -p "$data_dir" "$storage_dir"
    if [ "$current_hash" = "$installed_hash" ]; then
        require_hash "$data_backup" "$original_hash"
        require_hash "$storage_backup" "$original_hash"
        require_hash "$root_backup" "$original_hash"
        root_is_read_only
        echo 'open root ADB is already installed and verified'
        return
    fi
    [ "$current_hash" = "$original_hash" ] || {
        echo "refusing install: unexpected current hash $current_hash" >&2
        return 1
    }

    ensure_backup "$data_backup" "$original_hash"
    ensure_backup "$storage_backup" "$original_hash"

    remount_root_rw
    ensure_backup "$root_backup" "$original_hash"
    rollback_needed=1
    temporary=$target.c1-new.$$
    cp "$staged_candidate" "$temporary"
    chmod 755 "$temporary"
    require_hash "$temporary" "$installed_hash"
    mv "$temporary" "$target"
    require_hash "$target" "$installed_hash"
    remount_root_ro

    cp "$0" "$data_dir/device-open-adb.sh"
    cp "$0" "$storage_dir/device-open-adb.sh"
    chmod 700 "$data_dir/device-open-adb.sh" "$storage_dir/device-open-adb.sh"
    write_manifest "$data_dir" "$original_hash" "$installed_hash"
    write_manifest "$storage_dir" "$original_hash" "$installed_hash"
    root_is_read_only
    rollback_needed=0
    echo 'open root ADB installed; reboot is required for cold-start verification'
}

verify_open_adb() {
    original_hash=$1
    installed_hash=$2

    require_hash "$target" "$installed_hash"
    require_hash "$root_backup" "$original_hash"
    require_hash "$data_backup" "$original_hash"
    require_hash "$storage_backup" "$original_hash"
    [ ! -e /adb_keys ]
    [ ! -e /data/misc/adb/adb_keys ]
    root_is_read_only
    pidof adbd >/dev/null
    [ -d /sys/kernel/config/usb_gadget/demo/configs/c.1/ffs.adb ]
    [ -d /sys/kernel/config/usb_gadget/demo/configs/c.1/ffs.mtp ]
    echo 'open root ADB persistent state verified'
}

uninstall_open_adb() {
    original_hash=$1
    installed_hash=$2
    current_hash=$(hash_file "$target")

    [ "$current_hash" = "$installed_hash" ] || {
        if [ "$current_hash" = "$original_hash" ]; then
            root_is_read_only
            echo 'original USB startup is already restored'
            return
        fi
        echo "refusing uninstall: unexpected current hash $current_hash" >&2
        return 1
    }
    require_hash "$storage_backup" "$original_hash"
    require_hash "$data_backup" "$original_hash"

    remount_root_rw
    temporary=$target.c1-restore.$$
    cp "$root_backup" "$temporary"
    chmod 755 "$temporary"
    require_hash "$temporary" "$original_hash"
    mv "$temporary" "$target"
    require_hash "$target" "$original_hash"
    remount_root_ro
    root_is_read_only
    echo 'original MTP-only USB startup restored; reboot is required'
}

trap cleanup 0 1 2 15

action=${1:-}
original_hash=${2:-}
installed_hash=${3:-}
[ -n "$original_hash" ] && [ -n "$installed_hash" ] || {
    echo "usage: $0 {install|verify|uninstall} ORIGINAL_SHA256 INSTALLED_SHA256" >&2
    exit 64
}

case "$action" in
    install) install_open_adb "$original_hash" "$installed_hash" ;;
    verify) verify_open_adb "$original_hash" "$installed_hash" ;;
    uninstall) uninstall_open_adb "$original_hash" "$installed_hash" ;;
    *)
        echo "usage: $0 {install|verify|uninstall} ORIGINAL_SHA256 INSTALLED_SHA256" >&2
        exit 64
        ;;
esac

trap - 0 1 2 15
exit 0