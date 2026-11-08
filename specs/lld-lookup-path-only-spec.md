# lld-lookup-path-only — the compiler looks for `ld.lld` only on PATH and silently drops LTO (defect spec)

> Status: **fixed on cajeta main 2026-11-08**, unreleased (test/cli/LldLookupTests.cpp);
> filed 2026-10-25 by benchmark-reference units 5, 9 and 15 on v0.46.3. On proton the
> defect was sharper than §1: PATH's `ld.lld` was ROCm's (LLVM 22) and the link failed
> on "Invalid summary version 13" instead of dropping LTO. Use case 2.2's fail-by-name
> for an explicit `--lto=thin` is not done: the flag does not record whether the
> release flavor or the developer set it, so both fall back with the paths looked in.

## 1. Definition
The release flavor defaults to `--lto=thin`. When `ld.lld` is not on PATH the
compiler prints "needs lld … building without LTO" and links without it; the
build succeeds and the binary is less optimized. On Phoenix the fork's
`ld.lld` sits in `cajeta-llvm/llvm-install/bin`, next to the clang the
compiler was built with, and is not on PATH in a non-login shell, so every
release build the units made without putting that directory first lost LTO.
A warning on a default setting that changes the output's performance is a
silent downgrade.

## 2. Use cases
- **2.1** As the compiler, when LTO is requested or defaulted, then I find the
  linker beside my own installation (the configured `LLVM_DIR`'s `bin`, then
  the directory of my own executable) before PATH.
- **2.2** As a developer who asked for `--lto=thin` explicitly, when no lld
  can be found anywhere, then the build fails by name rather than linking
  without LTO; the default may fall back, but it says so once per build at
  warning level with the path it looked in.
- **2.3** As the test suite, then a build-tool test with an empty PATH and a
  configured LLVM_DIR still links with LTO.

## 3. Acceptance
`bench.sh` no longer needs to prepend the fork's bin; the compile-lto thin
row's "needs lld" check never fires.
