#!/bin/sh
echo "\${INVFS_PROFILE:-unset}" > "$WORK/profseen"
cp "\$1" "\$2"
