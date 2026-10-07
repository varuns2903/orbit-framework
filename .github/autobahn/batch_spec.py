#!/usr/bin/env python3
"""Prints a fuzzingclient spec for one section of the suite.

Usage: batch_spec.py SECTION   (e.g. "6" or "12.2")

It is fuzzingclient.json with "cases" narrowed to SECTION.* and the report
written to /reports/servers/SECTION, so every section gets its own
index.json. The exclusions in fuzzingclient.json still apply.
"""
import json
import os
import sys

section = sys.argv[1]
here = os.path.dirname(os.path.abspath(__file__))
spec = json.load(open(os.path.join(here, "fuzzingclient.json")))
spec["cases"] = [f"{section}.*"]
spec["outdir"] = f"/reports/servers/{section}"
json.dump(spec, sys.stdout, indent=2)
