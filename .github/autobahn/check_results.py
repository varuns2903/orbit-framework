#!/usr/bin/env python3
"""Fails if any Autobahn case did not pass.

Usage: check_results.py [--expect N] INDEX_JSON...  (one index.json per wstest run)

With --expect, it also fails unless exactly N cases ran, so a section left out
of a batched run is noticed rather than silently untested.

OK, NON-STRICT (allowed by RFC 6455, e.g. invalid UTF-8 detected at the end of
a fragmented message rather than mid-way) and INFORMATIONAL count as passing.
"""
import collections
import json
import sys

PASSING = {"OK", "NON-STRICT", "INFORMATIONAL"}

args = sys.argv[1:]
expect = None
if args[:1] == ["--expect"]:
    expect = int(args[1])
    args = args[2:]
if not args:
    sys.exit("usage: check_results.py [--expect N] INDEX_JSON...")

failed = []
counts = collections.Counter()
for path in args:
    results = json.load(open(path))
    for agent, cases in results.items():
        for case_id, result in cases.items():
            counts[result["behavior"]] += 1
            if result["behavior"] not in PASSING or result["behaviorClose"] not in PASSING:
                failed.append(f"{agent} {case_id}: {result['behavior']} / close {result['behaviorClose']}")

total = sum(counts.values())
print(f"{total} cases from {len(args)} runs: {dict(counts)}")
if expect is not None and total != expect:
    print(f"Expected {expect} cases, but {total} ran.")
    sys.exit(1)
if failed:
    print("Failed cases:")
    print("\n".join(sorted(failed)))
    sys.exit(1)
print("All cases passed.")
