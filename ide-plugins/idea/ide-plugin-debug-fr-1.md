# IDE Plugin Debugger — Functional Requirements 1: Ownership, Allocation & Lifetime Visualization

Status: living requirements doc. First in the `ide-plugin-debug-fr-N` series.

This document is the **what** (capabilities, acceptance criteria, requirement
IDs) for making memory ownership, allocation class, and lifetime *visible* in
the Cajeta debugger. The **how** (build order, checkpoints) lives in `Plan.md`;
the DAP wire contract lives in `cajeta-docs/Debugging.md`; the language
semantics being visualized live in `cajeta-docs/stdlib/MemoryModel.md` and
`cajeta-docs/stdlib/FieldOwnership.md`.

Companion docs:
- `cajeta-docs/stdlib/MemoryModel.md` — source of truth for allocation classes,
  ownership, transfer (`#`), borrow, and drop. This doc visualizes those
  concepts; it does not redefine them.
- `cajeta-docs/stdlib/FieldOwnership.md` — field-level ownership rules.
- `cajeta-docs/Debugging.md` — the DAP adapter contract the plugin speaks (see
  its “ownership annotations in the variables panel” v1 goal, § Goals).
- `Plan.md` / `README.md` (this directory) — overall plugin plan and status.

---

## 1. Motivation & scope

### 1.1 Motivation

Cajeta's defining feature is explicit, statically-checked memory ownership over
three allocation classes (stack / heap / shared). Today the debugger renders a
variable as `name : type = value` with no indication of **who owns what**,
**where it lives**, or **how long it lives**. A developer stepping through code
cannot see — without re-reading the source and re-deriving the borrow rules —
whether a binding owns its referent or merely borrows it, whether a value is on
the stack, the heap, or shared memory, or whether it has been moved out and is
about to (or has ceased to) drop.

The goal is a **strong, at-a-glance indication of allocation and lifetime**,
surfaced in both the debugger **Variables view** and **on the page** (inline in
the editor) as debugging is ongoing.

### 1.2 In scope (confirmed first-cut scope)

- Capturing ownership / allocation-class / lifetime metadata in debug info and
  propagating it through runtime → DAP → plugin.
- **All four distinction axes** in the first cut: owner vs borrow; stack vs heap
  vs shared; and lifetime / drop state (live / moved-out / about-to-drop).
- **All three surfaces** in the first cut: the Variables view, editor **gutter
  icons**, and **full inline decorations** that update live as the user steps.
- **Icon + color + bold** as the visual encoding (see FR-5/FR-7).
- User configuration of the encoding and accessibility fallbacks.

### 1.3 Out of scope (this document)

- Changing the language's memory model or borrow checker. This doc *visualizes*
  existing semantics; any semantic gap discovered is filed against
  `cajeta-docs/stdlib/MemoryModel.md`, not fixed here.
- Heap-graph / object-graph visualization (who-points-at-whom across the whole
  heap). Tracked separately if pursued.
- Allocation *timelines* / historical replay. This doc covers the live state at
  each stop, not a recorded history (that overlaps time-travel debugging, a
  v1.5 item in `Debugging.md`).

### 1.4 Relationship to in-flight debugger work

Sequencing is decided: this lands as a **new checkpoint group (CP7)** after the
in-flight CP6f work (conditional breakpoints done; fibers view CP6f-2 and
exception breakpoints CP6f-3 to finish first). The metadata-foundation
checkpoints (CP7-1a..c) touch different layers than CP6f and may be authored in
parallel, but the feature is scheduled after CP6f per the decision in § 8.

---

## 2. Glossary (as visualized)

Terms are normative in `MemoryModel.md`; summarized here so the requirements are
self-contained.

- **Allocation class** — **stack** (default; frame-local value, `StackField`),
  **heap** (`new T(...)` / class instances & array references, `HeapField`;
  sole-pointer owner frees on drop), **shared** (CajetaXPU `shared`; lifetime is
  the enclosing kernel launch).
