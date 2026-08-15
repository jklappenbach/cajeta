# jitlink-coff-seh-drop-uaf — defect (ours; found by the release subset on PHOENIX)

## 1. Definition

**1.1 Symptom.** With the COFF JIT chain otherwise complete (fork tag r9), the
release subset on PHOENIX came back:

```
Passed: 421   Failed: 0   Timed out: 11   Crashed: 14   (of 446)
```

Re-run one process per test: **12 `HashMapStreamParallelTests.*` +
`CastTests.roundToIntModes` crash with a bare SIGSEGV (rc=139)** — no gtest
failure, no JIT session error, no cajeta diagnostic. The last line on stderr is
`cajeta.jit: COFF host — JITLink object layer installed`.

All 13 **pass** under `CAJETA_COFF_JIT=off` (RuntimeDyld). So this is a
regression introduced by the JITLink switch, not a pre-existing defect it
exposed.

**1.2 Isolation.** One crashing test, five configurations:

| config | result |
|---|---|
| baseline (all changes) | SIGSEGV |
| `CAJETA_COFF_NOCLAIM=1` (no responsibility overrides) | SIGSEGV |
| `CAJETA_COFF_KEEPALIVE=1` (markAllSymbolsLive) | SIGSEGV |
| `CAJETA_COFF_KEEP_SEH=1` (**plugin not installed**) | link error, no crash |
| `CAJETA_COFF_JIT=off` (RuntimeDyld) | **pass** |

The only configuration that does not crash is the one that does not run
`DropSehFramesPlugin`. That plugin is ours (commit 88e0a9b1), not LLVM's.

**1.3 Root cause — use-after-free in `DropSehFramesPlugin`.** The pass removed
`.pdata*` sections with a bare `LinkGraph::removeSection()`. That is not a safe
operation on a section anything still points at:

- `LinkGraph::removeSection(Sec)` is exactly `Sections.erase(Sec.getName())`
  (`JITLink.h:1653`). Nothing scrubs references.
- `~Section()` then destroys every `Symbol` and `Block` the section owns
  (`JITLink.cpp:165`).
- COFF records an associative COMDAT as an edge in the **inbound** direction:
  `.pdata$fn` is associative to `.text$fn`, and `COFFLinkGraphBuilder`
  (`IMAGE_COMDAT_SELECT_ASSOCIATIVE`, `COFFLinkGraphBuilder.cpp:590`) emits
  `getGraphBlock(Target)->addEdge(Edge::KeepAlive, 0, *GSym, 0)` — a KeepAlive
  **from the `.text` block into the `.pdata` symbol**. Every COMDAT function in
  the graph has one, and our modules are mostly template instantiations, so
  there are hundreds.
- `jitlink::prune()` runs immediately after the PrePrune passes
  (`JITLinkGeneric.cpp:35`) and, for every edge of every live block, does:

  ```cpp
  if (E.getTarget().isDefined() && !E.getTarget().isLive())
    Worklist.push_back(&E.getTarget());
  E.getTarget().setLive(true);
  ```

  — a read **and a write** through the dangling `Symbol*`, and then, for any
  freed symbol pushed onto the worklist, a walk of a freed `Block`'s edge list.

The freed memory is usually still intact, which is why 421 of 434 tests linked
and ran correctly over it and only 13 died. That ratio is the signature of the
bug, not evidence against it.

**1.4 Why it looked like an LLVM problem.** Three of our changes landed
together (leaderless-COMDAT patch, external-call stubs, SEH drop) and the two
LLVM-side ones were the plausible suspects. Both were cleared:

- The stub pass sets `x86_64::BranchPCRel32` in a PostPrune pass, and
  `COFFLinkGraphLowering_x86_64` runs later in PreFixup. Those two enums do not
  collide — `EdgeKind_coff_x86_64::PCRel32 = x86_64::FirstPlatformRelocation`,
  so COFF kinds sit strictly above every generic kind and our edges fall
  through the lowering switch's `default:` untouched.
- The responsibility overrides and dead-stripping were both excluded by the
  A/B in §1.2.

