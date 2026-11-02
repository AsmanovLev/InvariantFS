#!/bin/sh
# InvariantFS Quick Installer
# https://invfs.ru
set -eu
if command -v curl >/dev/null 2>&1; then
    FETCH="curl -fsSL"
elif command -v wget >/dev/null 2>&1; then
    FETCH="wget -qO-"
else
    echo "invfs: error: curl or wget required" >&2
    exit 1
fi
exec $FETCH https://invfs.ru/bootstrap.sh | sh -s -- "$@"