- **Owner** — the single binding responsible for dropping a heap/shared value.
- **Borrow** — a non-owning, aliasing/read reference (`Scope.liveBorrows`); must
  not outlive the owner.
- **Transferred (moved-out)** — ownership handed off via `#`
  (`FormalParameter.transferred`, move tracking); the source binding is
  moved-out and must not be read.
- **Live / moved-out / about-to-drop** — lifetime states: live from initializer
  until scope exit or move-out; moved-out bindings don't drop; owners drop in
  reverse declaration order at scope exit.

---

## 3. Metadata capture & propagation (foundation)

The visualization is only as good as the metadata behind it. Audit finding: the
compiler already distinguishes `StackField` vs `HeapField` at the `emitDbgLocal`
site, tracks `#`-transfer via `FormalParameter.transferred`, and tracks borrows
via `Scope.liveBorrows`; the drop chain exists in the runtime. So owner/borrow
and stack/heap are highly feasible; **shared** and **drop-state** need the most
new wiring.

**FR-1 — Ownership & allocation metadata in debug info.**

- **FR-1.1** The debug-info codegen path (`emitDbgLocal` and its callers
  `LocalVariableDeclaration` / `Method`) MUST capture, per local/parameter, an
  **allocation class** ∈ {stack, heap, shared, unknown}. Stack vs heap derives
  from the `StackField`/`HeapField` choice already made at the call site; shared
  from the XPU `shared` marker. Not-statically-determinable ⇒ **unknown** (never
  silently defaulted to stack).
- **FR-1.2** The path MUST capture an **ownership role** ∈ {owner, borrow,
  transferred-out, unknown} per binding, from `FormalParameter.transferred`,
  heap-owning locals (`new`, drop-entry presence on the `Field`), and
  `Scope.liveBorrows`.
- **FR-1.3** Opt-in with debug info only; **zero cost** to non-debug builds,
  consistent with the existing safepoint/frame-chain gating (`emitGuard`).
- **FR-1.4** Graceful degradation: an undeterminable facet is tagged **unknown**
  and rendered neutrally (FR-5.5), never miscategorized.

**FR-2 — Runtime carries per-binding metadata.**

- **FR-2.1** The runtime frame chain (`cajeta_dbg_frame` / `__cajeta_dbg_local`)
  MUST carry allocation class + ownership role alongside `{name, type, addr}`,
  readable via stateless host accessors (same pattern as the CP5 frame-chain and
  CP6f-2a fiber accessors). This extends the `__cajeta_dbg_local` signature
  (or adds a sibling) to take the two enums.
- **FR-2.2** Lifetime state (live / moved-out / about-to-drop) MUST be derivable
  at a stop. Moved-out draws on the move/`#` tracking; about-to-drop draws on
  the runtime drop chain (the next owners to drop at scope exit). Where a sub-
  facet can't be computed in the first cut it is marked **unknown** (FR-4.4),
  but lifetime is in-scope for the first cut per § 1.2.
- **FR-2.3** Metadata accessors MUST be safe to call from the debugger thread
  while parked (no lock the carrier holds at a safepoint), per the established
  stop-the-world inspection model.

**FR-3 — DAP wire carries presentation metadata.**

- **FR-3.1** The `variables` response per variable MUST carry allocation class,
  ownership role, and lifetime state. Use DAP-standard `presentationHint`
  (`kind`, `attributes`, `visibility`) where it maps; carry Cajeta-specific
  facets in a namespaced `cajeta` sub-object rather than overloading standard
  fields.
- **FR-3.2** Backward-compatible: a variable with no metadata MUST still render
  with a neutral presentation; the plugin MUST NOT require the fields present.
- **FR-3.3** The additions MUST be documented in `cajeta-docs/Debugging.md`
  (§ Cajeta-specific DAP extensions).

---

## 4. Semantic visualization requirements

What distinctions MUST be visible, independent of exact glyph/color (FR-5/FR-7).
All four axes below are first-cut must-haves (§ 1.2).

- **FR-4.1 (Ownership)** A binding that **owns** its referent MUST be visually
  distinct from one that **borrows** it. Highest-priority distinction.
