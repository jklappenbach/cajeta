# Cajeta Debugger Memory Viewers — Spec

> Status: **DRAFT for review** (design skill). Specs the **memory viewers** for
> the IntelliJ debugger and **how each works** end to end (UI → DAP → runtime
> seam). Four distinct viewers, each its own section: **(A) raw memory / hex**,
> **(B) register / peripheral**, **(C) heap / arena / allocation-map**,
> **(D) flash / RAM footprint** (static budget + runtime overlay).
>
> These are runtime/space + footprint views — distinct from and complementary to
> the shipped **CP7 memory-facet** visualization (per-*variable* ownership /
> alloc-class / lifetime annotation; `ide-plugin-debug-fr-1.md`). Companions:
> [`docs/Debugging.md`](../docs/specification/debugging/Debugging.md) (DAP contract),
> [`docs/specs/frame-arena-spec.md`](archive/frame-arena-spec.md) (the bump arena),
> [`docs/specification/lang/MemoryModel.md`](../docs/specification/lang/MemoryModel.md)
> (ownership/drop), [`docs/Embedded.md`](../docs/specification/embedded/Embedded.md) (target tiers + per-tier
> size targets). Plan lives at `agents/memory-viewer-plan.md` once approved.

---

## 1. Definition

### 1.1 Purpose
Give a developer, while stopped in the debugger (and, for footprint, also right
after a build), direct views of **memory** — bytes at an address, hardware
registers/peripherals, the live heap/arena, and the flash/RAM budget — for both
host debugging and (with the remote enabler, §1.4) on-device embedded/robotics
targets. Where CP7 answers "what does *this binding* own and where does it live,"
these answer "what is *at this address / in this region / in this binary* right
now."

### 1.2 Scope (v1)
- **(A) Raw memory / hex viewer** — read (and optionally write) bytes at an
  address or `memoryReference`, hex+ASCII grid, navigable, live-updating.
- **(B) Register / peripheral viewer** — memory-mapped registers/peripherals
  decoded from a descriptor, read via the same memory path, bitfield decoding.
- **(C) Heap / arena / allocation-map viewer** — live picture of the runtime's
  bump arena occupancy and the heap drop-chain/live-set.
- **(D) Flash / RAM footprint viewer** — a **static** section-based flash/RAM
  budget (post-build, no run) **plus a runtime overlay** (static + stack
  high-water + heap/arena live-set) against the target's RAM ceiling.
- Navigation glue (variable → "Show in memory"; stop → live refresh) and editing
  (write-back) shared across A/B (§6).

**Placement.** All four viewers live **in the debugger** — a "Memory" tool window
/ debug-session tabs alongside Variables/Watches — and **never in the build-tool
widget**. Viewers A/B/C require an active (or attached) session. Viewer D is the
one with a static half: it lives in the *same debug-view Memory panel*, shows the
static flash/RAM budget from the last build **even with no active session**, and
adds the runtime overlay once a session is live. The build widget at most offers
a "Show footprint" action that opens this debug-view panel — it does not host a
memory viewer.

### 1.3 Problem
The debugger today shows variables as `name : type = value` (+ CP7 facets) but
offers **no** view of raw memory, a hardware register, the heap/arena as a whole,
or the binary's flash/RAM footprint. Embedded and robotics work needs all four:
inspecting MMIO/DMA buffers and registers (A, B), watching constrained RAM for
leaks/fragmentation and no-alloc-in-hot-loop discipline (C), and confirming the
program *fits* the device — static + worst-case stack + heap high-water ≤ RAM
(D). Cajeta's ownership model + bump arena make C/D unusually faithful — the
runtime already knows owners, drop order, alloc class, and arena marks.

### 1.4 Constraints & dependencies
- **Memory access over DAP.** A/B read via DAP `readMemory` and write via
  `writeMemory`; **cajeta dap implements neither yet** (§7.1). A variable's
  address is already carried in the frame chain
  (`__cajeta_dbg_local{name,type,addr}`, CP5/CP7), so a `memoryReference` is
  derivable today.