**1.5 Fix.** Scrub inbound edges before removing the sections
(`src/cajeta/jit/JitCoffLinking.h`, `dropSehFrames`): walk every block outside
the doomed set and `removeEdge` any edge whose target is a defined symbol
belonging to a doomed section, then `removeSection`. The pass body was lifted
out of the plugin lambda into a named free function so it can be unit-tested
directly.

## 2. Acceptance

- **2.1** `dropSehFrames` removes every `.pdata*` section.
  **MET** — `DropSehFramesTests.removesPdataSections`.
- **2.2** No edge anywhere in the graph targets a symbol the graph no longer
  owns after the pass. **MET** —
  `DropSehFramesTests.dropsInboundKeepAliveEdgesBeforeRemovingSection`; red
  before the fix with `edge in section .text$fn targets a symbol the graph no
  longer owns`.
- **2.3** The scrub is targeted — edges that do not point into a removed
  section survive. **MET** — `DropSehFramesTests.preservesEdgesThatDoNotTargetPdata`,
  also red before the fix (2 edges instead of 1).
- **2.4** A graph with no unwind tables is untouched. **MET** —
  `DropSehFramesTests.noOpWhenNoPdataPresent`.
- **2.5** The 13 crashing tests pass on PHOENIX under JITLink. **MET** —
  13/13, one gtest process each, on the merged tree (`711bad53`, r9 toolchain).
  The `HashMapStreamParallelTests` also got **2.5x faster** — 64-68s each,
  against 130-176s with the corruption present. That is corroboration, not a
  bonus: a freed symbol pushed onto `prune()`'s worklist makes it walk graph it
  should never have visited.
- **2.6** The release subset is green on PHOENIX. **PENDING**.

The tests build a `LinkGraph` by hand rather than driving the JIT, so this
COFF-only pass is covered on Linux and in CI, not only on the Windows runners
where it bit.

## 3. Follow-up — `removeSection` is a footgun worth reporting

Nothing in LLVM stops this. `LinkGraph::removeSection()` has no doc comment
warning about inbound references and no assertion; upstream only ever calls it
from `mergeSections()`, which has already moved every block and symbol out, so
the section it removes is empty and the hazard never shows. A client that
removes a *populated* section — the obvious reading of the API — silently
corrupts the graph.

Worth sending upstream with the JITLink COFF patches: a doc comment stating the
precondition, and a debug assertion that no edge in the graph targets a symbol
in the section being removed. Cheap, and it converts a silent
one-in-thirty-modules memory corruption into an immediate failure.

## 4. Follow-up — SEH is dropped, not supported

Dropping `.pdata` is still the right call *today* only because nothing
registers it: cajeta's JIT never calls `RtlAddFunctionTable`, so the OS has
never known about these tables and SEH has never been able to unwind through a
JIT'd frame on Windows.

The reason it cannot simply be kept is `__ImageBase`. `.pdata` entries are
32-bit RVAs (`IMAGE_REL_AMD64_ADDR32NB` → `x86_64::Pointer32`, computed as
`target - __ImageBase`), and JITLink resolves `__ImageBase` as an ordinary
external — `COFFLinkGraphBuilder::addImageBaseSymbol()` — which on Windows
binds to the **host executable's** image base near `0x7ff6…` while the JIT slab
lands near `0x27d…`. The delta does not fit in 32 bits, hence
`section .pdata: relocation target … is out of range of Pointer32 fixup`.

RuntimeDyld avoids this by defining its own image base at the JIT allocation
and registering the tables against it. The equivalent for JITLink is a
PostAllocation pass that rewrites the `__ImageBase` symbol's address to the
slab base, plus a PostFixup `RtlAddFunctionTable` registration — doable
entirely in a cajeta-side plugin, no LLVM change. That is worth doing when
Windows JIT stack traces or exception propagation through JIT'd frames matter;
it is out of scope for this release. Until then the pass stays and
`CAJETA_COFF_KEEP_SEH=1` remains the escape hatch.
