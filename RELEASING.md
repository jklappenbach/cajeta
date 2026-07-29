# Releasing cajeta

Cajeta versions are bumped manually. Releases are produced by
`.github/workflows/release.yml`, which cross-builds the compiler binary
for every supported target and uploads the artifacts.

The workflow has two triggers and two operating modes:

| Trigger | Mode | Tag created? | Release published? | Use for |
|---|---|---|---|---|
| Push `v*` tag | (always production) | yes (the pushed tag) | yes, "latest" | Normal release cuts driven from a local `git tag` |
| Actions UI → Run workflow → `dry-run` | dry-run | **no** | **no** | Verify the matrix without burning a version |
| Actions UI → Run workflow → `production` | production | `v<version>` | yes, "latest" | Cut a real release from the UI instead of `git tag` |

**Pre-releases** are handled via semver: push a tag with a `-` suffix
(`v0.2.0-rc1`, `v0.2.0-beta1`) and `softprops/action-gh-release` auto-
detects it as a pre-release. No special workflow mode needed.

## Version scheme

Semver (`MAJOR.MINOR.PATCH`). While the compiler is pre-1.0:

- `0.MINOR.PATCH` — `MINOR` bumps for any user-visible behavior change
  (language additions, archive-format additions, CLI surface changes).
- `PATCH` bumps for bug-fix-only releases.

Post-1.0:

- `MAJOR` bumps for breaking changes to the language, archive format, or
  CLI surface.
- `MINOR` bumps for additions.
- `PATCH` bumps for bug fixes.

The canonical source of the version string is the `VERSION` file at the
repo root. `CMakeLists.txt` reads it at configure time and bakes it into
the compiler as the `CAJETA_VERSION` define; `cajeta --version` prints
it back.

## Recommended flow

The "don't burn version numbers on broken builds" flow:

```sh
# 1. Bump VERSION. Be specific about which level moved.
$EDITOR VERSION
git add VERSION
git commit -m "release: prepare v$(cat VERSION)"
git push origin main
```

```text
# 2. Trigger a dry-run first to verify the matrix.
# Actions → release → Run workflow → mode = dry-run → Run workflow
# (Watch the run, fix anything that breaks. No tag is created;
# nothing public is touched.)
```

```sh
# 3. Once dry-run is green on every target, cut the real tag.
git tag "v$(cat VERSION)"
git push origin "v$(cat VERSION)"
```

Once the tag lands, watch the workflow at
`https://github.com/jklappenbach/cajeta/actions`. The build jobs run in
parallel (one per target, plus the IDE plugin), then a final `release`
job collects the artifacts and publishes them at
`https://github.com/jklappenbach/cajeta/releases/tag/v<version>`.

That last job only runs if **every** build leg succeeded — publishing is
all-or-nothing, so a failed target means no Release at all rather than a
partial one. A failed cut therefore costs no version number: fix forward
and re-cut the same tag. See "When a tagged build fails for one target".

If you'd rather cut releases entirely from the UI (no local `git tag`):
bump VERSION + push to main, then Actions → release → Run workflow →
mode = production → Run. The workflow creates the tag itself at the
end.

For early-access / pre-release binaries that shouldn't show as "latest"
on the repo page: tag with a semver pre-release suffix like
`v0.2.0-rc1`, `v0.2.0-beta1`, or `v0.2.0-alpha2`. Pushing such a tag
publishes a Release that GitHub auto-flags as a pre-release (the
`softprops/action-gh-release` action detects the `-` suffix). The tag
itself is permanent, like any production tag.

## When a dry-run fails

That's the happy path of dry-runs — they exist to catch failures
before you commit a version number. Inspect the failing job's log,
push a fixup commit to main, re-trigger the dry-run from the UI. The
VERSION number doesn't move until you cut a real tag.

## When a tagged build fails for one target

**Publishing is all-or-nothing.** The build matrix is `fail-fast: false`,
so one target failing doesn't abort the others — but the `release` job is
gated on `needs.build.result == 'success'`, which is true only when
*every* leg passed. One failed target means **no Release is created at
all**, not a partial one. That gate is deliberate: a tag with a missing
binary is worse than no tag, because `cvm` would resolve a manifest with
partial artifacts.

So a failed tag build **burns no version number**. The tag ref exists on
origin, but nothing public was produced and nothing downstream can
consume it — a consumer pinned to that version fails with `release not
found` exactly as if it had never been cut.

### The rule

> **Bump VERSION only when a Release was actually published.**
> If the cut failed, fix forward and re-cut the *same* version.