- **FR-4.2 (Allocation class)** **Stack**, **heap**, and **shared** bindings
  MUST be mutually visually distinct.
- **FR-4.3 (Transferred / moved-out)** A **moved-out** binding MUST be visibly
  marked as consumed and MUST NOT present a misleading live value (reading it is
  a language error).
- **FR-4.4 (About-to-drop / lifetime)** A binding scheduled to drop at the
  current scope exit (owner on the live drop chain) SHOULD be distinguishable;
  combined with FR-4.3 this gives the live/moved-out/about-to-drop lifetime
  signal.
- **FR-4.5 (Unknown)** Any **unknown** facet MUST render neutrally and be
  distinguishable from a known value (unknown alloc class ≠ “stack”).
- **FR-4.6 (Composability)** Ownership, allocation class, and lifetime are
  **orthogonal** axes and MUST be representable simultaneously (e.g. a
  heap-owning-about-to-drop binding vs a heap-borrow).

---

## 5. Variables-view presentation

**FR-5 — Variables tool-window rendering.**

- **FR-5.1** Each variable MUST encode ownership role, allocation class, and
  lifetime via the IntelliJ `XValue` presentation API (icon, value-text
  attributes, node label/tag).
- **FR-5.2** Default encoding (confirmed): **icon + color + bold**. Ownership by
  **icon** (e.g. filled = owner, outline = borrow) with **owners bold**;
  allocation class by **color/tint** (stack / heap / shared each distinct);
  lifetime via an overlay/treatment (moved-out struck/greyed per FR-5.4).
- **FR-5.3** A concise textual affordance (tooltip and/or appended tag such as
  `[heap, owned]` / `[moved]`) MUST be available so meaning is discoverable
  without memorizing the legend and survives screenshots, colorblind themes, and
  high-contrast modes. (Color is never the sole carrier — FR-7.4.)
- **FR-5.4** Moved-out bindings MUST render with a distinct “consumed” treatment
  (struck-through / greyed + `[moved]`) and MUST NOT show a misleading live
  value.
- **FR-5.5** Unknown facets render neutral (no icon / plain weight / default
  color) with a tooltip noting the facet is unavailable.
- **FR-5.6** Presentation MUST be consistent for nested children once object
  expansion exists, and MUST NOT regress current leaf rendering when metadata is
  absent.

---

## 6. In-editor ("on the page") presentation — first-cut: full

Confirmed: the first cut includes **gutter icons** and **full inline
decorations**, not just the Variables view.

**FR-6 — Editor decorations during a live debug session.**

- **FR-6.1 (Inline decorations)** While parked, in-scope local bindings on/near
  the current line MUST be decorated inline (inlay hints and/or text attributes)
  indicating ownership role, allocation class, and lifetime — using the same
  icon+color+bold language as the Variables view.
- **FR-6.2 (Gutter icons)** The editor gutter MUST show, per relevant line, an
  icon summarizing the allocation/ownership of the binding(s) declared or
  active there.
- **FR-6.3 (Live update)** Decorations MUST update on step / resume / frame
  change and clear when the session ends.
- **FR-6.4 (Unobtrusive)** Decorations MUST be individually toggleable
  (FR-7.3), MUST NOT disruptively shift layout, and MUST NOT fight existing
  inspections/highlighting.
- **FR-6.5 (Single source of truth)** Editor decorations MUST source the same
  DAP variable facets as the Variables view (no parallel derivation), so the two
  surfaces never disagree.

---

## 6b. Drop / destructor breakpoints

