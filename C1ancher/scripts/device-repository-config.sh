#!/bin/sh
# Install a TRUSTED LOCAL profile after the signed core enrollment. Never fetch
# this script/public key from an untrusted mirror and execute them as root.
set -eu
[ "$(id -u)" = 0 ] || { echo 'Root required' >&2; exit 1; }
profile=${1:?Usage: device-repository-config.sh PROFILE_DIRECTORY}
profile=$(CDPATH= cd -- "$profile" && pwd -P)
(cd "$profile" && sha256sum -c SHA256SUMS) || exit 1
for name in repository.url core-repository.url repository.ed25519.pub c1-update-check.sh; do
    [ -f "$profile/$name" ] && [ ! -L "$profile/$name" ] || exit 1
done
[ "$(wc -c < "$profile/repository.ed25519.pub")" -eq 32 ] || exit 1
for name in repository.url core-repository.url; do
    [ "$(wc -l < "$profile/$name")" -eq 1 ] || exit 1
    [ "$(wc -c < "$profile/$name")" -le 1025 ] || exit 1
    grep -Eq '^https?://[A-Za-z0-9.-]+(:[0-9]+)?(/[A-Za-z0-9._/-]*)?$' "$profile/$name" || exit 1
done
for dir in /usr/data/c1 /usr/data/c1/pkg /usr/data/c1/update /usr/data/c1/bin; do
    [ ! -L "$dir" ] || { echo 'Unsafe configuration directory' >&2; exit 1; }
    mkdir -p "$dir"
done
key=/usr/data/c1/pkg/repository.ed25519.pub
if [ -e "$key" ] || [ -L "$key" ]; then
    [ ! -L "$key" ] && cmp -s "$profile/repository.ed25519.pub" "$key" || {
        echo 'Existing trust key differs. Use an explicit key migration; do not erase sequence history.' >&2
        exit 1
    }
fi
copy_config() {
    source=$1 target=$2 mode=$3
    temporary=$target.new.$$
    [ ! -e "$temporary" ] && [ ! -L "$temporary" ] || exit 1
    (umask 077; set -C; cat "$source" > "$temporary")
    chmod "$mode" "$temporary"
    chown 0:0 "$temporary"
    sync
    mv -f "$temporary" "$target"
    sync
}
copy_config "$profile/repository.ed25519.pub" "$key" 600
copy_config "$profile/core-repository.url" /usr/data/c1/update/repository.url 600
copy_config "$profile/c1-update-check.sh" /usr/data/c1/bin/c1-update-check 700
# Commit the application endpoint last. Installed apps, data, caches and
# anti-rollback counters are deliberately preserved across server migration.
copy_config "$profile/repository.url" /usr/data/c1/pkg/repository.url 600
echo 'Repository profile installed; run c1pkg refresh to verify connectivity.'