Don't burn a patch number on a build that produced nothing. Version
numbers describe what shipped; an unpublished tag shipped nothing.

Check which case you're in — this is the whole decision:

```sh
gh release view "v$(cat VERSION)" --json name,assets
#   "release not found"  -> nothing published; re-cut the same version (below)
#   a Release with assets -> it shipped; fix forward and bump VERSION
```

### Re-cutting the same version (nothing was published)

Safe because there is no Release, no assets, and nothing to have been
downloaded. Push the fixes to main first, then move the tag:

```sh
git push origin main                        # fixes must be on main first
git tag -d "v$(cat VERSION)"
git push origin ":refs/tags/v$(cat VERSION)"
git tag "v$(cat VERSION)"                   # from the fixed HEAD
git push origin "v$(cat VERSION)"
```

Before re-pushing, prefer a **dry-run scoped to the targets that failed**
— it skips publishing entirely and costs one leg instead of the full
matrix:

```text
Actions → release → Run workflow
  mode    = dry-run
  targets = aarch64-apple-darwin,x86_64-w64-mingw32
```

Iterate there until green, and only then move the tag. Expect to go a
few rounds: each fix can uncover the next failure on that platform, since
the build stops at the first error.

### When a Release *did* publish

Then the version is spent — it exists publicly and may already be
resolved by `cvm` or a downstream project's `CAJETA_VERSION` pin. Push
the fix, bump VERSION to the next patch, and cut a new tag. The original
Release stays as the record.

## Why non-Linux breakage tends to arrive all at once

Only the tag-triggered workflows build macOS and Windows; there is no CI
on pushes to `main`. Everything merged between two releases is therefore
Linux-only in practice, and a release cut is the first time those targets
see the accumulated diff. Symptoms cluster in a few families:

- **libstdc++-only headers/extensions** on macOS (libc++), e.g.
  `<ext/stdio_filebuf.h>` / `__gnu_cxx::*`. A bare `#ifndef _WIN32` guard
  is not enough — that is "not Windows", not "is libstdc++".
- **Transitively-included headers** that libstdc++ happens to pull in and
  MinGW does not (`<algorithm>` for `std::replace`, and similar).
- **POSIX-only libc calls** in tests — use `test/PortableEnv.h`, which
  maps `setenv`/`unsetenv`/`getpid` onto the Windows CRT equivalents.
- **Mach-O symbol prefixing.** Linker-level names carry an extra leading
  underscore that ELF does not, so `-Wl,-u,<sym>` must be spelled
  differently on Apple (see `debug-tests/CMakeLists.txt`).

Because compilation stops at the first error, fixing one usually reveals
the next. When you fix one of these, grep for the same pattern across the
whole tree rather than only the file CI named — it is much cheaper than
another 40-minute round trip.

## Notifications

GitHub's default notification settings email the workflow's triggering
user when any run fails — Settings → Notifications → Actions, ensure
"Send notifications for: Failed workflows only" is selected.

For a richer signal (Slack, auto-issue-on-failure, etc.), the workflow
has a single trailing job that's the natural place to add a
`failure-notify` step. Not wired in by default.

## Supported binary targets

| Target | Triple | Runner | Status |
|---|---|---|---|
| Linux x86_64 | `x86_64-linux-gnu` | `ubuntu-latest` | tier 1 (primary dev platform) |
| Linux ARM64 (incl. NVIDIA Grace, Ampere Altra, AWS Graviton, Apple Silicon under Linux) | `aarch64-linux-gnu` | `ubuntu-24.04-arm` | tier 1 |
| macOS Apple Silicon | `aarch64-apple-darwin` | `macos-14` | tier 1 |
| macOS Intel | `x86_64-apple-darwin` | (not in matrix) | build from source — see below |
| Windows x86_64 | `x86_64-pc-windows-msvc` | `windows-latest` | tier 1 |
| Linux RISC-V 64 | `riscv64-linux-gnu` | `ubuntu-latest` + QEMU cross | best-effort (no native runner; cross-compiled, not test-run on host) |

NVIDIA's recent ARM-based platforms (Grace CPU servers, Jetson Orin
edge SoCs, DGX Spark) all share the `aarch64-linux-gnu` triple — they
consume the Linux ARM64 build. CUDA-codegen support
(`nvptx64-nvidia-cuda`) is a separate roadmap item; "NVIDIA ARM" here
refers to running cajeta-compiled programs on NVIDIA's CPU silicon,
not targeting their GPUs as a codegen sink.

### Intel macOS — build from source

