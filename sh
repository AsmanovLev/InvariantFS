#!/bin/sh
# InvariantFS Quick Installer
# https://invfs.ru
set -eu
exec curl -fsSL https://github.com/AsmanovLev/InvariantFS/releases/latest/download/bootstrap.sh | sh -s -- "$@"
