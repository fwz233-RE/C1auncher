#!/usr/bin/env bash
set -Eeuo pipefail
IFS=$'\n\t'
umask 022

usage() {
    cat >&2 <<'EOF'
Usage: sudo setup-app-repo.sh --server-name HOST [--listen PORT] [--publisher-user USER]

Creates a separate Nginx virtual host on an already-used or new TCP port. It does
not edit, disable, or replace any existing virtual host or listen directive.
EOF
    exit 2
}

server_name=
listen_port=80
publisher_user="${SUDO_USER:-root}"
while (($#)); do
    case "$1" in
        --server-name) (($# >= 2)) || usage; server_name=$2; shift 2 ;;
        --listen) (($# >= 2)) || usage; listen_port=$2; shift 2 ;;
        --publisher-user) (($# >= 2)) || usage; publisher_user=$2; shift 2 ;;
        -h|--help) usage ;;
        *) usage ;;
    esac
done

[[ ${EUID} -eq 0 ]] || { echo 'Run this script as root.' >&2; exit 1; }
[[ $server_name =~ ^[A-Za-z0-9.-]+$ && $server_name != .* && $server_name != *. ]] || {
    echo 'The server name must be a DNS name or IPv4 address.' >&2; exit 1;
}
[[ $listen_port =~ ^[0-9]+$ ]] && ((listen_port >= 1 && listen_port <= 65535)) || {
    echo 'The listen port must be between 1 and 65535.' >&2; exit 1;
}
[[ $publisher_user =~ ^[A-Za-z_][A-Za-z0-9_-]*$ ]] || { echo 'Invalid publisher user.' >&2; exit 1; }
id "$publisher_user" >/dev/null 2>&1 || { echo 'Publisher user does not exist.' >&2; exit 1; }
if ! command -v nginx >/dev/null 2>&1; then
    command -v apt-get >/dev/null 2>&1 || { echo 'Nginx is missing and apt-get is unavailable.' >&2; exit 1; }
    export DEBIAN_FRONTEND=noninteractive
    apt-get update
    apt-get install -y --no-install-recommends nginx
fi
for command in nginx systemctl install ln readlink mktemp cmp; do
    command -v "$command" >/dev/null 2>&1 || { echo "Required command is missing: $command" >&2; exit 1; }
done

repo_root=/srv/c1repo
releases=$repo_root/releases
staging=$repo_root/staging
available=/etc/nginx/sites-available/c1-app-repo
enabled=/etc/nginx/sites-enabled/c1-app-repo

install -d -m 0755 -o "$publisher_user" -g "$publisher_user" "$repo_root" "$releases" "$staging"
for path in "$repo_root" "$releases" "$staging"; do
    [[ ! -L $path && $(readlink -f -- "$path") == "$path" ]] || {
        echo "Repository path is not a real directory: $path" >&2; exit 1;
    }
done

candidate=$(mktemp)
cleanup() { rm -f -- "$candidate"; }
trap cleanup EXIT
cat >"$candidate" <<EOF
# Managed exclusively by setup-app-repo.sh.
server {
    listen ${listen_port};
    listen [::]:${listen_port};
    server_name ${server_name};

    location = /c1/v1 {
        return 308 /c1/v1/;
    }

    location ^~ /c1/v1/ {
        alias /srv/c1repo/current/;
        autoindex off;
        default_type application/octet-stream;
        etag on;
        if_modified_since exact;
        add_header X-Content-Type-Options nosniff always;
        limit_except GET HEAD { deny all; }
    }
}
EOF

created_available=0
created_enabled=0
if [[ -e $available ]]; then
    [[ ! -L $available && -f $available ]] || { echo "Refusing unexpected config path: $available" >&2; exit 1; }
    cmp -s -- "$candidate" "$available" || {
        echo "$available exists with different content; refusing to overwrite it." >&2; exit 1;
    }
else
    install -m 0644 -o root -g root "$candidate" "$available"
    created_available=1
fi

if [[ -e $enabled || -L $enabled ]]; then
    [[ -L $enabled && $(readlink -f -- "$enabled") == "$available" ]] || {
        echo "$enabled exists but does not enable the managed site." >&2; exit 1;
    }
else
    ln -s "$available" "$enabled"
    created_enabled=1
fi

if ! nginx -t; then
    ((created_enabled == 0)) || rm -f -- "$enabled"
    ((created_available == 0)) || rm -f -- "$available"
    echo 'Nginx rejected the new virtual host; the new configuration was rolled back.' >&2
    exit 1
fi

if systemctl is-active --quiet nginx; then
    nginx_action=(systemctl reload nginx)
else
    nginx_action=(systemctl enable --now nginx)
fi
if ! "${nginx_action[@]}"; then
    ((created_enabled == 0)) || rm -f -- "$enabled"
    ((created_available == 0)) || rm -f -- "$available"
    if ((created_enabled != 0 || created_available != 0)); then
        nginx -t >/dev/null 2>&1 && systemctl reload nginx >/dev/null 2>&1 || true
    fi
    echo 'Nginx reload failed; newly created configuration was rolled back.' >&2
    exit 1
fi
printf 'C1 application repository is configured at /c1/v1/ for server name %s.\n' "$server_name"
printf 'Publish a signed release to create /srv/c1repo/current atomically.\n'