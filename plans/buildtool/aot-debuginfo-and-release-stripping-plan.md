# AOT Debug Info & Release Stripping Plan

Status: **proposed** (not started). Owner: TBD.

A real, self-contained feature: give `--emit=exe`/`--emit=obj` builds a proper
**debug-info + split + strip** pipeline (frontend DWARF metadata + lld
split/strip), so debug builds are debuggable in a native debugger and release
builds are lean with symbols archived for crash symbolication. Includes the
debugging + releases documentation.

---

## 0. Current state (baseline)

- **The debugger is JIT-based, not DWARF-based.** `CompilerFlags::debugInfo`
  (`--debug-info` / `-g`) emits `__cajeta_dbg_safepoint(loc_id)` at statement
  boundaries so the in-process DAP debugger (`cajeta dap`) can park a fiber at a
  breakpoint. It is codegen instrumentation, **not** symbol tables. (See the
  facets/ownership-viz work — [[cajeta-debugger-ownership-viz]].)
- **The AOT path emits no DWARF and never strips.** `--emit=exe` links via lld
  in-process; the binary carries whatever LLVM leaves, ungoverned.
- **The flavor vocabulary already anticipates this but is inert.** `debug-info`
  (`off|line|full`), `strip-symbols` (bool), and `lto` (`off|thin|full`) are
  declared in `flavorPropertyVocab()` but lower to **no compiler frontend flag**
  (`Flavor.cpp::toCompilerFlags`, `compilerFlag == ""`). So a debug vs. release
  exe differs **only** in `opt` (O0/O2) and `bounds-check` (on/off) today.
- Net gaps: no source-level debugging of AOT binaries; no lean stripped
  releases; no crash symbolication.

## 1. Goals

- [ ] Debug-flavor AOT binaries are debuggable in a native debugger (lldb/gdb/VS)
      and/or symbolicate post-mortem.
- [ ] Release-flavor binaries are **stripped**, with DWARF/symbols **split to a
      sidecar** that the release pipeline archives (symbol-server model).
- [ ] The existing flavor knobs (`debug-info`, `strip-symbols`, and a decision on
      `lto`) are wired to real behavior, replacing the inert mappings.
- [ ] First-class docs for the debugging story and the release/symbol story.

## 2. Design

### 2a. Reconcile the two debug-info worlds
Define one `--debug-info=off|line|full` axis that governs **both** mechanisms:

| `--debug-info` | DWARF | JIT safepoints (`cajeta dap`) |
|---|---|---|
| `off`  | none | off |
| `line` | line tables only | off (or opt-in) |
| `full` | full DWARF (types, locals, ownership facets) | on |

Decision needed: keep the JIT-safepoint instrumentation orthogonal (its own
flag) or fold it under `full`. (Recommendation: fold under `full`; expose an
escape hatch.)

### 2b. Frontend DWARF metadata
- [ ] Emit LLVM debug metadata during codegen (`DICompileUnit`, `DISubprogram`,
      `DILocation`, `DILocalVariable`, `DIType`), gated by `--debug-info`.
- [ ] Map Cajeta types → DWARF types; surface ownership/drop state where it adds
      value (ties into the debugger facets, CP7).
- [ ] Honor `--debug-prefix-map` (already emitted by the build tool's
      reproducibility set, see `Reproducibility.cpp`) when writing source paths
      into DWARF — reproducible, relocatable debug info.

### 2c. Split + strip at link (lld, per platform)
- [ ] **ELF:** `-gsplit-dwarf` → `.dwo`/`.dwp`, or `--only-keep-debug` +
      `--strip-debug` + `.gnu_debuglink`.
- [ ] **Mach-O:** `dsymutil` → `.dSYM` bundle, then `strip`.
- [ ] **PE/COFF:** `.pdb` via the LLVM CodeView path.
- [ ] Drive split/strip through the in-process lld emit pipeline (no external
      objcopy/strip dependency where avoidable).
- [ ] `strip-symbols` (flavor bool) strips the shipped binary; release default
      `true`, debug `false`.

### 2d. Symbol sidecar + archiving
- [ ] Release build emits `<binary>` (stripped) **plus** `<binary>.{dwp,dSYM,pdb}`.
- [ ] The build-tool `package` / `publish` actions collect + ship the sidecar.
- [ ] Release workflow uploads symbol sidecars alongside installers (so a crash
      report from a shipped binary can be symbolicated). Coordinates with the
      installer plan ([[cajeta-installer-progress]]).

### 2e. `lto` decision
- [ ] Decide whether `lto=off|thin|full` maps to a real LLVM LTO pass in the
      `--emit=exe`/`uber` pipeline (the compiler already operates on bitcode, so
      thin-LTO across user + built-in stdlib + deps is feasible) or stays
      reserved. If implemented, wire `--lto` + the flavor mapping.

## 3. Compiler CLI surface (to add)

- [ ] `--debug-info=off|line|full` — wire to frontend DI + JIT safepoints.
- [ ] `--strip-symbols=on|off`.
- [ ] `--split-debug=on|off` (or implied by `strip-symbols` + `debug-info`).
- [ ] `--lto=off|thin|full` (if §2e lands).
- [ ] Once these exist, update the `compilerFlag` mappings in
      `flavorPropertyVocab()` so `toCompilerFlags` lowers them (today they map to
      `""`).

## 4. Build-tool integration

- [ ] Flavor → real flags: set `compilerFlag` for `debug-info`, `strip-symbols`,
      `lto` in `Flavor.cpp` once the compiler accepts them.
- [ ] `package`/`publish`: collect + ship the symbol sidecar.
- [ ] Release workflow: archive symbols per target.

## 5. Documentation (REQUIRED deliverable of this plan)

- [ ] **`docs/Debugging.md`** — the two debuggers: live in-process
      JIT/DAP debugging (`cajeta dap`, IDE plugin) vs. native DWARF debugging /
      post-mortem; what `--debug-info=off|line|full` does; how to debug a debug
      build; how to symbolicate a release crash with the archived sidecar.
- [ ] **`docs/Releases.md`** (or a section in `BuildTool.md`) — debug vs.
      release flavors and exactly what each binary contains (opt, bounds,
      symbols); stripping + split-debug; the symbol-server / sidecar archiving
      model; reproducible builds (`--source-date-epoch`, `--debug-prefix-map`,
      `--seed`).
- [ ] Update `docs/CompilerModes.md` + `BuildTool.md` "Built-in flavors" /
      "Property vocabulary" tables to reflect `debug-info`/`strip-symbols`/`lto`
      once they are wired (they are currently documented as *reserved /
      not-yet-plumbed*).

## 6. Phasing

- [ ] **Phase 1** — frontend DWARF **line tables** (`--debug-info=line`) + an
      lldb/gdb smoke test (breakpoint + line step on a debug build). Ship
      `Debugging.md` draft.
- [ ] **Phase 2** — full DWARF (types, locals, ownership facets),
      `--debug-info=full`, unify with JIT safepoints.
- [ ] **Phase 3** — split-debug + strip (`strip-symbols`, per-platform sidecar).
      Ship `Releases.md`.
- [ ] **Phase 4** — symbol archiving in `package`/`publish` + release workflow.
- [ ] **Phase 5** — `lto` decision / implementation.

## 7. Open questions

- [ ] Unify or keep separate the JIT-safepoint and DWARF debug models?
- [ ] Release default: strip (recommended) vs. keep symbols?
- [ ] Symbol hosting: GitHub release asset vs. a dedicated symbol server?
- [ ] thin-LTO as the release default?