A lifetime breakpoint complement to the visualization: stop the program **when
an object is dropped**. Audit finding (runtime, verified): class drops route
through `__cajeta_class_virtual_drop(instance)` → it loads the instance's vtable
and calls the vtable's `drop_fn` slot (`CAJETA_VTABLE_DROP_FN_OFFSET`) → the
compiler-synthesized per-class drop wrapper → the user's `~Class()` body. (Array
/ view / closure drops have their own dispatchers; the drop *chain* fires
entries via `e->drop_fn(e->obj)` at scope exit.) The cleanest place to honor a
“break on drop of T” without a real destructor is the safepoint the synthesized
drop wrapper already can carry — i.e. the wrapper for class T contains a
statement whose loc maps to T's destructor (or its synthetic site), so a normal
line breakpoint there parks via the existing mechanism. Setting a breakpoint on
a hand-written `~Class()` already works the moment destructor bodies emit
safepoints; the only new work is making the *synthesized* wrapper carry a
breakable safepoint for classes without an explicit destructor.

**FR-9 — Drop / destructor breakpoints.**

- **FR-9.1 (Drop-on-type)** The user MUST be able to set a breakpoint that fires
  when an instance of a chosen class is dropped — naturally surfaced as a
  breakpoint **on the class's destructor**. Setting it on the destructor source
  line is the primary UX; a “break on drop of T” affordance MAY also be offered
  for classes with no explicit `~T()`.
