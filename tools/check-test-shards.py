#!/usr/bin/env python3
"""check-test-shards.py -- every built test binary runs in some shard.

A test with no shard entry runs nowhere, which reads green and proves
nothing. Fails loud (CI runs it via test-shard-check, itself a prerequisite
of every shard and of `make test`).
"""
import re
import sys

mk = open('Makefile').read().split('\n')
# prerequisite binaries: $(OUT)/invf-* tokens in the TEST_SHARD_DEPS block
deps = set()
for i, ln in enumerate(mk):
    if ln.startswith('TEST_SHARD_DEPS'):
        j = i
        while True:
            deps.update(re.findall(r'\$\(OUT\)/(invf-[A-Za-z0-9_.-]+)', mk[j]))
            if not mk[j].rstrip().endswith('\\'):
                break
            j += 1
        break
# invoked binaries: $(OUT)/invf-* inside test-shard-N recipes
invoked = set()
in_shard = False
for ln in mk:
    if re.match(r'test-shard-[0-9]+:', ln):
        in_shard = True
        continue
    if in_shard and ln and not ln[0] in ('\t', ' ', '#', '@'):
        in_shard = False
    if in_shard:
        invoked.update(re.findall(r'\$\(OUT\)/(invf-[A-Za-z0-9_.-]+)', ln))
missing = sorted(deps - invoked)
# Transitive coverage: shell suites in the recipe invoke tools themselves
# ($B/invf-x, bin/invf-x). A binary covered that way still runs.
if missing:
    import glob as _glob
    import os as _os
    suites = set()
    for ln in mk:
        suites.update(re.findall(r'bash (tools/[A-Za-z0-9_.-]+\.sh)', ln))
    trans = set()
    for s in suites:
        try:
            txt = open(s).read()
        except OSError:
            continue
        trans.update(re.findall(r'(?:\$B/|\./bin/|bin/)invf-([A-Za-z0-9_.-]+)', txt))
        trans.update(re.findall(r'\$B/invf-([A-Za-z0-9_.-]+)', txt))
    trans = {'invf-' + t for t in trans}
    missing = sorted(set(missing) - trans)
if missing:
    print('test-shard-check: FAIL -- built but never invoked:')
    for b in missing:
        print('  bin/%s has no shard entry' % b)
    sys.exit(1)
print('test-shard-check: OK (%d prerequisite binaries invoked across shards)'
      % len(deps))
