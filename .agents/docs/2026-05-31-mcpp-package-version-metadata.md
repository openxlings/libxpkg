# mcpp Package Version Metadata Follow-up

> Date: 2026-05-31 | Status: follow-up required

## Context

While validating xlings with `mcpp build`, the installed
`mcpplibs.xpkg@0.0.41` package was found to contain stale mcpp metadata:

```toml
[package]
namespace = "mcpplibs"
name = "xpkg"
version = "0.0.39"
```

The archive itself is installed under:

```text
~/.mcpp/registry/data/xpkgs/mcpplibs-x-mcpplibs.xpkg/0.0.41/libxpkg-0.0.41
```

## Impact

mcpp currently builds dependency cache keys from the package manifest name and
version. Because the embedded version is `0.0.39`, the BMI cache entry becomes:

```text
~/.mcpp/bmi/<fingerprint>/deps/mcpplibs/xpkg@0.0.39
```

The consuming project still depends on:

```text
mcpplibs.xpkg v0.0.41
```

This mismatch makes mcpp status output and cache identity harder to reason
about. mcpp should be hardened against stale embedded metadata, but libxpkg
should also keep release package metadata aligned with the release tag.

## Tasks

- [ ] Ensure the next libxpkg release updates `[package].version`.
- [ ] Decide whether `[package].name` should remain short (`xpkg`) plus
      `namespace = "mcpplibs"` or become fully qualified.
- [ ] Add a lightweight release check that compares the release tag/version
      with the mcpp manifest version before publishing.
- [ ] After the next release, update the mcpp package index entry if needed.

## Coordination

The immediate mcpp PR should not depend on republishing `0.0.41`; it should use
the index-resolved dependency identity for cache and UI behavior. This libxpkg
task prevents the same mismatch from recurring in later package releases.
