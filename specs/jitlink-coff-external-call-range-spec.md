# jitlink-coff-external-call-range — defect (LLVM, fixed here; found by device-tests CI)

## 1. Definition

**1.1 Symptom.** With the COMDAT and `__imp_` defects fixed (r8), every JIT'd
module that calls the host C runtime failed to link:

```
JIT session error: In graph cajeta_test-jitted-objectbuffer, section .text:
  relocation target 0x7ff8fe67d870 (strcmp) is out of range of PCRel32 fixup
  at address 0x27d219e3000 + 0x232
```

Measured on PHOENIX under r8: **24 of 24 `BinaryOpTests` failed**, all on this
one signature — `strcmp` ×22, `__divti3` ×1, `fmod` ×1.

**1.2 Root cause.** A COFF `IMAGE_REL_AMD64_REL32` fixup reaches ±2 GB. That
covers any reference inside one JIT'd object, but a call into the host process
does not: on Windows the JIT slab is commonly allocated low (`0x27d…`) while
loaded modules sit near `0x7ff8_00000000` — several terabytes apart. JITLink's
COFF/x86-64 backend had no stub mechanism for out-of-range branches.

**1.3 Why this is COFF-specific and why nobody hit it.** ELF gets the same
service free: `R_X86_64_PLT32` marks a call, `R_X86_64_PC32` marks a data
reference, and `x86_64::PLTTableManager` stubs the former. COFF has a *single*
REL32 relocation for both, so the distinction has to be inferred — which is
presumably why the backend never grew one. RuntimeDyld's COFF backend has
always emitted stubs for external REL32
(`RuntimeDyldCOFFX86_64.h`: `if (IsExtern) generateRelocationStub(...)`), so
the gap only shows once a client forces JITLink on COFF, which LLJIT does not
do by default (`UseJITLink = !TT.isOSBinFormatCOFF()`).

**1.4 Fix.** `buildTables_COFF_x86_64` in `PostPrunePasses`, using the same
`x86_64::GOTTableManager` / `PLTTableManager` ELF uses, plus
`x86_64::optimizeGOTAndStubAccesses` in `PreFixupPasses` so a target that lands
in range after allocation is rewritten back to a direct branch and the stub
costs nothing. Shipped as `cajeta-llvm-23-r9`.

**1.5 Only CALL/JMP/Jcc sites are promoted — this is load-bearing.** The first
version promoted every external REL32 and broke upstream's
`COFF_external_var.s`, which asserts that an out-of-range
`movl var(%rip), %eax` must **error**. A jump stub is valid only for control
transfer; routing a data load through one reads the stub's code bytes instead
of the variable. The opcode preceding the displacement decides:

```
E8 cd      call rel32
E9 cd      jmp rel32
0F 8x cd   jcc rel32
```

Anything else stays `PCRel32`, so an out-of-range data access still fails, as
it must.

## 2. Acceptance

- **2.1** A call to an external symbol beyond PCRel32 range links.
  **MET** — new test `COFF_external_call_stub.s`, red without the change.
- **2.2** An out-of-range data reference to an external still errors.
  **MET** — `COFF_external_var.s` continues to pass.
- **2.3** No regression in the JITLink suite. **MET** — 106 passed, 0 failed.
  One existing test, `COFF_comdat_weak_duplicate.s`, had two pinned block
  addresses updated: the stubs section shifts its block by a page. The
  addresses are incidental to what that test checks (duplicate comdat-any
  definitions must not collide) and ELF's equivalent tests already account for
  a PLT section — but it *is* an upstream test edited to accommodate this
  change, and the fork commit says so.
- **2.4** `BinaryOpTests` passes on Windows under r9. **PENDING** — the
  measurement that decides whether the chain ends here.

## 3. Upstreaming

This one is written here, not taken from upstream, and there is no equivalent
upstream. It should go to `llvm/llvm-project` with `COFF_external_call_stub.s`
so the fork can drop it on the next rebase — same disposition as
[jitlink-coff-comdat-section-symbol](jitlink-coff-comdat-section-symbol-spec.md) §3.4.
