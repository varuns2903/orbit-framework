#!/usr/bin/env python3
"""Combines vcpkg's per-package SBOMs into one SPDX 2.3 document for a release.

vcpkg writes share/<port>/vcpkg.spdx.json for every installed port, with the
exact port version and the upstream sources it was built from. This merges
them under a root package for Orbit itself.

usage: merge_sbom.py <vcpkg_installed/<triplet>> <version> <output.spdx.json>
"""
import datetime
import glob
import json
import os
import re
import sys

REPO = "https://github.com/varuns2903/orbit-framework"


def spdx_id(*parts):
    return "SPDXRef-" + "-".join(re.sub(r"[^A-Za-z0-9.]+", ".", p) for p in parts)


def main():
    installed, version, output = sys.argv[1], sys.argv[2], sys.argv[3]
    files = sorted(glob.glob(os.path.join(installed, "share", "*", "vcpkg.spdx.json")))
    if not files:
        sys.exit(f"no vcpkg.spdx.json files under {installed}/share")

    root = spdx_id("orbit-framework")
    packages = [{
        "name": "orbit-framework",
        "SPDXID": root,
        "versionInfo": version,
        "downloadLocation": f"git+{REPO}@v{version}",
        "licenseConcluded": "MIT",
        "licenseDeclared": "MIT",
        "copyrightText": "Copyright (c) 2026 Orbit Framework Contributors",
        "supplier": "Organization: Orbit Framework Contributors",
    }]
    relationships = [{
        "spdxElementId": "SPDXRef-DOCUMENT",
        "relationshipType": "DESCRIBES",
        "relatedSpdxElement": root,
    }]

    for path in files:
        doc = json.load(open(path))
        port = next((p for p in doc["packages"] if p["SPDXID"] == "SPDXRef-port"), None)
        if port is None:
            continue
        port_id = spdx_id(port["name"])
        packages.append({
            "name": port["name"],
            "SPDXID": port_id,
            "versionInfo": port.get("versionInfo", "NOASSERTION"),
            "downloadLocation": port.get("downloadLocation", "NOASSERTION"),
            "licenseConcluded": port.get("licenseConcluded", "NOASSERTION"),
            "licenseDeclared": port.get("licenseDeclared", "NOASSERTION"),
            "copyrightText": "NOASSERTION",
            "comment": "vcpkg port",
        })
        relationships.append({
            "spdxElementId": root,
            "relationshipType": "DEPENDS_ON",
            "relatedSpdxElement": port_id,
        })
        # Upstream sources the port was built from.
        for i, res in enumerate(p for p in doc["packages"] if p["SPDXID"].startswith("SPDXRef-resource")):
            res_id = spdx_id(port["name"], "source", str(i))
            packages.append({
                "name": res["name"],
                "SPDXID": res_id,
                "versionInfo": res.get("versionInfo") or "NOASSERTION",
                "downloadLocation": res.get("downloadLocation", "NOASSERTION"),
                "licenseConcluded": res.get("licenseConcluded", "NOASSERTION"),
                "licenseDeclared": res.get("licenseDeclared", "NOASSERTION"),
                "copyrightText": "NOASSERTION",
                **({"checksums": res["checksums"]} if res.get("checksums") else {}),
            })
            relationships.append({
                "spdxElementId": port_id,
                "relationshipType": "GENERATED_FROM",
                "relatedSpdxElement": res_id,
            })

    now = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    sbom = {
        "spdxVersion": "SPDX-2.3",
        "dataLicense": "CC0-1.0",
        "SPDXID": "SPDXRef-DOCUMENT",
        "name": f"orbit-framework-{version}",
        "documentNamespace": f"{REPO}/releases/v{version}/sbom",
        "creationInfo": {
            "created": now,
            "creators": ["Tool: orbit-framework/.github/release/merge_sbom.py", "Tool: vcpkg"],
        },
        "packages": packages,
        "relationships": relationships,
    }
    with open(output, "w") as f:
        json.dump(sbom, f, indent=2)
    print(f"{output}: orbit-framework {version} with {len(files)} vcpkg ports")


if __name__ == "__main__":
    main()
