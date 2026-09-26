#!/bin/bash
# lint-test-heredocs.sh — syntax-check the embedded python in tools/test-*.sh.
#
# A fixture generator inside a test suite is code, and it breaks the suite
# the same way any other code does. It also breaks it SILENTLY: a heredoc
# that does not compile kills the suite during "generate fixtures", before a
# single assertion runs, so the log ends with no FAIL line at all and the
# suite just reports a bare non-zero exit. That is exactly how a mis-indented
# `dirs.sort()` I added to 13 fixture generators (f6f4c46) took out 8 suites
# and looked like 8 unrelated mysteries.
#
#   bash tools/lint-test-heredocs.sh          # every tools/test-*.sh
#   bash tools/lint-test-heredocs.sh a.sh b.sh
#
# Exit 0 = all heredocs compile, 1 = at least one does not (each is printed
# with its file and the compile error).

set -u
cd "$(git rev-parse --show-toplevel 2>/dev/null || echo .)" || exit 2

if [ "$#" -gt 0 ]; then
    files=("$@")
else
    files=(tools/test-*.sh)
fi

[ -e "${files[0]}" ] || { echo "no such file: ${files[0]}" >&2; exit 2; }

tmp=$(mktemp -d) || exit 2
trap 'rm -rf "$tmp"' EXIT

bad=0
n=0
for f in "${files[@]}"; do
    # Extract every `python3 - <<'TAG' ... TAG` / `<<TAG ... TAG` block and
    # hand it to the interpreter's own parser. awk keeps the body verbatim,
    # including indentation, which is the whole point.
    awk -v out="$tmp" -v src="$f" '
        # only blocks handed to a python interpreter: a bare `<<TAG` is as
        # likely to be a C fixture or a php snippet as a fixture generator
        /<<-?[ \t]*[\042\047]?[A-Za-z_][A-Za-z0-9_]*[\042\047]?[ \t]*$/ {
            # capture the tag, and whether the body is <<- (strippable tabs)
            line = $0
            head = line
            sub(/<<-?[ \t]*/, "", head)      # the opener itself carries the python
            if (head !~ /python[0-9.]*/) next # ... only python heredocs are ours
            tag = line
            sub(/.*<<-?[ \t]*/, "", tag)
            sub(/[ \t]*$/, "", tag)
            gsub(/[\042\047]/, "", tag)       # bare and quoted tags both occur
            if (tag !~ /^[A-Za-z_][A-Za-z0-9_]*$/) next
            idx++
            # sanitise the NAME only, then join: sanitising the whole path
            # also rewrote the slashes in $tmp, which turned the target into
            # a relative path and dropped every extracted block into the
            # repository root (391 of them got committed).
            name = sprintf("%s.%d.py", src, idx)
            gsub(/[^A-Za-z0-9_.-]/, "_", name)
            file = out "/" name
            body = (line ~ /<<-/) ? 1 : 0
            print file > (out "/.blocks")
            print body > (out "/.tabs")
            inblock = 1
            collecting = 1
            body_file = file
            next
        }
        collecting && $0 == tag { collecting = 0; next }
        collecting {
            line = $0
            if (body) sub(/^\t+/, "", line)
            print line > body_file
        }
    ' "$f" >/dev/null 2>&1

    [ -s "$tmp/.blocks" ] || continue
    while read -r py; do
        [ -n "$py" ] && [ -f "$py" ] || continue
        n=$((n + 1))
        if ! err=$(python3 -m py_compile "$py" 2>&1); then
            bad=1
            echo "HEREDOC-SYNTAX $f :: $(basename "$py")"
            # py_compile reports "<file>: line N: <error>"; make it relative
            printf '%s\n' "$err" | sed "s|$tmp/[^:]*\.py|$f|p" | sed 's/^/    /'
        fi
    done < "$tmp/.blocks"
    rm -f "$tmp/.blocks" "$tmp/.tabs"
done

if [ "$bad" -eq 0 ]; then
    echo "test heredocs: OK ($n python block(s) compile)"
else
    echo "test heredocs: FAIL -- the blocks above do not compile; their suite" >&2
    echo "  will die during 'generate fixtures' with no FAIL line." >&2
fi
exit "$bad"
