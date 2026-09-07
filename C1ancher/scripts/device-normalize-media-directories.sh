#!/bin/sh
# One-device maintenance: rename media folders; never merge or overwrite files.
set -eu
root=/storage/mtp
report=/storage/c1/recovery/media-paths-20260906
[ "$(id -u)" = 0 ]
[ -d "$root" ] && [ ! -L "$root" ]
[ ! -e "$report" ] || { echo 'Migration report already exists; review before rerunning' >&2; exit 1; }
for pair in pic:Pic music:Music Books:Book book:Book; do
    source=${pair%:*}; target=${pair#*:}
    if [ -e "$root/$source" ] || [ -L "$root/$source" ]; then
        [ -d "$root/$source" ] && [ ! -L "$root/$source" ]
        if [ -e "$root/$target" ] || [ -L "$root/$target" ]; then
            [ -d "$root/$target" ] && [ ! -L "$root/$target" ]
            [ -z "$(find "$root/$target" -mindepth 1 -maxdepth 1 -print -quit)" ] || {
                echo "Both $source and $target contain data; refusing overwrite" >&2; exit 1;
            }
        fi
    fi
done
[ ! -d "$root/Books" ] || [ ! -d "$root/book" ] || { echo 'Two old book folders need conflict review' >&2; exit 1; }
mkdir -p "$report"
for pair in pic:Pic music:Music Books:Book book:Book; do
    source=${pair%:*}; target=${pair#*:}
    [ -d "$root/$source" ] || continue
    (cd "$root/$source" && find . -type f -exec sha256sum '{}' ';' | LC_ALL=C sort) >"$report/$source.before.sha256"
    (cd "$root/$source" && find . -type d | LC_ALL=C sort) >"$report/$source.before.dirs"
    # rmdir refuses if a user has added anything since the preflight check.
    if [ -d "$root/$target" ]; then rmdir "$root/$target"; fi
    mv -T "$root/$source" "$root/$target"
    (cd "$root/$target" && find . -type f -exec sha256sum '{}' ';' | LC_ALL=C sort) >"$report/$source.after.sha256"
    (cd "$root/$target" && find . -type d | LC_ALL=C sort) >"$report/$source.after.dirs"
    cmp "$report/$source.before.sha256" "$report/$source.after.sha256"
    cmp "$report/$source.before.dirs" "$report/$source.after.dirs"
    echo "Renamed and content-verified: $source -> $target"
done
mkdir -p "$root/Pic" "$root/Music" "$root/Book"
sync
printf 'Verified canonical directories: Pic Music Book\n'
