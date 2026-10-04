# Release Process

How Orbit is versioned, what each release contains, how to verify a
download, and the checklist maintainers follow.

## Versioning policy

Orbit uses [Semantic Versioning](https://semver.org/) with one caveat for 1.x.

### 1.x (current): pre-stable

- **Patch** (`1.6.1`): bug and security fixes only. No API changes, no
  behaviour changes beyond the fix.
- **Minor** (`1.7.0`): new features, and — because the API is not yet
  frozen — possibly breaking changes. Every breaking change is listed under
  "Upgrade notes" in [CHANGELOG.md](../CHANGELOG.md) and explained, with
  before/after code, in [migration.md](migration.md).
- **No ABI stability.** Rebuild your application against each release; do
  not swap the shared library underneath a built binary.

### From 2.0: stable API

- Breaking changes only in major releases.
- Anything to be removed is first marked `[[deprecated("use X")]]` and
  documented, and stays for **at least one minor release** (and at least
  three months) before removal.
- ABI compatibility is kept within a major version for the shared library
  build.

### What counts as breaking

- Removing or renaming a public header, type, function, member or CMake
  target/option.
- Changing a public signature in a way that existing calls no longer
  compile, or compile with a different meaning.
- Changing documented default behaviour that applications rely on (status
  codes, headers, error handling, defaults in `ServerConfig`), unless the old
  behaviour was a security vulnerability — those are fixed in any release
  and called out in the upgrade notes.

Not breaking: adding overloads, members, options or headers; internal
classes in `detail` namespaces; anything documented as experimental
(currently HTTP/3).

## What a release contains

Each `vX.Y.Z` tag runs `.github/workflows/release.yml`, which attaches to the
GitHub release:

| File | What it is |
|---|---|
| `orbit-framework-X.Y.Z.tar.gz`, `.zip` | Source archives of the tag (`git archive`) |
| `orbit-framework-X.Y.Z.spdx.json` | SBOM (SPDX 2.3): every vcpkg dependency at the exact version and upstream source used for the release |
| `SHA256SUMS` | SHA-256 of every file above |
| `*.sigstore.json` | Sigstore signature bundle for each file, including `SHA256SUMS` |

Signatures are keyless: they are tied to the release workflow running on a
tag of this repository, and recorded in the public Rekor transparency log.

## Verifying a release

```bash
VERSION=1.6.0
# 1. Checksums
sha256sum -c SHA256SUMS --ignore-missing

# 2. Signature (install cosign: https://docs.sigstore.dev/cosign/system_config/installation/)
cosign verify-blob "orbit-framework-$VERSION.tar.gz" \
  --bundle "orbit-framework-$VERSION.tar.gz.sigstore.json" \
  --certificate-identity-regexp '^https://github\.com/varuns2903/orbit-framework/\.github/workflows/release\.yml@refs/(tags/v.*|heads/main)$' \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com
```

`Verified OK` means the file was produced by this repository's release
workflow and has not been modified since. (Artifacts rebuilt by a manual run
are signed from `main` rather than the tag, hence both in the pattern.)

## Security releases

Vulnerabilities are reported privately (see [SECURITY.md](../SECURITY.md))
and tracked as draft GitHub Security Advisories. Fixes land with neutral
commit and PR titles. Once the release containing them is published, the
advisories are published with that version as the patched version.

## Release checklist

1. **Prepare** — on a `release/vX.Y.Z` branch:
   - bump the version in `CMakeLists.txt` (`project(... VERSION X.Y.Z)`),
     `vcpkg.json`, `tools/cli/orbit` (`GIT_TAG`) and the README examples;
   - add the `CHANGELOG.md` section, with "Upgrade notes" for anything
     breaking, and update [migration.md](migration.md);
   - open a PR; all workflows must pass.
2. **Tag** — after merging, from up-to-date `main`:
   ```bash
   git tag -a vX.Y.Z -m "vX.Y.Z"
   git push origin vX.Y.Z
   ```
   The release workflow refuses a tag that does not match the version in
   `CMakeLists.txt`.
3. **Review the draft** — the workflow creates a draft release with the
   changelog section and all artifacts. Download one archive and run the
   verification steps above, then publish the draft.
4. **Follow-ups**:
   - update `packaging/vcpkg-port` (version and the `SHA512` of the new tag
     archive);
   - publish any security advisories fixed in this release;
   - announce breaking changes where users will see them.

If the workflow failed or a tag needs its artifacts rebuilt, run
*Release Artifacts* manually (Actions → Release Artifacts → Run workflow)
with the tag name; it replaces the assets on the existing release.