`x86_64-apple-darwin` is not in the release matrix because GitHub-hosted
`macos-13` (Intel) runners are scarce and slow to dispatch (Apple
stopped selling Intel Macs in 2022; the GH runner pool is being wound
down). Intel Mac users build from source:

```sh
brew install cmake ninja llvm antlr4-cpp-runtime openjdk@21 \
    googletest glog zstd xxhash
export LLVM_DIR="$(brew --prefix llvm)/lib/cmake/llvm"
git clone https://github.com/jklappenbach/cajeta.git
cd cajeta
./setup.sh && ./build.sh
./build/src/cajeta --version
```

Same recipe as the workflow's `aarch64-apple-darwin` job. When
runner availability improves (or you ask for an x86_64 binary
specifically), restoring the matrix row is a 4-line YAML edit.

## Artifact shape

Each release publishes a generic archive per target plus a native installer
per (format × arch), and the IDE plugin zip:

```
cajeta-v<version>-<triple>.tar.gz     generic archive (Unix)
cajeta-v<version>-<triple>.zip        generic archive (Windows)
cajeta_<version>_<arch>.deb           Debian/Ubuntu      (amd64 / arm64)
cajeta-<version>-1.<arch>.rpm         Fedora/RHEL        (x86_64 / aarch64)
cajeta-<version>-<arch>.msi           Windows installer
cajeta-<version>-<arch>.pkg           macOS installer
cajeta-idea-<version>.zip             IntelliJ IDEA plugin (arch-independent)
*.sha256                              checksum beside each installer
```

Generic-archive contents:

- `bin/cajeta` (or `bin\cajeta.exe`) — the compiler binary.
- `VERSION` — exact version string this build was cut from.
- `README.md`, `LICENSE` — top-of-tree as of the tag.

The native installers (`cpack`-driven, from the CMake `install()` rules) lay
the binary out per OS — Linux `/usr/lib/cajeta/cajeta` + `/usr/bin/cajeta`
symlink; Windows `C:\Program Files\Cajeta\bin` (+ system `PATH`); macOS via
`productbuild`. The Windows installers bundle the MinGW runtime DLLs so the
binary is self-contained; the Linux packages declare only base-system deps
(once the self-contained static-link lands — see `plan/installer-plan.md` D2).

## Distribution channels

Tiered; each tier consumes the one below (full table in
`plan/installer-plan.md` §2):

- **Tier 0 — GitHub Releases:** the artifacts above. Always produced. The
  `cajeta` installers here are the direct single-version path; we do **not**
  host `cajeta` apt/dnf repos.
- **Tier 1 — `cvm` (Cajeta Version Manager) via package managers + shim:** the
  ecosystems (apt / dnf / AUR / Homebrew / winget) ship **cvm**, not `cajeta` —
  cvm is version-independent, so one evergreen package per ecosystem covers "get
  latest" despite distro lag. `cvm self update` defers to the package manager
  when cvm is PM-installed. The `curl … | sh` shim covers brewless/repoless.
  Written in Cajeta (`plan/installer-plan.md` §2 / D12).
- **Tier 2 — IDE plugin:** Marketplace, `cajeta ide install` (embedded), or
  install-from-disk with `cajeta-idea-<ver>.zip`.

## IDE plugin & JetBrains Marketplace

The `plugin` job (JDK 21 + Gradle) builds `cajeta-idea-<version>.zip` once; its
`pluginVersion` is synced to `VERSION` at build time (`-PpluginVersion=...`).
Every compiler build job downloads that zip and embeds it into the binary, so
`cajeta ide install` works from any distribution form.

On **production tags** (never dry-run, and pre-release `-rc`/`-beta` tags are
skipped), the workflow runs `signPlugin` + `publishPlugin` to the Marketplace.
This requires repository secrets:

- `PUBLISH_TOKEN` — Marketplace API token (vendor account).
- `CERTIFICATE_CHAIN`, `PRIVATE_KEY`, `PRIVATE_KEY_PASSWORD` — plugin signing.

**One-time bootstrap:** the very first upload of a plugin must be done by hand
through the Marketplace UI (JetBrains review); automated `publishPlugin` only
works for subsequent updates. Until the secrets are set, the publish step
no-ops with a warning (the release still ships the plugin zip + embedded copy).

## Installer tooling provisioned in CI

- **Linux:** `dpkg-deb` (preinstalled) + `rpmbuild` (`rpm` package) for
  `cpack -G DEB`/`-G RPM`.
