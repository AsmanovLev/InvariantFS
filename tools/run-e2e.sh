#!/bin/bash
# run-e2e.sh <test-script.sh> [args...] — run an e2e suite under the global
# e2e lock. Concurrent agents/worktrees share /dev/shm and test images use
# fixed names, so parallel e2e runs interfere; serialize them (winner runs,
# losers wait). Unit tests do not need this lock.
# Usage: bash tools/run-e2e.sh tools/test-foo.sh
exec flock -w 7200 /tmp/invfs-e2e.lock bash "$@"
