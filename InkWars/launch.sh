#!/bin/sh
# c1pkg payload is sealed read-only; all mutable state belongs in /usr/data.
set -eu
APP_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
umask 077
mkdir -p /usr/data/inkwars
cd /usr/data/inkwars
exec "$APP_DIR/inkwars" "$@"
