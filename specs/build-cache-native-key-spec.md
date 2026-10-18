# Build cache key misses baked native archives (spec)

Status: draft, 2026-10-18. A defect found by cajeta-crypto plan 5.1.1. This
is a stub to be scoped with Julian.

## 1. Definition

A library build bakes the static archive of each live `@Native` library into
its `.cja` (`Compiler.cpp`, the bake step after the skills). It finds those
archives through `nativeLinkSearchDirs()`: `CAJETA_NATIVE_PATH`, then
`~/.cajeta/native`.

The build action's artifact cache (`BuildAction.cpp`, the
`.cajeta/cache/artifact/<digest>` slot) keys on the flags discriminator, the
entry method, every source digest and every skill digest. It does not key on
the native archives the build bakes, or on `CAJETA_NATIVE_PATH`.

Measured on cajeta-crypto with cajeta main 498c7feb, 2026-10-18 00:00:
1. `cajeta install` without `CAJETA_NATIVE_PATH` built a `.cja` with no
   shim ("native lib 'cajeta_crypto' not bundled").
2. `CAJETA_NATIVE_PATH=$PWD/native cajeta install` then answered
   `[cache] hit` and republished the shim-less archive. Deleting `build/`
   did not help; the slot is under `.cajeta/cache`.
3. Only after `rm -rf .cajeta/cache` did the archive carry
   `libcajeta_crypto.a`, and the outside consumer linked.

The same key ignores a rebuilt shim, so after editing `native/*.c` and
rerunning `native/build.sh`, a library build can still ship the old C.
Release CI starts clean and is not affected.

## 2. Use cases

- **2.1** When a library build would bake a different set of native archive
  bytes than the cached artifact holds, it rebuilds.
- **2.2** When nothing it would bake changed, the cache still hits.

## 3. Open questions

- **3.1** The key is computed before compiling, when the live `@Native`
  libraries are not yet known. Key on the manifest's `native-libraries`
  names and digest what `findNativeArchivesByPlatform` finds for each, or
  key on every archive under the search dirs?
- **3.2** The `--emit=exe` error names "the project native/ dir" as a search
  location, but `nativeLinkSearchDirs()` does not search it. Fix the message
  or the search.
