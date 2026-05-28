# Release readiness progress (cajeta v0.1.0)

_Last updated: 2026-05-28. Branch `main`, HEAD `b2e7740`._

## Goal

Get the cross-platform release workflow (`.github/workflows/release.yml`) green so
`v0.1.0` can be tagged. Targets: `x86_64-linux-gnu` (primary / hard gate),
`aarch64-linux-gnu`, `aarch64-apple-darwin`, `x86_64-w64-mingw32` (MSYS2 / MinGW).

## TL;DR current status

- **The release is GREEN and shippable.** Every dry-run comes back `success` at the
  run, job, and step level. Build + smoke test (`cajeta --version`) + packaging +
  artifact upload all succeed on all four targets. The AOT binary works.
- **Do NOT tag yet** — per the working agreement, hold the `v0.1.0` tag until we
  decide the Windows JIT regression below is either fixed or formally accepted as a
  tracked known-issue. Tagging is the user's call.
- **One real issue remains: the Windows JIT *test* suite fails.** It does **not**
  block the release (see "continue-on-error masking" below), but it is a genuine
  Windows port gap worth closing now that we have a Windows box to iterate on.

## CRITICAL gotcha: continue-on-error masks Windows/macOS/ARM regression

`release.yml` runs the regression suite on every target but sets
`continue-on-error: ${{ matrix.target != 'x86_64-linux-gnu' }}`. Only
`x86_64-linux-gnu` is a hard gate; the other three are non-fatal.

GitHub Actions semantics: when a `continue-on-error` step **fails**, its
`outcome` is `failure` but its reported `conclusion` is `success`. `gh run view
--json jobs` exposes only `conclusion`. So a failing Windows regression step shows
up as `conclusion=success` everywhere in the API — the run looks fully green even
though the Windows JIT tests are failing (exit code 3).

**To see the TRUE Windows regression result you must read the job LOG, not the
conclusion:**

```bash
# overall + per-job conclusions (will all say success — misleading for non-linux):
gh run view <RUN_ID> --json status,conclusion,jobs \
  -q '.jobs[] | "\(.name): \(.conclusion)"'

# REAL Windows regression outcome — pull the job log via the API (works even while
# the overall run is still in progress, since the job itself has finished):
gh api repos/jklappenbach/cajeta/actions/jobs/<WINDOWS_JOB_ID>/logs > /tmp/win.log
grep -c '\[  FAILED  \]' /tmp/win.log
grep -c 'CAJETA_ERROR_PACKAGE_MISMATCH' /tmp/win.log
grep -E 'Symbols not found|Process completed with exit code' /tmp/win.log | head

# find the windows job id for a run:
gh run view <RUN_ID> --json jobs \
  -q '.jobs[] | select(.name|test("w64-mingw32")) | .databaseId'
```

## How to trigger a dry-run

```bash
gh workflow run release.yml --ref main -f mode=dry-run
gh run list --workflow=release.yml --limit 1 \
  --json databaseId,status,headSha -q '.[] | "id=\(.databaseId) \(.status) \(.headSha[0:7])"'
```

Dry-run builds + tests all targets and skips the GitHub Release publish step
(no tag, no public release). A `v*` tag push (or `mode=production`) does the real
release. **In-flight as of this writing:** dry-run `26591867203` on `b2e7740`
(building when this file was committed — check its result first on resume).

## Commits landed this session (all Windows / cross-platform fixes)

Other three targets were already green; every fix below targets MinGW/MachO/COFF.

- `c1191c5` align `getTypeFlagsOf` signature with the `CajetaTypeFlags` (uint64_t)
  typedef — LLP64 `unsigned long` is 32-bit and truncated 64-bit type IDs.
- `31a803c` portable death-test matchers (`KilledBySignal` is POSIX-only).
- `de7bd10` trap death-tests accept SIGILL **or** SIGTRAP (ARM `brk`); gate a
  Linux-only `/etc/passwd` canonical-path test.
- `1bd7417` make `runToString` shape detection deterministic across object formats
  (compare against the real `cajeta.lang.String#VTable` symbol, not a pointer sniff).
- `7960ba5` register the unrecoverable-exception vtable marker via a global ctor +
  runtime setter instead of an ELF-only weak global (MachO/COFF JIT can't override
  weak globals).
- `f4e258a` port System env/io tests off POSIX-only APIs for MinGW
  (`setenv`/`unsetenv` -> `_putenv_s` via `test/PortableEnv.h`; `captureFd`
  rewritten from non-blocking pipe to `tmpfile()` + `dup2`).
- `aa8c4f0` `test/jit/EmutlsShim.c`: `posix_memalign` -> `_aligned_malloc` /
  `_aligned_free` wrapper (MSVCRT has no `posix_memalign`).
- `4b0d438` link lld libs **before** LLVM component libs (`src/CMakeLists.txt`) so
  GNU ld (MinGW) resolves `llvm::opt` / `llvm::TarWriter` in its single pass.
- `84f3a3f` add the `lto` LLVM component (lld's ThinLTO BitcodeCompiler needs
  `libLLVMLTO`), gated on `CAJETA_HAS_LLD` so the Linux apt build — which lacks
  `libPolly.a`, a transitive `lto` dep — stays green; also link `bcrypt` for the
  native runtime's `BCryptGenRandom` on Windows.
