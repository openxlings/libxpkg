# patchelf 0.18.0 op-order corruption on compact ELFs

**Date**: 2026-05-03
**Affects**: `mcpplibs-xpkg` ≤ v0.0.36 — any consumer using `elfpatch` on small ELFs (~280 KB or smaller, e.g. ninja 1.12.1 at 273 KB).
**Reported by**: mcpp sandbox bring-up — `g++` segfault diagnosis traced back to `xlings install ninja`.
**Fix**: ship in v0.0.37 — reverse op order in `_patch_elf_executables` and `_patch_elf` fallback so `--set-rpath` runs before `--set-interpreter`.

## Symptom

After `xlings install ninja` on a fresh sandbox using `xlings 0.4.12` (predicate-driven elfpatch enabled by default), the resulting ninja binary segfaults at execve+1:

```
$ ninja --version
Segmentation fault (core dumped)
```

`readelf -a` on the broken binary shows:

- ✅ `PT_INTERP` correct (points at sandbox `xim-x-glibc/2.39/lib64/ld-linux-x86-64.so.2`)
- ❌ `RUNPATH` empty — but xlings's elfpatch DID call `--set-rpath`
- ❌ `DT_NEEDED` entries missing — dynamic section corrupted
- File size 273 KB → 282 KB (patchelf grew it ~8 KB)

## Reproduction

Use the patchelf shipped via `xim:patchelf@0.18.0` and a fresh ninja 1.12.1 binary from upstream GitHub release. No xlings code involved — this is a pure patchelf characterization.

| # | patchelf invocation(s) | Result |
|---|---|---|
| T1 | `--set-interpreter <loader>` only | ✅ binary OK |
| T2 | `--set-rpath <dir>` only | ✅ binary OK |
| T3 | `--set-rpath <dir>` then `--set-interpreter <loader>` (two invocations) | ✅ binary OK |
| **T4** | `--set-interpreter <loader>` then `--set-rpath <dir>` (two invocations) | ❌ **segfault @ execve+1** |
| **T5** | `--set-interpreter <loader> --set-rpath <dir>` (single command) | ❌ **segfault @ execve+1** |
| **T6** | `--set-rpath <dir> --set-interpreter <loader>` (single command, reversed CLI args) | ❌ **segfault @ execve+1** |
| T8 | After T4 corruption, run `--shrink-rpath` or `--set-rpath <dir>` again to "fix" | ❌ unrecoverable |

Key observations:

- **T5 vs T6**: putting `--set-rpath` first in CLI args does NOT help. `patchelf` processes interp before rpath internally regardless of the CLI arg order, so combined-command form is equivalent to T4.
- **T7** (not in matrix): the same wrong order on a larger binary like `openssl` (~700 KB) leaves the binary intact. The bug only triggers on compact ELFs whose program-header padding is too tight to absorb the `.interp` section growth.
- **T8**: once corrupted, no patchelf operation recovers the binary. DT_NEEDED is gone; the loader can't resolve symbols at all.

## Why this only surfaces under `xlings 0.4.12+`

The 0.4.10-era xlings did **not** auto-patchelf consumer binaries. Migration to predicate-driven elfpatch (xlings 0.4.11, then fixed in 0.4.12) means xlings now runs patchelf on **every** ELF in install_dir whenever a runtime dep declares `exports.runtime.loader` (e.g. ninja → glibc). Compact binaries that were previously left alone now trip the order bug.

The contributor reporting this (mcpp sandbox) had been independently running their own `patchelf_walk` as a second pass for years — when the second pass ran on bins that 0.4.10 had left untouched, only their pass hit ninja and they used the same wrong order. So the bug was reachable before too, just less visible because 0.4.10 + their patch_walk hit fewer binaries.

## Root cause (patchelf internal)

`patchelf 0.18.0` processes `--set-interpreter` first within its operation pipeline:

1. `--set-interpreter` extends the `.interp` section (longer absolute path → larger string). To fit, patchelf appends a new `PT_LOAD` segment at file tail and rewrites program headers.
2. `--set-rpath` then attempts to add a `DT_RUNPATH` entry to the dynamic section. It computes the dynamic section's file offset based on assumptions stale after step 1's program-header rewrite.
3. The rpath write lands at a wrong offset, overwriting `DT_NEEDED` entries — causing the loader to crash trying to walk a malformed dynamic table.

For binaries with comfortable padding (most release binaries > 500 KB), step 1's PT_LOAD addition fits without disturbing later structures, and step 2's stale offset still happens to be correct. For compact binaries like ninja, the padding is exhausted and offsets shift.

The order-sensitive nature is patchelf's robustness gap — see [NixOS/patchelf](https://github.com/NixOS/patchelf) issue tracker for similar reports across versions. We don't fix patchelf here; we work around it.

## Fix

In `src/lua-stdlib/xim/libxpkg/elfpatch.lua`, reverse op order in two places:

### `_patch_elf_executables` (declarative bins/libs mode)

```diff
-if loader and _has_pt_interp(filepath, patch_tool) then
-    ok = _exec_ok(... " --set-interpreter " ...)
-end
-if ok and rpath and rpath ~= "" then
-    ok = _exec_ok(... " --set-rpath " ...)
+if rpath and rpath ~= "" then
+    ok = _exec_ok(... " --set-rpath " ...)
+end
+if ok and loader and _has_pt_interp(filepath, patch_tool) then
+    ok = _exec_ok(... " --set-interpreter " ...)
 end
```

### `_patch_elf` fallback (full-scan mode)

Same reversal applied to the fallback branch.

### Why reverse-order avoids the bug

When `--set-rpath` runs first:

1. rpath modification touches only the dynamic section (DT_RUNPATH entry). Program headers stay intact.
2. `--set-interpreter` then extends `.interp` and appends a PT_LOAD. Even if program-header layout shifts, no further patchelf op runs after this — so no stale-offset write occurs.

Verified empirically (T3 in the matrix): rpath-first survives ninja 1.12.1 cleanly.

## Regression test

`tests/test_executor.cpp::ApplyElfpatchAuto_LinuxRpathBeforeInterpreter`:

- Stub `patchelf` shell script logs every invocation
- Run `apply_elfpatch_auto` against a minimal ELF
- Read log, assert `--set-rpath` line index < `--set-interpreter` line index

If a future change accidentally reverts the order, this test fails immediately with a clear message.

## Cross-platform scope

| Platform | Affected | Reason |
|---|---|---|
| Linux | YES (this fix) | patchelf-based path |
| macOS | NO | uses `install_name_tool` which has no equivalent op-order interaction |
| Windows | NO | no patchelf path; `M._apply` early-bails |

## Ship plan

1. Land this fix as libxpkg PR (this branch)
2. Tag `v0.0.37` after merge
3. Register in `mcpplibs-index`
4. Bump xlings's `add_requires("mcpplibs-xpkg X")` in a follow-up PR (no xlings release yet — defer to next 0.4.x release window)

## Upstream patchelf

This is patchelf's robustness gap. Filing an upstream bug is out-of-scope for this PR but worth doing — note that the trigger isn't just "compact binary" but also "two sequential ops where one mutates program headers". A defensive patchelf would re-resolve dynamic-section offsets after every PT_LOAD modification.
