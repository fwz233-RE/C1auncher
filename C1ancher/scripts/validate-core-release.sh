#!/usr/bin/env bash
set -Eeuo pipefail
IFS=$'\n\t'
LC_ALL=C
export LC_ALL

usage() {
    printf 'Usage: validate-core-release.sh RELEASE_DIRECTORY TRUSTED_PUBLIC_KEY_PEM\n' >&2
    exit 2
}

(($# == 2)) || usage
release=$1
trusted_key=$2
manifest=$release/manifest.v1
signature=$release/manifest.v1.sig
artifacts=$release/artifacts
max_component=33554432
max_payload=100663296
expected_target=mips32r2-little-o32-hard-float-double-static

fail() {
    printf 'Core release validation failed: %s\n' "$1" >&2
    exit 1
}

for command in find sort cmp wc od tail tr grep sha256sum openssl readelf stat mktemp; do
    command -v "$command" >/dev/null 2>&1 || fail "required command is missing: $command"
done

[[ -d $release && ! -L $release ]] || fail 'release root is not a real directory'
[[ -f $trusted_key && ! -L $trusted_key ]] || fail 'trusted public key is not a regular file'
[[ -d $artifacts && ! -L $artifacts ]] || fail 'artifacts is not a real directory'

expected_files=$'artifacts/C1ancher\nartifacts/C1ancher-launcher\nartifacts/c1pkg\nartifacts/c1updater\nmanifest.v1\nmanifest.v1.sig'
actual_files=$(find "$release" -type f -printf '%P\n' | sort)
[[ $actual_files == "$expected_files" ]] || fail 'release has an unexpected file set'
[[ -z $(find "$release" -mindepth 1 ! -type f ! -type d -print -quit) ]] || fail 'release contains a link or special file'
[[ -z $(find "$release" -type f -links +1 -print -quit) ]] || fail 'release contains a hard-linked file'
actual_directories=$(find "$release" -mindepth 1 -type d -printf '%P\n' | sort)
[[ $actual_directories == 'artifacts' ]] || fail 'release has an unexpected directory set'

[[ $(wc -c <"$signature") -eq 64 ]] || fail 'signature is not exactly 64 bytes'
manifest_size=$(wc -c <"$manifest")
((manifest_size > 0 && manifest_size <= 65536)) || fail 'manifest size is invalid'
[[ $(tail -c 1 "$manifest" | od -An -tu1 | tr -d ' \n') == 10 ]] || fail 'manifest must end with one LF'
[[ -z $(tr -d '\11\12\40-\176' <"$manifest") ]] || fail 'manifest contains non-ASCII or forbidden control bytes'
[[ -z $(grep -n $'\r' "$manifest" || true) ]] || fail 'manifest contains CR bytes'
mapfile -t lines <"$manifest"
((${#lines[@]} == 14)) || fail "manifest must contain exactly 14 records (found ${#lines[@]})"
[[ ${lines[0]} == 'C1CORE-MANIFEST 1' ]] || fail 'manifest header is invalid'

valid_u64() {
    local value=$1 nonzero=$2
    [[ $value =~ ^(0|[1-9][0-9]*)$ ]] || return 1
    ((${#value} < 20)) || { ((${#value} == 20)) && [[ $value < 18446744073709551616 ]]; } || return 1
    [[ $nonzero == 0 || $value != 0 ]]
}

valid_token() {
    local value=$1
    ((${#value} >= 1 && ${#value} <= 64)) || return 1
    [[ $value =~ ^[A-Za-z0-9][A-Za-z0-9._+-]*[A-Za-z0-9]$ || $value =~ ^[A-Za-z0-9]$ ]]
}

decimal_le() {
    local value=$1 limit=$2
    ((${#value} < ${#limit})) || { ((${#value} == ${#limit})) && [[ $value < $limit || $value == "$limit" ]]; }
}

tab_field_count() {
    local value=$1 count=1
    while [[ $value == *$'\t'* ]]; do
        value=${value#*$'\t'}
        ((count += 1))
    done
    printf '%s' "$count"
}

parse_tagged_u64() {
    local line=$1 tag=$2 value
    [[ $line == "$tag"$'\t'* ]] || return 1
    value=${line#*$'\t'}
    [[ $value != *$'\t'* ]] && valid_u64 "$value" 1
}

parse_tagged_token() {
    local line=$1 tag=$2 value
    [[ $line == "$tag"$'\t'* ]] || return 1
    value=${line#*$'\t'}
    [[ $value != *$'\t'* ]] && valid_token "$value"
}

parse_tagged_u64 "${lines[1]}" S || fail 'sequence is invalid'
parse_tagged_token "${lines[2]}" V || fail 'version is invalid'
parse_tagged_u64 "${lines[3]}" E || fail 'security epoch is invalid'
[[ ${lines[4]} == "T"$'\t'"$expected_target" ]] || fail 'target ABI is invalid'
parse_tagged_token "${lines[5]}" B || fail 'minimum bootstrap version is invalid'
parse_tagged_token "${lines[6]}" U || fail 'minimum updater version is invalid'
parse_tagged_token "${lines[7]}" C || fail 'compatibility set is invalid'
parse_tagged_token "${lines[8]}" R || fail 'source revision is invalid'
parse_tagged_u64 "${lines[9]}" D || fail 'source date epoch is invalid'

roles=(c1ancher c1pkg launcher updater)
paths=(artifacts/C1ancher artifacts/c1pkg artifacts/C1ancher-launcher artifacts/c1updater)
payload_size=0
for index in 0 1 2 3; do
    line=${lines[index + 10]}
    [[ $(tab_field_count "$line") == 6 ]] || fail 'component field count is invalid'
    IFS=$'\t' read -r record role path digest size mode <<<"$line"
    [[ $record == F && $role == "${roles[index]}" && $path == "${paths[index]}" ]] || fail 'component identity is invalid'
    [[ $digest =~ ^[0-9a-f]{64}$ ]] || fail 'component digest is invalid'
    valid_u64 "$size" 1 || fail 'component size is invalid'
    decimal_le "$size" "$max_component" || fail 'component exceeds the size limit'
    [[ $mode == 700 ]] || fail 'component mode is invalid'
    file=$release/$path
    [[ $(stat -c '%s' "$file") == "$size" ]] || fail 'component size does not match the manifest'
    actual_digest=$(sha256sum "$file")
    actual_digest=${actual_digest%% *}
    [[ $actual_digest == "$digest" ]] || fail 'component digest does not match the manifest'
    ((payload_size += size))
    ((payload_size <= max_payload)) || fail 'total payload exceeds the size limit'
done

validate_abi() {
    local file=$1 report
    report=$(readelf -h -l -A -d "$file")
    grep -Eq 'Class:[[:space:]]+ELF32' <<<"$report" || return 1
    grep -Eq "Data:[[:space:]]+2's complement, little endian" <<<"$report" || return 1
    grep -Eq 'Flags:.*o32.*mips32r2' <<<"$report" || return 1
    grep -Eq 'ISA:[[:space:]]+MIPS32r2' <<<"$report" || return 1
    grep -Eq 'FP ABI:[[:space:]]+Hard float \(double precision\)' <<<"$report" || return 1
    grep -Fq 'There is no dynamic section in this file.' <<<"$report" || return 1
    ! grep -Fq 'Requesting program interpreter' <<<"$report"
}

for path in "${paths[@]}"; do
    validate_abi "$release/$path" || fail "artifact ABI is invalid: $path"
done

temporary_der=$(mktemp)
cleanup() { rm -f -- "$temporary_der"; }
trap cleanup EXIT HUP INT TERM
openssl pkey -pubin -in "$trusted_key" -outform DER -out "$temporary_der" >/dev/null 2>&1 || fail 'trusted key is not a public key'
[[ $(wc -c <"$temporary_der") -eq 44 ]] || fail 'trusted key is not canonical Ed25519 SPKI'
[[ $(od -An -tx1 -N12 "$temporary_der" | tr -d ' \n') == 302a300506032b6570032100 ]] || fail 'trusted key is not Ed25519'
openssl pkeyutl -verify -rawin -pubin -inkey "$trusted_key" -in "$manifest" -sigfile "$signature" >/dev/null 2>&1 || fail 'manifest signature verification failed'

manifest_digest=$(sha256sum "$manifest")
manifest_digest=${manifest_digest%% *}
printf 'Core release verified: sequence=%s version=%s digest=%s\n' \
    "${lines[1]#*$'\t'}" "${lines[2]#*$'\t'}" "$manifest_digest"