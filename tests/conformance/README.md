# The conformance cases of the xpkg format

Each `*.lua` file here is one case: a small index (its recipes, libs and
declarations), and what an implementation must give for it. The cases are
the executable half of the specification (`docs/spec/xpkg.md`); every chapter
of it has cases here.

They are embedded in libxpkg (`build.mcpp`), so every implementation runs the
same ones:

- libxpkg's tests (`tests/test_conformance.cpp`) read each recipe, resolve its
  resources and run its hooks through the executor, on Linux, macOS and Windows;
- a host runs each case that has `install` through its own whole install and
  checks the payload against `payload`. xlings does (`tests/e2e/test_xpkg_conformance.cpp`).

A host reads the cases with `import mcpplibs.xpkg.conformance;`:
`conformance::cases()` gives them, `materialize(case, dir)` writes the case's
index under `dir`, `unmet(expectations, install_dir)` says what does not hold.
An implementation that does not link libxpkg reads the files here.

## A case

A case file returns a table:

| Field | Meaning |
|---|---|
| `chapter` | the specification's chapter it belongs to: `versions`, `package`, `xpm`, `lifecycle`, `environment`, `declarations`, `index` |
| `summary` | what it shows, one line |
| `package` | the recipe under test: `pkgs/<first letter>/<package>.lua` of `files` |
| `files` | the index: relative path → content |
| `loaded` | fields of the loaded package table and their text: `name`, `spec`, `description`, `type`, `status`, `archs` and `programs` (comma-joined in order), `platforms` (sorted, comma-joined). `error` instead: the recipe does not load, and its error contains this |
| `executor_error` | no executor is made for the recipe; the error contains this |
| `resolve` | resolutions: `{ platform, arch, version [, url] [, sha256] [, strip] }`, or with `error`: resolving fails and the error contains it. A field left out is not checked |
| `hooks` | hooks run in order on one install directory: `{ "<hook>" [, ok = false] [, error = "..."] [, after = {...}] [, notices = {...}] [, call_failures = n] }` |
| `install` | what a host's whole install of the package ends with on any platform: `installed`, `failed` (a hook fails) or `refused` (no hook runs) |
| `payload` | with `installed`: what the install directory holds after the host's install |

An expectation (`after`, `payload`) is `{ "<path relative to the install directory>", "file" | "dir" | "symlink" | "absent" [, text = "<exact content>"] }`.
A notice is `{ "warn" | "error", "<part of its text>" }`.

Recipes of a case are written for every platform (`linux`, `macosx`,
`windows`) unless the case is about one, use no network, and need no program
of the host.