- `b2e7740` normalize Windows `\` path separators in `CajetaModule`'s package-path
  derivation. Fixes the `CAJETA_ERROR_PACKAGE_MISMATCH` flood on Windows (and the
  shipped Windows AOT compiler, which used the same code).

## THE REMAINING ISSUE: Windows JIT can't resolve MinGW CRT symbols

### Symptom

Nearly every JIT-backed test on Windows fails with:

```
JIT session error: Symbols not found: [ write, unlink, strdup, stat64i32, rmdir,
  read, open, mkdir, lseek, ___chkstk_ms, getpid, ftruncate, __mingw_fprintf,
  __mingw_snprintf, __mingw_strtod, fstat64i32, close ]
... C++ exception "LLJIT initialize failed: Failed to materialize symbols:
  { (main, { <the entire cajeta stdlib> }) }"
```

The huge cajeta-symbol "Failed to materialize" list is a **cascade**: the embedded
runtime bitcode references those CRT functions, can't resolve them, so everything
depending on the runtime fails to materialize too. The CRT list is the root cause.

### Root cause

`test/jit/JitTestHelper.cpp:353` wires symbol resolution with only:

```cpp
auto generator = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
    jitState->jit->getDataLayout().getGlobalPrefix());
mainDylib.addGenerator(std::move(*generator));
```

On Linux/macOS, libc functions (`write`, `open`, `read`, ...) are dynamically
exported, so this generator (which searches loaded modules' export tables) finds
them. On Windows/MinGW these CRT functions are **statically linked** into
`cajeta_test.exe` and are **not in the PE export table**, so the generator can't see
them. This is fundamental to how the search generator works on Windows — there is no
config flag that fixes it. Affects ONLY the JIT test path; the AOT release binary
links the runtime natively and resolves these at link time (hence the release is
fine).

### Fix plan (do this on Windows where you can actually test it)

Add an explicit absolute-symbol map to the JIT dylib right after the generator is
added (JitTestHelper.cpp ~line 358), Windows-only. Mirror the existing
`EmutlsShim.c` pattern. Define the symbols the JIT needs and bind them to real
addresses in the test binary:

```cpp
#ifdef _WIN32
  auto& ES = jitState->jit->getExecutionSession();
  llvm::orc::SymbolMap m;
  auto reg = [&](llvm::StringRef n, void* p) {
    m[ES.intern(n)] = { llvm::orc::ExecutorAddr::fromPtr(p),
                        llvm::JITSymbolFlags::Exported };
  };
  // ... reg("write", (void*)&write); etc.
  llvm::cantFail(mainDylib.define(llvm::orc::absoluteSymbols(std::move(m))));
#endif
```

ABI gotchas to verify per symbol (this is why it needs a Windows box):
- `write/read/open/close/lseek/unlink/rmdir/getpid/strdup` — taking `&name` in the
  MinGW test TU should yield the right address (these alias the `_`-prefixed CRT
  funcs via MinGW's oldnames). Confirm the bitcode's reference and the test's `&name`
  resolve to the same MinGW header decl.
- `mkdir` — POSIX `mkdir(path, mode)` (2 args) vs Windows `_mkdir(path)` (1 arg).
  Needs a 2-arg wrapper that drops `mode`.
- `stat64i32` / `fstat64i32` — MinGW stat variants; verify the exact symbol and
  `struct stat` layout the bitcode expects; may need wrappers.
- `___chkstk_ms` — libgcc stack-probe intrinsic; declare `extern "C" void
  ___chkstk_ms();` and register `&___chkstk_ms`.
- `__mingw_fprintf` / `__mingw_snprintf` — varargs; register the real libmingwex
  symbols (declared in MinGW's stdio.h). `__mingw_strtod` is a normal signature.

Expect a **second wave** of missing symbols to appear once the first batch resolves
(the error only lists what's unresolved among currently-pulled objects). Iterate:
build `cajeta_test` locally on Windows, run a single JIT test
(`./build/test/cajeta_test --gtest_filter=WithAnnotationTests.fieldLevelWith`), read
the new "Symbols not found" list, add those, repeat until the suite passes.

### Alternative if the above proves too deep

`b2e7740` already cleared the package-mismatch class. If the CRT port stalls, the
honest fallback (consistent with release.yml's own comment about "known
platform-specific JIT issues … gating the release on them would be too tight a
contract for v0.x") is to accept the Windows JIT regression as a tracked known-issue
and ship — the release is green and the AOT binary is sound.

## Verifying locally (Linux dev box)

```bash
cmake --build build --target cajeta cajeta_test -- -j 4
CAJETA_SOURCE_ROOT="$(pwd)" ./build/test/cajeta_test            # full suite
CAJETA_SOURCE_ROOT="$(pwd)" ./build/test/cajeta_test --gtest_filter='Foo.*'
```

Working agreement during this push: do NOT run the full local regression on every
change — build/test the affected area, and rely on dry-runs to surface the next
cross-platform failure.
