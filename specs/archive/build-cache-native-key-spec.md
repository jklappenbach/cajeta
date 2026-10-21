# Build cache key misses baked native archives (spec)

Status: active; scoped 2026-10-21, §3.1 and §3.2 approved by Julian the same day. Drafted 2026-10-18 as
a defect found by cajeta-crypto plan 5.1.1.

## 1. Definition

A library build bakes the static archive of each live `@Native` library into
its `.cja` (`Compiler.cpp`, the bake step after the skills). It finds those
archives through `nativeLinkSearchDirs()`: `CAJETA_NATIVE_PATH`, then
`~/.cajeta/native`. For each live library it bakes every
`<dir>/<platform>/lib<name>.a` (or `<name>.a`) it finds, one per platform, and
for the host platform a shared library (`findNativeJitArtifact`: `.so`,
`.dylib`, `.dll` beside or under the dir) when no static archive was found.

The build action's artifact cache (`buildtool/actions/BuildAction.cpp`, the
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
  bytes than the cached artifact holds, it rebuilds. "Different" covers a
  shim provided after the cached build, a shim rebuilt from edited C, a
  shim removed, a platform added or removed, and a search path that now
  reaches a different copy.
- **2.2** When nothing it would bake changed, the cache still hits: a build
  with no native libraries anywhere keys exactly as today, and a build whose
  native directories are untouched between two runs hits on the second.
- **2.3** A `--emit=exe` build that cannot find a native library names only
  the places it searched.
- **2.4** A library whose checkout holds `native/<platform>/lib<name>.a`
  bakes it with a plain `cajeta build` or `cajeta install`, no environment
  variable set; a consumer's `--emit=exe` build through the build tool links
  its own project's `native/` the same way.

## 3. Decisions (approved 2026-10-21)

- **3.1 What the key folds.** The key is computed before compiling, when the
  live `@Native` set is not yet known, so it folds everything the bake could
  find rather than what the bake will choose: the ordered search-dir list as
  text, and for each search dir the path and content digest of every regular
  file directly under `<dir>/<platform>/` for every subdirectory but
  `include`, plus every `.so`, `.dylib`, `.dll` and `.a` directly under
  `<dir>` (the shared-library fallback's locations). A missing search dir
  contributes its path only. Over-invalidation is harmless: a change to any
  native file on the machine re-keys every library build, and native
  directories hold a handful of small archives, so digesting them costs less
  than a source digest walk. Digests are by content, as sources and skills
  are, so a rebuilt-then-reverted shim lands back on the first slot.
  Rejected: keying on the manifest's `native-libraries` block, because a
  library's live natives come from its `@Native` declarations, not from that
  block (cajeta-crypto declares none), and a first build cannot read the
  live set before it compiles.
- **3.2 The project's `native/` directory.** Decided with Julian 2026-10-21,
  revising the first proposal (fix the message only). Every library checkout
  already keeps its shim sources and `native/build.sh` under `native/`, whose
  output is `native/<platform>/lib<name>.a`, and each one's `run-tests.sh` and
  release workflow set `CAJETA_NATIVE_PATH` to that directory by hand. The
  build tool now passes `<project root>/native` to the compiler as an explicit
  search directory (`--native-dir=`), searched after the classpath's staged
  archives and before `CAJETA_NATIVE_PATH` and `~/.cajeta/native`, and the
  cache key folds it like any search dir. The project root comes from the
  manifest, never from the working directory, so a build's inputs do not
  depend on where the command ran; the bare compiler CLI without a manifest
  keeps today's two sources. The `--emit=exe` not-found message names the
  three places that are searched.
