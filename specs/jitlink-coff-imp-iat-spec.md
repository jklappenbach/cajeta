# jitlink-coff-imp-iat — defect (LLVM, fixed upstream; found by device-tests CI)

## 1. Definition

**1.1 Symptom.** On Windows/COFF with JITLink forced on, once the leaderless-COMDAT
defect ([jitlink-coff-comdat-section-symbol](jitlink-coff-comdat-section-symbol-spec.md))
was fixed, every link failed at symbol resolution instead:

```
JIT session error: Symbols not found: [ __imp_MapViewOfFile, __imp__errno,
  __imp_LoadLibraryA, __imp__commit, __imp_bind, __imp_VirtualFree,
  __imp_longjmp, __imp_accept, __imp_UnmapViewOfFile, ... ]
LLJIT initialize failed: Failed to materialize symbols: { (main, { ... }) }
```

**1.2 Found.** 2026-08-14 on PHOENIX, immediately behind the COMDAT fix (r7).
Reproduces on `BinaryOpTests.int128WidenNarrow` — no GPU, no device test.

**1.3 Root cause.** A dllimport reference is emitted as an indirect access
through a named `__imp_X` symbol (`callq *__imp_bar(%rip)`). Nothing in
JITLink's COFF path defined those slots, so every Windows API the runtime
touches came back unresolved. RuntimeDyld resolved them through its own path,
which is why this never appeared before `applyCoffJitLink` forced JITLink —
and the COMDAT crash then hid it, because the link died during graph
construction before any lookup ran.

**1.4 Fix — taken from upstream, not written here.** `54a7896ac`,
"[JITLink][COFF] Synthesize `__imp_` IAT entries" (#203906), landed upstream
2026-06-19. The fork branched from `203c0668d` on 2026-06-01, eighteen days
earlier, so it simply predated the fix. Cherry-picked onto `cajeta-spirv` and
shipped as `cajeta-llvm-23-r8`.

The pass defines `__imp_X` over an 8-byte pointer slot holding X's address and
leaves X as an ordinary external, resolved by whatever generator serves the
JITDylib — on Windows, the host process.

**1.5 Verification.** Against cajeta's own 5.5 MB stdlib COFF object, the
unresolved set changes from `__imp_`-prefixed names to the bare names
(`CloseHandle`, `_get_osfhandle`, `GetSystemInfo`, `BCryptGenRandom`, ...) —
exactly the transformation the pass promises. Those resolve from the process
on Windows; on Linux they do not exist, which is why the Linux check still
lists them. Confirmed dead on PHOENIX under r8: zero `__imp_` occurrences.

**1.6 Not taken: `eee7c2d22`** ("[ORC] Add AutoImportGenerator for COFF
dllimport auto-import", #203914). It targets a later ORC API than the fork's
base — its lookup returns `std::optional<ExecutorAddr>` upstream but
`std::optional<ExecutorSymbolDef>` here, so `COFFAutoImportGenerator.cpp:73`
does not compile. It is also bound to a single named DLL, which does not help
symbols that live in the host executable (`__divti3`). Cherry-picked, then
reverted with that reasoning recorded in the fork history.

## 2. Acceptance

- **2.1** No `__imp_`-prefixed symbol appears in an unresolved-symbols error on
  Windows. **MET** under r8.
- **2.2** The fork carries the upstream commit rather than a local
  reimplementation, so it drops out on the next rebase. **MET** (cherry-picked
  with `-x`).