- **Windows:** WiX Toolset **v5** as a dotnet global tool
  (`dotnet tool install --global wix --version 5.0.2` + the `WixToolset.UI`
  extension). Pinned to v5 deliberately — **WiX v6/v7 require accepting the
  Open Source Maintenance Fee (OSMF) EULA**, which we avoid.
- **macOS:** `productbuild` (preinstalled). Signing + notarization
  (Developer ID Installer cert → `notarytool` → `stapler`) is still open —
  see `plan/installer-plan.md` §6.

## Install-test matrix (validation)

Before a format is promoted from "produced" to a release gate, install-test it
in a clean environment (the workflow's packaging steps are `continue-on-error`
until then):

- `.deb` → `docker run ubuntu:24.04` → `apt install ./*.deb` → `cajeta --version`.
- `.rpm` → `docker run fedora:latest` → `dnf install ./*.rpm` → `cajeta --version`.
- `.msi` → clean Windows → `msiexec /i ... /qn` → fresh shell → `cajeta --version`.
- `.pkg` → clean macOS → `installer -pkg ... -target /` → `cajeta --version`.
- plugin → `cajeta ide install` into a clean IDEA; confirm it loads.

## When the workflow fails

The workflow runs at most one job per target — partial failures
publish the artifacts that built cleanly. To diagnose a failed job:

1. Go to `https://github.com/jklappenbach/cajeta/actions`, click the
   workflow run.
2. Each matrix job has the failing step expanded in the log. The most
   common failure modes:
   - **LLVM not found.** A runner image bumped its preinstalled LLVM
     version; the workflow's `apt-get install llvm-22-dev` (or `brew
     install llvm`) didn't materialize. Bump the workflow's LLVM
     version pin in lockstep with `setup.sh`.
   - **antlr4 generator timeout.** ANTLR4 codegen runs `java` against
     the parser grammar; if the runner image is missing JRE 21, the
     build fails at parser-generation time. The workflow installs
     `openjdk-21-jre` on all Linux jobs; macOS runners have JRE
     pre-bundled.
   - **lld not available.** Some runner images don't ship `lld-<ver>`
     as a separate package. The workflow sets `CAJETA_NO_LLD=1` to
     fall back to the system linker; `--emit=exe` then prints a hint
     rather than linking in-process. Doesn't affect the release
     binary itself.
3. Push a fixup commit and re-tag — the existing tag has to be
   deleted first (`git tag -d v<x.y.z> && git push origin
   :refs/tags/v<x.y.z>`) before the new push of the same tag
   re-triggers the workflow.

## Local sanity check before tagging

```sh
# Build everything from scratch and run the full regression.
rm -rf build && ./setup.sh && ./build.sh
./build/test/cajeta_test

# Confirm --version reports what you bumped VERSION to.
./build/src/cajeta --version
# → cajeta 0.1.0 (a3f2c1)
```

If those two checks pass on Linux, the workflow's Linux jobs almost
always pass; the cross-platform jobs are where most fixup happens.

## What the release workflow tests

The workflow does **not** run the full ~1725-test battery on every target —
that's too expensive across the four-platform matrix, and most of the suite
is the host-independent front-end (parser / typer / borrow-checker), which
can't regress because of cross-compilation. Each build job instead runs:

1. **Build** — proves the compiler + runtime compile and link on the target.
2. **Smoke** — `cajeta --version` (and `--version --verbose`): the binary
   loads and runs.
3. **Release tests** — `release_tests.sh`, the curated subset of
   cross-compilation-sensitive suites in `test/release_filter.txt`: numeric
   intrinsics, byte serialization + the `.cja` archive format, struct/view
   layout & alignment, atomics/locks, TLS + the drop chain, async/fibers,
   threading, OS/file I/O, plus a thin codegen/dispatch smoke. These are the
   areas the LLVM back-end + C runtime lower differently per ISA / OS /
   endianness / ABI — where a binary can break even though the front-end is
   green everywhere.

The subset is a **hard gate on `x86_64-linux-gnu`** and informational
(`continue-on-error`) on the other targets, matching the prior posture
(JIT-driven tests can hit known platform lowering quirks that don't affect
AOT releases).

Run the same subset locally:

```sh
./release_tests.sh                 # build + run the subset (sharded)
# or via the build system:
cmake --build build --target release_tests
```

`release_tests.sh` fails loudly if any pattern in `release_filter.txt`
matches zero tests (a suite was renamed/removed) — so the filter can't
silently rot into a no-op. When you rename a test suite that's listed there,
update `release_filter.txt`. The full battery still runs locally via
`./cajeta_tests.sh` (no args).
