#!/bin/bash
# watch-ci.sh -- block until the newest engine-ci run finishes, then print
# exactly the lines worth reading. Exists because most of this session went
# wrong the same way: the agent asked the human to paste a CI log, or read a
# STALE one, and then reasoned about symptoms instead of evidence.
#
#   tools/watch-ci.sh [--fail-fast] [minutes]     default 45
#
# Fail-early: every poll lists per-job conclusions, and a job that concludes
# "failure" is printed the moment it is seen ("!! FAIL: <job> | <steps>"),
# not at the end of the run. With --fail-fast the watcher exits 1 right
# then -- the bg notification wakes the agent while the other jobs are
# still running, so triage starts minutes earlier. Without it the watcher
# still prints early failures but waits for the run to complete, so one
# run's full failure set stays in one report.
#
# It resolves the run by WORKFLOW, not by "latest run" -- `per_page=1` on
# actions/runs returns the newest run of ANY workflow, which once handed this
# script a CodeQL "Analyze (python)" log and produced an hour of confusion.
set -uo pipefail

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
T="${GITHUB_TOKEN:-$(tr -d '\n' < "$HOME/.gh-token" 2>/dev/null)}"
[ -n "$T" ] || { echo "no token: set GITHUB_TOKEN or ~/.gh-token" >&2; exit 1; }
WF="engine-ci.yml"
API="https://api.github.com/repos/AsmanovLev/InvariantFS/actions"
FAILFAST=0
if [ "${1:-}" = "--fail-fast" ]; then FAILFAST=1; shift; fi
DEADLINE=$(( $(date +%s) + ${1:-45} * 60 ))

runs() { curl -sS --noproxy '*' --max-time 30 -H "Authorization: Bearer $T" -H "Accept: application/vnd.github+json" \
           "$API/workflows/$WF/runs?per_page=1"; }
top() { python3 -c 'import json,sys
d=json.load(sys.stdin)
r=d["workflow_runs"][0]
print(r["id"], r["status"], r["conclusion"] or "-", r["head_sha"][:7])' ; }

START=$(runs | top | cut -d' ' -f1)
[ -n "$START" ] || { echo "could not read the workflow's latest run" >&2; exit 1; }
echo "watching $WF from run $START (up to ${1:-45}m)"

run_status() { curl -sS --noproxy '*' --max-time 30 -H "Authorization: Bearer $T" -H "Accept: application/vnd.github+json" \
           "$API/runs/$START" | python3 -c 'import json,sys
d=json.load(sys.stdin)
print(d["status"], d["conclusion"] or "-", d["head_sha"][:7])'; }
# One line per FAILED job: "<job> | <failed steps>". Empty when none yet.
run_fails() { curl -sS --noproxy '*' --max-time 30 -H "Authorization: Bearer $T" -H "Accept: application/vnd.github+json" \
           "$API/runs/$START/jobs?per_page=30" | python3 -c 'import json,sys
for j in json.load(sys.stdin).get("jobs", []):
    if j.get("conclusion")=="failure":
        bad=[s["name"] for s in j.get("steps",[]) if s.get("conclusion")=="failure"]
        print(j["name"]+" | "+", ".join(bad))'; }
SEENFAIL=""
while :; do
  sleep 20
  if [ "$(date +%s)" -ge "$DEADLINE" ]; then
    echo "watcher: deadline reached, run $START not completed" >&2
    exit 2
  fi
  FRESH=0
  while IFS= read -r line; do
    [ -n "$line" ] || continue
    case "$SEENFAIL" in
      *"$line"*) ;;
      *) echo "  !! FAIL: $line"; SEENFAIL="$SEENFAIL$line;"; FRESH=1 ;;
    esac
  done < <(run_fails)
  if [ "$FAILFAST" = 1 ] && [ -n "$SEENFAIL" ]; then
    echo "watcher: fail-fast exit on run $START (other jobs still running)"
    exit 1
  fi
  [ "$FRESH" = 1 ] && continue
  set -- $(run_status)
  STATUS=$1; CONC=$2; SHA=$3; RID=$START
  case "$STATUS" in
    completed) ;;
    *) echo "  $SHA $STATUS ..."; continue ;;
  esac
  echo
  echo "=== $WF $SHA -> ${CONC:-unknown} (run $RID)"

  curl -sS --noproxy '*' --max-time 30 -H "Authorization: Bearer $T" -H "Accept: application/vnd.github+json" \
    "$API/runs/$RID/jobs" | python3 -c 'import json,sys
for j in json.load(sys.stdin).get("jobs", []):
    bad=[s["name"] for s in j.get("steps",[]) if s.get("conclusion")=="failure"]
    print("  %-15s %-9s %s" % (j["name"], j["conclusion"], " | ".join(bad)))'

  if [ "${CONC:-}" != "success" ]; then
    TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
    curl -sS --noproxy '*' --max-time 120 -H "Authorization: Bearer $T" -H "Accept: application/vnd.github+json" \
      "$API/runs/$RID/logs" -o "$TMP/l.zip" 2>/dev/null
    ( cd "$TMP" && unzip -qqo l.zip 2>/dev/null || tar xf l.zip 2>/dev/null )
    echo "--- failures and their neighbourhood ---"
    grep -rhnE 'FAIL|Error [0-9]+|error:|Permission denied|No such file|cannot|undefined reference' "$TMP" 2>/dev/null \
      | grep -viE 'E: Unable|apt-get|cite: WARN|\| FAIL' | head -40 | sed 's/^/  /'
    echo "--- (artifacts: $API/runs/$RID/artifacts ) ---"
  fi
  [ "$CONC" = "success" ] && exit 0
  [ "$CONC" = "failure" ] && exit 1
  exit 2
done
