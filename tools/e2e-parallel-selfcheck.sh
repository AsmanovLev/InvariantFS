#!/bin/bash
set -e
sleep 1
echo "$$" > /dev/shm/selfcheck.marker
sleep 2
got=$(cat /dev/shm/selfcheck.marker)
[ "$got" = "$$" ] || { echo "SHM COLLISION: $$ vs $got"; exit 1; }
echo "ISOLATED_OK pid=$$"