- **FR-9.2 (Mechanism)** Preferred: the destructor / synthesized drop wrapper
  for T emits a breakable safepoint (loc in T's `~T()` or a synthetic drop site),
  so a drop breakpoint is an ordinary armed line breakpoint that parks via the
  existing DebugController rendezvous — no new stop path. The `stopped` event
  MAY carry a drop hint and the dropped object's identity/type. Verify against
  the real runtime which drop routes (class virtual drop, array, view, closure)
  carry a safepoint; routes that don't are documented as not-yet-breakable.
  Zero cost when no such breakpoint is armed / debug info off (FR-1.3).
- **FR-9.3 (Inspectable at the stop)** At a drop stop the object being dropped
  MUST be inspectable (its address/type, and its fields once object expansion
  exists) and the current frame/stack MUST be the drop site, so the user sees
  *what* is being dropped and *where from* (scope exit vs explicit `delete`).
- **FR-9.4 (Dynamic / conditional)** Drop breakpoints SHOULD compose with the
  CP6f-1 condition grammar (e.g. break on drop only when a field compares a
  certain way), and MUST be toggleable/removable live like line breakpoints.
- **FR-9.5 (DAP surface)** Exposed over DAP either as a normal source breakpoint
  on the destructor line (preferred — no protocol extension) or, where “break on
  drop of T” without a visible destructor is wanted, as a data/function-style
  breakpoint documented in `cajeta-docs/Debugging.md`.

This is its own checkpoint, **CP7-6**, dependent on the CP7-1b runtime hook
plumbing (it reuses the same DebugController rendezvous) and naturally adjacent
to the CP6f-3 exception-breakpoint work (both add a non-line stop reason). It is
independent of the visualization checkpoints CP7-2..5 and could even precede
them.

---

## 7. Checkpoint mapping (provisional, CP7 group)

These span multiple checkpoints; each is independently testable (TDD against the
real `cajeta dap` binary where it crosses the wire) and gated for review, per
the project's checkpoint-by-checkpoint principle. Hashes filled in as they land.

| Checkpoint | Scope | Requirements | Layer |
|---|---|---|---|
| **CP7-1a** ✅ `baeed84` | Memory-facets classification core (AllocClass/OwnershipRole enums + `classifyField` + names; pure, 14 tests) | FR-1.1–FR-1.4 | compiler / `dbg::MemoryFacets` |
| **CP7-1b** ✅ | Runtime frame-chain carries facets + host accessors (`__cajeta_dbg_local` +2 bytes; `_alloc`/`_ownership` accessors; both call sites classify from Field/FormalParameter; 6 read-back tests) | FR-2.1, FR-2.3 | runtime + host |
| **CP7-1c** ✅ | Lifetime/drop-state derivation at a stop (`LifetimeState` enum + `deriveLifetime`; frame chain carries owner drop-entry ptr; `_drop_active` accessor; `walkFrames` fills `DbgVar.{alloc,ownership,lifetime}`; 9 tests incl. live drop-chain) | FR-2.2 | runtime + host |
| **CP7-1d** ✅ | DAP `variables` carries facets — `variableJson` adds namespaced `cajeta:{alloc,ownership,lifetime}` tags + `presentationHint.readOnly` for moved-out; `Debugging.md` updated; 3 builder tests | FR-3.1–FR-3.3 | DAP server |
| **CP7-2** ✅ | Variables-view rendering — plugin `MemoryFacets` core (parse + `present`→`FacetPresentation`) + `CajetaValue` wiring: ownership→icon, alloc→value color, moved-out→error styling + read-only, facet tag appended; 11 pure tests. (bold weight + native hover-tooltip deferred — info carried by icon+color+inline tag+error) | FR-4.*, FR-5.* | plugin |
| **CP7-3** ✅ | Editor gutter icons — pure `summarizeGutter`→`GutterSummary` (significance precedence + per-binding tooltip) + `FacetGutterManager`/`CajetaFacetGutterRenderer` line-highlighter on the stopped line, fed by the same `loadVariables` as the Variables view (FR-6.5); cleared on step/resume/session-end (FR-6.3); 5 pure tests | FR-6.2–FR-6.5 | plugin |
| **CP7-4** | Full inline decorations, live update | FR-6.1, FR-6.3–FR-6.5 | plugin |
| **CP7-5** | Configuration, legend, accessibility | FR-7.* | plugin |
| **CP7-6** | Drop / destructor breakpoints | FR-9.* | runtime + DAP + plugin |

Dependencies: CP7-2 depends on CP7-1a–d; CP7-1c depends on CP7-1b; CP7-3 and
CP7-4 depend on CP7-2 (shared metadata, FR-6.5). CP7-1a–d may be authored in
parallel with CP6f (different layers) but the group is scheduled after CP6f.

---

## 8. Acceptance, configuration & non-functional

**FR-7 — Configuration & accessibility.**

- **FR-7.1** The encoding (icon set, colors, bold) SHOULD be configurable in
  plugin settings, with the FR-5.2 default out of the box.
- **FR-7.2** A **legend** MUST be discoverable (settings page and/or tool-window
  affordance) explaining each glyph/color/weight.
- **FR-7.3** Variables-view encoding, gutter icons, and inline decorations MUST
  each be independently toggleable.
- **FR-7.4** Default palette MUST be colorblind-safe; meaning MUST never be
  conveyed by color alone (FR-5.3 textual affordance satisfies this).

**FR-8 — Non-functional.**

- **FR-8.1** Zero cost when debug info is off (FR-1.3); negligible added cost at
  a stop (read from the already-captured frame snapshot).
- **FR-8.2** Cross-platform (Windows/macOS/Linux), consistent with the existing
  debugger.
- **FR-8.3** Graceful degradation end-to-end: missing/unknown metadata ⇒ neutral
  rendering, never an error or misleading indication (FR-1.4, FR-3.2, FR-4.5,
  FR-5.5).
- **FR-8.4** Behavioral cores stay plain JVM (no `com.intellij.*`) so they
  unit-test without a platform fixture; platform classes stay thin — consistent
  with the existing debugger checkpoints.

**Acceptance.** Each checkpoint ships with tests. Wire-crossing requirements
(FR-1/2/3) are verified against the real `cajeta dap` binary with a sample
program exercising all three allocation classes, both ownership roles, and a
moved-out binding. Plugin rendering (FR-5/FR-6) is verified with the platform
test fixture; the plain-core mapping is unit-tested directly.

---

## 9. Decisions (confirmed)

- **Sequencing** — New **CP7 group, after CP6f** (fibers + exception bp finish
  first). CP7-1a–d may be authored in parallel since they touch other layers.
- **Distinctions** — All four axes are first-cut must-haves: owner/borrow,
  stack/heap/shared, and lifetime/drop-state (FR-4.1–FR-4.4).
- **Surfaces** — First cut is maximal: Variables view + gutter icons + full
  inline decorations (FR-5, FR-6).
- **Encoding** — Icon + color + bold (FR-5.2), always backed by the textual
  affordance (FR-5.3).
