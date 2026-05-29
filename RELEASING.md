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
`https://github.com/jklappenbach/cajeta/actions`. Five build jobs run
in parallel (one per target), then a final `release` job collects all
the artifacts and publishes them at
`https://github.com/jklappenbach/cajeta/releases/tag/v<version>`.

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

## When a published release fails to build for one target

The build matrix is `fail-fast: false`, so one target failing doesn't
abort the others. The `release` job still runs and attaches whichever
artifacts did build. The Release page then has a partial set of
binaries.

To fix: push a commit that addresses the failure, bump VERSION to the
next patch (`v0.1.0` → `v0.1.1`), and cut a new tag. The original
Release stays as a record of the partial-success cut.

For a tag that's been pushed but the workflow hasn't picked up yet
(seconds-scale race), or hasn't been pulled by any downstream consumer,
you can force-re-tag — but only do this if you're certain no one has
downloaded the partial release yet:

```sh
git tag -d v<x.y.z>
git push origin :refs/tags/v<x.y.z>
gh release delete v<x.y.z> --yes   # if a partial Release was created
# (re-push the tag from the fixed HEAD)
git tag v<x.y.z>
git push origin v<x.y.z>
```

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
brew install cmake ninja llvm antlr4-cpp-runtime openjdk@17 \
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

Each release publishes:

```
cajeta-v<version>-<triple>.tar.gz   (Unix)
cajeta-v<version>-<triple>.zip      (Windows)
```

Contents:

- `bin/cajeta` (or `bin\cajeta.exe`) — the compiler binary.
- `lib/cajeta-stdlib.cja` — the parsed stdlib as a project-only cja
  archive, suitable for downstream `--classpath` ingestion. (Today
  the compiler also embeds the stdlib internally, so the file is
  primarily for inspection / forward-compat with the eventual
  classpath-stdlib model.)
- `VERSION` — exact version string this build was cut from.
- `README.md`, `LICENSE` — top-of-tree as of the tag.

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
     the parser grammar; if the runner image is missing JRE 17, the
     build fails at parser-generation time. The workflow installs
     `openjdk-17-jre` on all Linux jobs; macOS runners have JRE
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
`./run_tests.sh` (no args).