- **Heap/arena introspection.** C (and D's runtime overlay) read the per-thread
  bump arena (`cajeta_arena`: base/cursor/mark/reserved,
  `runtime/native/cajeta_runtime.c` §frame-arena) and the drop-chain/live-set via
  **stateless host accessors** (the CP5/CP7-1b/1c `__cajeta_dbg_*` pattern); a new
  accessor surface is required (§7.2).
- **Footprint sources.** D's static half reads section sizes from the build
  artifact / a `cajeta build` size report (§7.5); its runtime half needs a
  per-carrier **stack high-water** probe (§7.6) plus C's heap/arena data.
- **On-device / remote.** Host (JIT-in-process) debugging reads host process
  memory now; **on-device** A/B/C/(D-runtime) require the **AOT + DWARF +
  remote-attach** debug path (`Plan.md` Part A; `cajeta dap --port` exists but
  the in-process model can't attach to a separate device process). Shared with
  the embedded-targets spec; called out, not solved here. D's **static** half
  needs no device and no session.
- **Behavioral cores plain JVM** — hex model, register-decode, arena/heap model,
  section/budget model are unit-tested without a platform fixture.

### 1.5 Non-goals (v1)
- Replacing CP7 per-variable facet viz (orthogonal; these are memory-space +
  footprint views).
- A full disassembler / mixed source+asm view.
- Time-travel / historical memory replay (live state only).
- Authoring peripheral descriptors in-IDE — descriptors are ingested.

---

## 2. Viewer A — Raw memory / hex

### 2.1 How it works
1. The user opens the viewer for a `memoryReference` — from a variable's "Show in
   memory" (the binding's `addr` → `memoryReference`), a typed address, or a
   register (§3).
2. The panel issues DAP **`readMemory{memoryReference, offset, count}`**; the
   server returns base64 bytes + unreadable ranges.
3. A plain-JVM **HexModel** lays the bytes into a hex+ASCII grid (width,
   endianness, group size), marking unreadable spans.
4. On each stop/step the visible window re-reads (§6); edits issue **`writeMemory`**.

### 2.2 Requirements
Hex+ASCII grid; goto-address; selectable width/endianness/group; data-type
overlay (int8/16/32/64, f32/f64, ptr); mark unreadable/unmapped ranges; copy-as
(hex/array). Reads are windowed — only the visible range (§8.1).

### 2.3 Use cases
- **2.3.1** As a developer stopped at a breakpoint, when I choose "Show in
  memory" on a buffer variable, then the hex viewer opens at its address.
- **2.3.2** As a developer, when I type an address + count, then those bytes
  render hex+ASCII with unmapped ranges marked, never erroring.
- **2.3.3** As a developer, when I select 4 bytes as "int32-LE", then the decoded
  value shows.
- **2.3.4** As a developer, when I step, then the visible window refreshes.

---

## 3. Viewer B — Register / peripheral

### 3.1 How it works
1. A **peripheral descriptor** (CMSIS-SVD-style or a Cajeta `@Native` register
   map) is ingested per target → peripherals → registers → bitfields with
   absolute addresses, access (RO/RW/W1C), reset values.
2. Reading a register issues DAP **`readMemory`** at its address; a plain-JVM
   **RegisterDecoder** splits the word into named bitfields with enum values.
3. Writing composes the new word (access-aware) and issues **`writeMemory`**;
   W1C/RO fields are guarded.
4. Live-refresh on stop (§6).

### 3.2 Requirements
Peripheral tree; per-register hex + decoded bitfields with names/enums/access;
edit RW fields with access-aware composition; reset-value and changed-since-stop
highlighting; descriptor source per target (§7.3). Host targets with no MMIO show
empty/disabled, not broken.

### 3.3 Use cases
- **3.3.1** As an embedded developer on an STM32 with its SVD loaded, when I
  stop, then I read e.g. `GPIOA.MODER` decoded into per-pin mode fields.
- **3.3.2** As a developer, when I write a RW bitfield, then only that field's
  bits change and the device reflects it.
- **3.3.3** As a developer, when a register is RO/W1C, then the editor
  prevents/encodes writes per access semantics.
- **3.3.4** As a developer on a host target (no MMIO), then the viewer is cleanly
  empty/disabled.

---

## 4. Viewer C — Heap / arena / allocation-map

### 4.1 How it works
Reads runtime structures Cajeta already maintains, via new host accessors (§7.2):
1. **Arena occupancy** — read the per-thread bump arena `cajeta_arena`
   (`base/cursor/mark/reserved`; `frame-arena-spec.md`): committed vs reserved,
   mark watermark(s), trim threshold → occupancy bar/map per carrier.
2. **Heap allocations** — walk the **drop chain / live-set** (CP7-1c owner
   drop-entry chain) for live heap allocations
   `{addr,type,size,owner-binding,drop-order,alloc-class}`; alloc-class colored
   with the CP7 facet palette (single source of truth).
3. **Fragmentation / map** — lay allocations on an address-ordered map to show
   gaps (heap) and linear arena fill.
4. Select an allocation → cross-link to its owner binding (Variables view) and
   its bytes (Viewer A). Live-refresh on stop.

### 4.2 Requirements
Per-thread arena occupancy (committed/reserved/mark); live heap allocation list
(type/size/owner/drop-order/alloc-class); address-ordered map with fragmentation;
totals + high-water; navigation to owner + raw bytes; refresh on stop. Accessors
**safe to call while parked** (CP7 FR-2.3). Reads windowed (§8.1).

### 4.3 Use cases
- **4.3.1** As a developer, when I stop, then I see each carrier's arena fill and
  a list of live heap allocations with owning bindings.
- **4.3.2** As a robotics developer profiling a control loop, when I step across
  an iteration, then I can confirm **no new heap allocations** occurred.
- **4.3.3** As a developer, when I select a heap allocation, then the Variables
  view highlights its owner and Viewer A opens at its address.
- **4.3.4** As a developer, when allocations accumulate without dropping across
  stops, then the growing live-set is visible (leak signal).
- **4.3.5** As a developer on an older binary lacking the accessor, then the
  viewer degrades to "unavailable," never errors.

---

## 5. Viewer D — Flash / RAM footprint

### 5.1 How it works — two halves over one budget
1. **Static (build-time, no run).** Parse section sizes from the build artifact /
   a `cajeta build` size report (§7.5): **flash** = `.text` + `.rodata` +
   `.data` initializers (init images live in flash); **static RAM** = `.data` +
   `.bss` (+ reserved stack + reserved heap/arena from the link config). Render
   usage bars + a per-section / per-module / per-symbol **treemap** to locate
   bloat, and compare against the target tier's flash/RAM envelope (`Embedded.md`
   per-tier targets), warning when over.
2. **Runtime overlay (during a session).** Overlay *actual* RAM on the static
   budget: fixed `.data`/`.bss`, plus **live stack high-water** (a per-carrier
   probe, §7.6) and the **heap/arena live-set** (reused from Viewer C, §4 / §7.2).
   This turns "will it fit" into "is it fitting," against real high-water.

### 5.2 Why it is both static and runtime
Flash and static RAM (`.data`/`.bss`) are fixed at link time. But total RAM — the
quantity that must stay under the device ceiling — is `static + stack high-water
+ heap/arena high-water`, and the last two are **runtime**. So D is a build-time
budget whose **decisive half is runtime**, reusing Viewer C's heap/arena
measurement plus a stack high-water probe. It is not a pure static report — which
is why it belongs in the **debug-view Memory panel** beside Viewer C, not in the
build-tool widget, even though its static half can render before any run.

### 5.3 Requirements
Lives in the debug-view Memory panel (Placement, §1.2). Static section breakdown
(flash + static RAM) with per-section/module/symbol drill-down (treemap); per-tier
budget bars + over-budget warning identifying the largest contributors; the static
view is available in that panel **without an active session** (reads the last
build output); during a session, an actual-RAM overlay (static + stack high-water
+ heap/arena from §4) against the device limit, refreshing on stop.

### 5.4 Use cases
- **5.4.1** As an embedded developer, post-build, then I see flash/static-RAM
  usage vs the target tier budget with a treemap of the biggest contributors.
- **5.4.2** As a developer, when the binary is over a tier's flash/RAM, then a
  warning names the largest contributors.
- **5.4.3** As a developer in a live session, then I see actual RAM (static +
  stack high-water + heap/arena) vs the device limit, updating as I step.
- **5.4.4** As a developer, then I can confirm worst-case stack + heap high-water
  + static ≤ device RAM (the fit-on-device check).
- **5.4.5** As a developer with no size report or no session, then the viewer
  shows static-only or "unavailable," never an error.

---

## 6. Shared — navigation, live update & editing

### 6.1 Requirements
- "Show in memory" from any address-bearing variable (Variables/Watches/Viewer C)
  opens Viewer A at its `memoryReference`.
- All viewers refresh on stop/step/frame-change and clear at session end (CP7-3/4
  live-update discipline). D's static half persists without a session.
- Edits (Viewer A bytes, Viewer B RW fields) issue `writeMemory` and reflect on
  resume; failures surface non-fatally.

### 6.2 Use cases
- **6.2.1** As a developer, when I edit a byte in Viewer A and resume, then the
  program runs with the new value.
- **6.2.2** As a developer, when I change the selected frame, then address-derived
  views retarget to the new frame's binding.
- **6.2.3** As a developer, when the session ends, then live viewers clear (D
  reverts to its static budget).

---

## 7. Cross-repo prerequisites

- **7.1 DAP `readMemory` / `writeMemory`** in `cajeta dap` (`src/cajeta/dap/`):
  honor `memoryReference`+offset+count; report unreadable ranges; host process
  memory now, remote/AOT target later. (A, B.)
- **7.2 Heap/arena introspection accessors** (`cajeta_runtime.c` + `__cajeta_dbg_*`):
  arena `{base,cursor,mark,reserved}` per carrier + a drop-chain/live-set walk
  `{addr,type,size,owner,drop-order,alloc-class}`; safe at a safepoint. (C, D-runtime.)
- **7.3 Peripheral descriptor ingestion** — attach a CMSIS-SVD / Cajeta
  register-map per target into §3's model. (B.)
- **7.4 Remote/AOT enabler** (shared with embedded-targets spec) — on-device
  memory access needs AOT+DWARF+remote-attach; host works without it.
- **7.5 Build-tool size report** — `cajeta build` emits machine-readable
  per-section/module/symbol sizes (or the plugin reads the artifact section
  table). (D-static.)
- **7.6 Stack high-water probe** — runtime/host accessor for per-carrier stack
  high-water. (D-runtime.)

---

## 8. Non-functional requirements

- **8.1 Performance.** Reads windowed (only visible bytes/allocations fetched);
  refresh on stop incremental; large heaps/treemaps paginate/virtualize; nothing
  blocks the EDT; D's static parse is fast and cached per build output.
- **8.2 Cross-platform / cross-target.** Host (Linux/macOS/Windows) plus on-device
  tiers via the remote enabler; D-static works for any built target.
- **8.3 Graceful degradation.** Missing `readMemory`/`writeMemory`, missing
  accessors, no descriptor, host-with-no-MMIO, no size report, no session, or
  remote unavailability degrade to empty/disabled/"unavailable"/static-only with
  a note — never an error dialog, hang, or misleading value (CP7 FR-8.3).
- **8.4 Safety.** Introspection accessors never take a lock the parked carrier
  holds; writes are access-aware (B) and confirmable; nothing executed.
- **8.5 Testability.** HexModel, RegisterDecoder, arena/heap model, and the
  section/budget model are plain-JVM cores with direct unit tests
  (bytes→grid, word→fields, runtime snapshot→map, sections→budget); platform
  panels + wire path integration-tested against the real `cajeta dap` (skipping
  when absent).
- **8.6 Single source of truth.** Viewer C's alloc-class coloring reuses the CP7
  facet palette; D's runtime heap/arena reuses Viewer C; "Show in memory" uses
  one `memoryReference` derivation.
