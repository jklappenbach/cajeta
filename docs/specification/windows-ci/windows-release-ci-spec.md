# Windows Release CI — Spec

## Goal
The `release.yml` workflow's Windows leg (`build (x86_64-w64-mingw32)`) must
build, smoke-test, and run the release tests to green on the self-hosted
PHOENIX runner, **without regressing** the already-green Linux (x86_64,
aarch64), macOS (arm64), and IDE-plugin legs.

## Why
Only the Windows leg is failing. The Linux/macOS legs pass. The Windows host
differs in three ways that the shared build paths did not account for:

1. **MSYS2 `$HOME` is `/`** (a read-only mount) on the self-hosted runner, so
   the LLVM toolchain could not be unpacked under `$HOME`.
2. **Two clangs/llvm-links on the box** — MSYS2 ships LLVM 22, the cajeta-llvm
   fork is LLVM 23. CMake's `find_program` picked the wrong (LLVM 22) ones for
   the runtime-bitcode step, which must match the LLVM the compiler JITs against.
3. **Workspace reuse** — unlike GitHub-hosted runners, the self-hosted runner
   keeps `build/CMakeCache.txt` between runs, so a stale cached tool path
   survives a re-configure.

## Constraints (from the user, verbatim intent)
- **Windows-only changes.** Do not make global changes that may break the other
  builds. Gate any genuine code-behavior change behind a Windows `#ifdef` /
  CMake `if(WIN32)`.
- If a real code bug is found that only Windows tripped over, gate it for
  Windows **and** record a mandate here to verify the same code path on
  Linux/macOS later.
- Build against latest `origin/main`; work in the isolated worktree
  `D:\code\cpp\cajeta-ci-wt` so the user's uncommitted WIP is never disturbed.

## Capabilities required
- Fetch the prebuilt cajeta-llvm fork toolchain (LLVM 23) and use **its** clang
  and llvm-link for the runtime-bitcode step on Windows.
- Configure + build + test the Windows leg with mingw `g++` for cajeta's C++
  (the fork's clang++ doesn't auto-link winpthreads on mingw).
- Keep build-warning output free of system-header noise so a genuine warning is
  visible.
