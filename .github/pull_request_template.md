## Summary

<!-- What does this change, and why? Link the issue it closes: "Closes #123". -->

## Type of change

- [ ] Bug fix (non-breaking change that fixes an issue)
- [ ] New feature (non-breaking change that adds functionality)
- [ ] Breaking change (existing API changes behaviour or signature)
- [ ] Documentation
- [ ] Build system / CI
- [ ] Refactor or performance (no behaviour change)

## How was this tested?

<!--
Describe the tests you added and how you verified the change. Paste the
relevant `ctest` output. If this touches the event loop, parsers, or QUIC,
include sanitizer results.
-->

## Checklist

- [ ] `ctest --output-on-failure` passes locally
- [ ] I added tests covering the change (or explained below why none apply)
- [ ] I ran the suite with `-DENABLE_SANITIZERS=ON` if this touches I/O, parsing, or memory ownership
- [ ] I updated the relevant guide under `docs/`
- [ ] I added a `CHANGELOG.md` entry if this is user-visible
- [ ] My commits follow [Conventional Commits](https://www.conventionalcommits.org/)
- [ ] I read [CONTRIBUTING.md](https://github.com/varuns2903/orbit-framework/blob/main/CONTRIBUTING.md)

## Breaking changes

<!-- If you ticked "Breaking change", describe the migration path for users. -->
