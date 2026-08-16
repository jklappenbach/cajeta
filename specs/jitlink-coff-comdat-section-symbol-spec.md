# jitlink-coff-comdat-section-symbol — defect (LLVM, upstream; found by device-tests CI)

## 1. Definition

**1.1 Symptom.** On Windows/COFF, with JITLink forced on (which cajeta must do —
see `windows-jit-coff-reloc`), every JIT link of a cajeta module fails:

```
cajeta.jit: COFF host — JITLink object layer installed
JIT session error: Could not find symbol at given index, did you add it to JITSymbolTable? index: 13, section: 10
C++ exception with description "LLJIT initialize failed: Failed to materialize symbols: {...}"
```

99 of the 259 device tests failed on this one signature in run 31762622334.
It is not device-specific: `BinaryOpTests.int128WidenNarrow`, a plain
expression test, reproduces it exactly.

**1.2 Attribution: LLVM, conclusively.** The object cajeta hands JITLink is
well-formed COFF. **`lld-link` 23.0.0 — the same LLVM revision — links that
exact object successfully** (`/dll /noentry /force`, RC=0, 3.6 MB image). A
real COFF linker accepts what JITLink rejects, so the defect is in JITLink's
COFF LinkGraph builder, not in what we emit.

**1.3 Root cause.** `COFFLinkGraphBuilder::graphifySymbols` assumes every COMDAT
section carries **exactly two symbols in sequence**. Its own comment states it:

> Since two symbols always come in a specific order, we initiate pending COMDAT
> export request when we encounter the first symbol and actually exports it
> when we process the second symbol.

`createCOMDATExportRequest` therefore records `PendingComdatExports[SecIndex]`
and returns `nullptr` — the section symbol gets **no** graph symbol. The
back-fill (`setGraphSymbol(..., PendingComdatExport->SymbolIndex, ...)`) lives
only in `exportCOMDATSymbol`, which runs when a *second, external* symbol in
that section is processed. There is no end-of-pass flush.

The assumption is false for exception-handling COMDATs. Our object:

```
[10](sec  6) .text$__cajeta_vrel_cajeta_lang_Utf8   Selection: Any (0x2)
[12](sec  6) __cajeta_vrel_cajeta_lang_Utf8          <- leader; back-fills [10]
[13](sec  7) .xdata$__cajeta_vrel_cajeta_lang_Utf8  Selection: Any (0x2)
[15](sec  8) .drectve                                <- no leader for sec 7
```

`.xdata$name` holds unwind data. It is not externally named, so it has **only**
its section symbol. `PendingComdatExports[7]` is set and never flushed;
`GraphSymbols[13]` stays null. Then the COMDAT `.pdata` entry references it:

```
Section (11) .pdata$__cajeta_vrel_cajeta_lang_Utf8 {
  0x0 IMAGE_REL_AMD64_ADDR32NB .text$__cajeta_vrel_cajeta_lang_Utf8 (10)   OK — [10] was back-filled
  0x4 IMAGE_REL_AMD64_ADDR32NB .text$__cajeta_vrel_cajeta_lang_Utf8 (10)   OK
  0x8 IMAGE_REL_AMD64_ADDR32NB .xdata$__cajeta_vrel_cajeta_lang_Utf8 (13)  FAILS
}
```

`COFF_x86_64.cpp::addSingleRelocation` calls `getGraphSymbol(13)`, gets null,
and raises the error. (The message's "section: 10" is `SectionRef::getIndex()`,
which is 0-based on COFF — COFF section number 11, the `.pdata$` COMDAT.)

**1.4 Nothing exotic on our side.** `__cajeta_vrel_cajeta_lang_Utf8` is an
ordinary comdat/weak function. Any C++ inline function on Windows produces the
same `.text$x` / `.xdata$x` / `.pdata$x` triple. The bug reaches any COFF
JITLink client whose input has a COMDAT function with unwind info — which is
essentially all of them.

**1.5 Not fixed upstream, and not reported.** The fork's
`COFFLinkGraphBuilder.cpp` and `.h` (branch `cajeta-spirv`, forked from
`203c0668d`, 2026-06-01) are **byte-identical to `llvm/llvm-project` main**, so
this is not fork drift and no upstream commit addresses it. A GitHub issue
search for `"did you add it to JITSymbolTable"` in `llvm/llvm-project` returns
zero results. The path is rarely exercised because LLJIT deliberately avoids
JITLink on COFF (`UseJITLink = !TT.isOSBinFormatCOFF()`), which is exactly why
`applyCoffJitLink` has to force it.

**1.6 Upstream JITLink/COFF work the fork has NOT taken** (neither fixes this):
- `54a7896ac` (2026-06-19) [JITLink][COFF] Synthesize `__imp_` IAT entries (#203906)
  — the only delta in the fork's `COFF_x86_64.cpp`, +78 lines.
- `eee7c2d22` (2026-08-02) [ORC] AutoImportGenerator for COFF dllimport auto-import (#203914).

**1.7 Reproduce.** On Windows, with `applyCoffJitLink` active:

```
build/test/cajeta_test.exe --gtest_filter=BinaryOpTests.int128WidenNarrow
```

Capture the object with `CAJETA_DUMP_OBJ=<dir>` (see `JitCoffLinking.h`), then
`llvm-objdump --syms` / `llvm-readobj --relocations` to see the above, and
`lld-link /dll /noentry /force` to confirm the object is good.

## 2. Fix shape

**2.1** In `COFFLinkGraphBuilder::graphifySymbols`, after the symbol loop, flush
any `PendingComdatExports[SecIndex]` still set: a COMDAT section with no
leader symbol should get an ordinary defined graph symbol for its *section*
symbol (`Scope::Local`, the recorded `Linkage`), so relocations naming it
resolve. ~10 lines, no change to the two-symbol path.

**2.2** Alternative considered and rejected: special-casing `.xdata$`/`.pdata$`
by name. The defect is structural — any leaderless COMDAT hits it — and a
name-based patch would not be upstreamable.

## 3. Acceptance

- **3.1** `BinaryOpTests.int128WidenNarrow` passes on Windows with JITLink
  forced (it is the cheapest reproducer, ~100 s).
- **3.2** The `*DeviceTests*` filter on `windows-nvidia` reports zero failures
  carrying the "Could not find symbol at given index" signature; the 99
  failures of run 31762622334 become a real pass/fail split.
- **3.3** The ELF path is unaffected — the same module already links and passes
  on Linux, and any fix is inside the COFF builder.
- **3.4** The fix is submitted upstream, not only carried in the fork. The fork
  exists for SPIR-V work; a JITLink COFF correctness fix belongs in
  `llvm/llvm-project` so the fork can drop it on the next rebase.
