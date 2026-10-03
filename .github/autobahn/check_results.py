#!/usr/bin/env python3
"""Fails if any Autobahn case did not pass.

OK, NON-STRICT (allowed by RFC 6455, e.g. invalid UTF-8 detected at the end of
a fragmented message rather than mid-way) and INFORMATIONAL count as passing.
"""
import collections
import json
import sys

PASSING = {"OK", "NON-STRICT", "INFORMATIONAL"}

results = json.load(open(sys.argv[1]))
failed = []
counts = collections.Counter()
for agent, cases in results.items():
    for case_id, result in cases.items():
        counts[result["behavior"]] += 1
        if result["behavior"] not in PASSING or result["behaviorClose"] not in PASSING:
            failed.append(f"{agent} {case_id}: {result['behavior']} / close {result['behaviorClose']}")

print(dict(counts))
if failed:
    print("Failed cases:")
    print("\n".join(sorted(failed)))
    sys.exit(1)
print("All cases passed.")
