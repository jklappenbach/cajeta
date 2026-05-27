# Releasing cajeta

Cajeta versions are bumped manually and pushed as git tags; pushing a `v*`
tag triggers `.github/workflows/release.yml`, which cross-builds the
compiler binary for every supported target and uploads the artifacts to
the matching GitHub Release.

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

## Cutting a release

From a clean main branch with the regression suite green:

```sh
# 1. Bump VERSION. Be specific about which level moved.
$EDITOR VERSION

# 2. Commit. Tag message can be empty; the GitHub Release notes
#    are generated from the tag message + the workflow's commit-list
#    summary (auto-populated by softprops/action-gh-release).
git add VERSION
git commit -m "release: v$(cat VERSION)"
git push origin main

# 3. Tag and push the tag. THIS is what triggers the workflow.
git tag "v$(cat VERSION)"
git push origin "v$(cat VERSION)"
```

Once the tag lands on origin, watch the workflow at
`https://github.com/jklappenbach/cajeta/actions`. Six jobs run in
parallel (one per target), then a final `release` job collects all
the artifacts and creates / updates the GitHub Release at
`https://github.com/jklappenbach/cajeta/releases/tag/v<version>`.

## Supported binary targets

| Target | Triple | Runner | Status |
|---|---|---|---|
| Linux x86_64 | `x86_64-linux-gnu` | `ubuntu-latest` | tier 1 (primary dev platform) |
| Linux ARM64 (incl. NVIDIA Grace, Ampere Altra, AWS Graviton, Apple Silicon under Linux) | `aarch64-linux-gnu` | `ubuntu-24.04-arm` | tier 1 |
| macOS Intel | `x86_64-apple-darwin` | `macos-13` | tier 1 |
| macOS Apple Silicon | `aarch64-apple-darwin` | `macos-14` | tier 1 |
| Windows x86_64 | `x86_64-pc-windows-msvc` | `windows-latest` | tier 1 |
| Linux RISC-V 64 | `riscv64-linux-gnu` | `ubuntu-latest` + QEMU cross | best-effort (no native runner; cross-compiled, not test-run on host) |

NVIDIA's recent ARM-based platforms (Grace CPU servers, Jetson Orin
edge SoCs, DGX Spark) all share the `aarch64-linux-gnu` triple — they
consume the Linux ARM64 build. CUDA-codegen support
(`nvptx64-nvidia-cuda`) is a separate roadmap item; "NVIDIA ARM" here
refers to running cajeta-compiled programs on NVIDIA's CPU silicon,
not targeting their GPUs as a codegen sink.

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
     version; the workflow's `apt-get install llvm-20-dev` (or `brew
     install llvm@20`) didn't materialize. Bump the workflow's LLVM
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
