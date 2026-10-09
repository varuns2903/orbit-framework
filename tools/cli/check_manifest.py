#!/usr/bin/env python3
"""Checks that the manifest `orbit new` writes matches Orbit's vcpkg.json.

A scaffolded project lists Orbit's dependencies itself (FetchContent builds
Orbit inside the project, against the project's manifest). Its list must be
the core dependencies plus those of Orbit's default features, at the same
builtin-baseline, or new projects fail to configure (#186).
"""
import importlib.machinery
import importlib.util
import json
import pathlib
import sys

sys.dont_write_bytecode = True  # importing the CLI must not leave __pycache__ behind

root = pathlib.Path(__file__).resolve().parents[2]
loader = importlib.machinery.SourceFileLoader("orbit_cli", str(root / "tools/cli/orbit"))
spec = importlib.util.spec_from_loader("orbit_cli", loader)
cli = importlib.util.module_from_spec(spec)
loader.exec_module(cli)

manifest = json.loads((root / "vcpkg.json").read_text())


def key(dep):
    return dep if isinstance(dep, str) else dep["name"]


def normalise(dep):
    return json.dumps(dep, sort_keys=True) if not isinstance(dep, str) else json.dumps({"name": dep})


expected = {key(d): d for d in manifest["dependencies"]}
for feature in manifest.get("default-features", []):
    for d in manifest["features"][feature]["dependencies"]:
        expected.setdefault(key(d), d)

actual = {key(d): d for d in cli.ORBIT_DEPENDENCIES}
problems = []
if cli.ORBIT_BUILTIN_BASELINE != manifest["builtin-baseline"]:
    problems.append(f"builtin-baseline: CLI {cli.ORBIT_BUILTIN_BASELINE}, vcpkg.json {manifest['builtin-baseline']}")
for name in sorted(set(expected) - set(actual)):
    problems.append(f"missing from the CLI: {name}")
for name in sorted(set(actual) - set(expected)):
    problems.append(f"not in vcpkg.json (core or default features): {name}")
for name in sorted(set(expected) & set(actual)):
    if normalise(expected[name]) != normalise(actual[name]):
        problems.append(f"{name}: CLI {actual[name]}, vcpkg.json {expected[name]}")

if problems:
    print("tools/cli/orbit ORBIT_DEPENDENCIES / ORBIT_BUILTIN_BASELINE are out of date:")
    for p in problems:
        print("  - " + p)
    sys.exit(1)
print(f"CLI manifest matches vcpkg.json ({len(actual)} dependencies)")
