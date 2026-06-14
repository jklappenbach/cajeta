# Windows GA handoff — where we left off

_Written 2026-06-14. Branch: `main` @ `acad758` (pushed). GA v0.7.0 is PAUSED on
the Windows leg being abnormally slow (~1.4h+ build+test). Everything else is
done and green._

## TL;DR

cajeta v0.7.0 GA is one green Windows leg away from tagging. The other three
targets (aarch64-linux, x86_64-linux, aarch64-apple-darwin) are validated green
on the GA commit. The Windows CI leg (GitHub `windows-latest`, 4 cores) takes
~1.4h+ which the user considers abnormal, so we're moving Windows build+test to
the user's **fast native Windows machine** (see memory `fast-windows-machine`)
to diagnose and iterate, instead of burning ~2.5h CI cycles.

**Resume action:** on the fast Windows box, `git pull` main, build, run
`./release_tests.sh`, and find out whether the ~1.4h is the C++ **build** or the
**test sweep** — then fix it. Once Windows is green, tag GA (below).

## Goal

Ship cajeta **v0.7.0 GA**: tag `v0.7.0` from `main`, which triggers the
production `release.yml` run (builds + tests all 4 targets, then publishes the
GitHub Release). GA tags from `main` — NOT `cajeta-xpu` (memory
`main-is-central-branch`).

## Current git state (all committed + pushed)

`main` @ `acad758`, in order:
- `81a44b8` merge `feature/cvm` → main (the whole v0.7.0 candidate: cvm tooling,
  the cross-platform JIT fixes, fiber/Task UAF fix, async test fix)
- `fb79ea8` build: pin runtime-bitcode clang/llvm-link to the compiler's LLVM
  (LLVM_TOOLS_BINARY_DIR + version-assert; stops ROCm clang 22 silently
  compiling the runtime bitcode for the LLVM-23 JIT — memory
  `runtime-bitcode-toolchain-pin`)
- `e4e5b9a` test: batch mode used bash-4 `declare -A`; macOS ships bash 3.2 —
  reworked to indexed arrays
- `8038ed0` **release: VERSION 0.7.0-rc3 → 0.7.0 + pin release gate to BATCH=0**
  (this is the commit to TAG for GA — it's the one the 3 fast legs were gated
  green on)
- `acad758` test: per-test breadcrumb in shard logs (`>> Suite.test ... RESULT`,
  name first/no-newline so a dangling line names a mid-test kill; `grep '^>> '`)

**Tag GA on `8038ed0`, not `acad758`** — `8038ed0` is the gated commit; the
breadcrumb commit on top only changes log formatting (harmless, but ungated).

## How we got here this session

1. Fixed the lone Windows release-test failure: `AsyncSyntaxTests.
   implicitFunctionBodyScopeJoinsSpawn` UAF'd the async lock (destroyed before
   the implicit join; benign on glibc, SIGSEGV on mingw winpthreads). Worker now
   owns the lock's whole lifetime. Valgrind-clean. (`c5abe35`, in the merge.)
2. Got a full 4-target dry-run **4/4 green** on `feature/cvm` `c5abe35`.
3. User: GA must come from `main`. Merged `feature/cvm` → `main` (clean) — but
   `main` also carried 8 newer commits (docs, `xpu(11)` device dispatch, grammar
   `double`-keyword removal), so the merged HEAD needed re-validation.
4. To soften the re-test cost, built a **per-suite batch test mode** (BATCH=1,
   stdlib primes once per suite instead of per test; per-test crash fallback).
   Parity 474/0 on Linux. BUT: macOS bash 3.2 broke on `declare -A` (fixed), and
   on **Windows the batch made things WORSE/slow** — running a whole suite in one
   process likely destabilizes the Windows JIT and trips the per-test fallback
   (suite pays a failed batch attempt + a full per-test re-run). So we pinned the
   release GATE back to `BATCH=0` (`8038ed0`). BATCH=1 stays the local-dev
   default.
5. Toolchain pin (`fb79ea8`) — found while answering "which LLVM": runtime
   bitcode clang was unpinned; ROCm clang 22 on PATH would silently miscompile.
6. Dry-run on `8038ed0` (BATCH=0): aarch64-linux, x86_64-linux, macOS all GREEN.
   **Windows leg cancelled** — it was the ~1.4h+ holdout the user wants to fix on
   real hardware. (Cancelled run id 27484248581.)

## The open Windows question

Is the ~1.4h dominated by the **cajeta C++ build** (mingw g++ over ~640 objects)
or the **release-test sweep**? Prior data:
- Pre-batch Windows sweep ("Run release tests") = 6943s ≈ **116 min**, 474 tests,
  4 workers, ~58s/test (JIT prime ~40-60s dominates per-test).
- Windows build step adds on top (unmeasured precisely, ~20-40 min?).

On the fast box, measure each phase separately. Levers:
- **Build:** is it parallelized (`ninja -j$(nproc)`)? mingw g++ is slow; check
  core utilization. The fork's clang for the runtime-bitcode step is fine.
- **Sweep:** `BATCH=0` (per-test, gate default, reliable) vs `BATCH=1` (per-suite,
  faster if stable). Use the new breadcrumb to see per-test timing and whether
  batch trips fallbacks. The dominant cost is the ~40-60s stdlib JIT prime per
  process — on a fast machine that prime should be much less, so a fast box may
  make BATCH=0 perfectly acceptable.
- Worker count: CI used 4 (runner cores). A fast box with more cores → more
  shards → near-linear speedup on the per-test path.

## Resume checklist (fast Windows box)

1. `git pull` (main @ `acad758` or later).
2. Ensure the cajeta-llvm FORK (LLVM 23) is available + `LLVM_DIR` points at its
   Windows `lib/cmake/llvm`. The runtime-bitcode clang must be the fork's
   `clang-23` (the new version-assert will FATAL the configure otherwise — that's
   intended). Fork is published per-target as release `cajeta-llvm-23-r4` on
   github.com/jklappenbach/cajeta-llvm, or build it.
3. Build via msys2/mingw (`./setup.sh` then `./build.sh`, or the project's
   documented Windows build). Time it.
4. `NO_BUILD=1 KEEP_LOGS=run-logs ./release_tests.sh` — time it. Compare BATCH=0
   vs `BATCH=1 ...`. Watch `run-logs/shard_*.out` / `grep '^>> '`.
5. Identify + fix the slowness (build flags, worker count, or batch viability).
6. When Windows is green and fast: confirm all 4 targets, then **tag GA**:
   ```
   git tag -a v0.7.0 -m "cajeta v0.7.0 — GA" 8038ed0
   git push origin v0.7.0      # triggers production release.yml -> publish
   ```
   (Tag-push is an outward-facing publish — the user authorizes/pushes it.)

## Pointers / memory

- `main-is-central-branch` — GA tags from `main`.
- `fast-windows-machine` — use the user's fast Windows box for this.
- `runtime-bitcode-toolchain-pin` — the LLVM-match assert; expected to pass with
  the fork's clang-23 on PATH.
- `test-harness-and-flags` — BATCH mode, bash-3.2 constraints, every env flag.
- `build-requires-cajeta-llvm-fork`, `llvm-fork-toolchain-skew` — LLVM fork setup
  + the skew gotcha (fork clang-23 older than its LLVM libs → JIT linkRuntime
  crashes/hangs; check before blaming source).
