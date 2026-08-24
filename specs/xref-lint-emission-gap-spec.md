# xref-lint-emission-gap — spec

**Status:** draft — defect, found 2026-08-24 during the ide-symbol-index live pass.

## 1 Definition

### 1.1 The defect
The cross-reference index the IDE consumes carries **no call edges and no field
references**. Every export the plugin makes is produced by `--lint`, and the
records for calls and field accesses are produced on a path `--lint` does not
run. The index therefore resolves **types only**.

Ctrl-click on a method or a field has never worked through the IDE, in any
project. It was reported as a single failing line — `c.value()` in
`ClassesDemo.cajeta:18` — and is not specific to that line, that file, or that
project.

### 1.2 Measured
`samples/tour`, correct source root (`samples/tour/src/main/cajeta`), compiler at
`10854f15`:

| relation | whole-root `--lint --emit-xref` | Unit 2 acceptance (2.3.4) |
|---|---|---|
| declarations | 6435 | — |
| inheritance | 554 | — |
| overrides | 383 | — |
| references, `kind:"type"` | 5938 | 3062 |
| references, `kind:"field"` | **0** | 3757 |
| calls | **0** | 2087 |

`tour/Counter.cajeta` linted alone emits its 6 declarations and 1 inheritance
edge and **nothing else** — no reference for `this.v`, which it reads or writes
on four lines.

The target of the reported navigation exists in the index and is correct:
`tour.Counter.value`, `overloadKey tour.Counter::value(pointer)`,
`tour/Counter.cajeta:29`. Only the edge to it is missing.

### 1.3 Cause — calls (established)
`xref::noteResolvedCall` records nothing unless a `CallSiteScope` is open
(`XrefIndex.cpp:571`: `if (!gCaptureEnabled || calleeKey.empty() ||
gOpenSites.empty()) return;`). A `CallSiteScope` is opened in exactly two
places, both in codegen:

- `MethodCallExpression::generateCode` (`MethodCallExpression.cpp:1604`)
- `CreatorRest` (`CreatorRest.cpp:42`)

`--lint` stops before codegen — its own documented contract ("run the semantic
passes over one file and stop before codegen"). So `gOpenSites` is always empty
when `CajetaClass::resolveMethod` resolves a callee, and every call edge is
dropped at the guard.

The recording machinery is otherwise intact and correct: `resolveMethod` is the
funnel every callee resolution passes through, `drainCalls` merges duplicates,
and `pruneDanglingEdges` would keep the edges (it indexes both `fqn` and
`overloadKey`, and the callee keys match declared overload keys).

### 1.4 Cause — field references (established 2026-08-24; SAME cause)
`DotExpression::recordFieldXref` is called from `DotExpression::resolveTypes`,
which looked like a path lint runs. It is not: **body `resolveTypes` is invoked
only from `Method::generateCode`.** Signature-position type resolution happens
under lint; nothing inside a method body is resolved at all.

So both relations have one cause — **resolution of method bodies is entangled
with codegen, and lint stops before codegen.** This is one fix, not two.

### 1.4.1 It is documented, and the documentation contradicts itself
`Compiler::lint` (`Compiler.cpp:1991`):

> What lint-mode capture yields, honestly: declarations, inheritance, enums,
> template members, and parse-time type references. NOT calls or field
> accesses — body resolveTypes runs only inside `Method::generateCode`, the
> codegen phase lint deliberately stops before. Per-edit, the buffer's own
> declarations are what must stay fresh; **edges refresh on build or whole-root
> export.**

`Compiler::lintRoot` (`Compiler.cpp:2238`) — the whole-root export:

> this parses everything and stops where lint stops ... **Call and field-access
> edges need body resolution (codegen) and come from a real build's
> `--emit-xref` instead.**

The first promises the whole-root export refreshes edges. The second, which IS
the whole-root export, says it does not and points at a real build. Measured,
the second is correct.

The IDE runs only these two paths. So the promise the per-edit path makes is
kept by nothing the product ever executes. `xref::captureStaticReceivers` — a
separate AST walk added to both paths to recover static-receiver TYPE
references — is the existing evidence that this gap was understood and patched
narrowly rather than closed.

### 1.5 Why this survived as accepted
`ide-symbol-index` plan item 2.3.4 records measured counts — 2087 call edges,
3757 field references — with per-edge position accounting. Those numbers are
real, but they were taken from an export the IDE does not use. The acceptance
never exercised the `--lint` path, so a relation that is empty on every path the
product consumes read as delivered.

### 1.6 Scope
In scope: making the lint-mode export carry call and field-reference records
that are equal in content to the ones the non-lint path produces.

Out of scope: new relation kinds, changes to the xref schema, changes to how the
plugin ingests or resolves records, and IDE-side navigation behaviour. Those all
work — they have nothing to resolve.

### 1.7 Non-goals
- **1.7.1** Running codegen during lint. Lint's value is that it stops early;
  paying codegen per keystroke would defeat the lint server.
- **1.7.2** Shipping a partial fix for calls only. Both relations are Ctrl-click
  targets a developer cannot distinguish by eye.

## 2 The export must be the same on both paths

### 2.1 Requirements
- **2.1.1** A lint-mode export and a full-build export of the same source root
  must agree on `calls` and on `references` of kind `field` — same edges, same
  positions.
- **2.1.2** An edge must not be recorded from a position that lint cannot know.
  A missing edge costs a navigation; a misattributed one sends "who calls this"
  to the wrong line (ide-symbol-index spec §1.3, unchanged here).
- **2.1.3** Recording must stay behind the xref capture flag, so a normal build
  and a normal lint are untouched.
- **2.1.4** Per-file lint (`--lint <file> --emit-xref`) must carry the same
  relations as whole-root lint, since per-edit shards are how the index stays
  current between rebuilds.

### 2.2 Use cases
- **2.2.1** When a method is called on a receiver whose type resolves, a lint
  export carries a call edge from the call site to the callee's overload key.
- **2.2.2** When a field is read or written through a receiver, a lint export
  carries a field reference at the identifier's own position.
- **2.2.3** When a call is made through a generic, the edge names the template
  member the developer wrote, not the monomorphized instantiation.
- **2.2.4** When a callee cannot be resolved, no edge is emitted, and lint does
  not fail because of it.
- **2.2.5** When the same call site is resolved more than once across passes,
  the export carries one edge, not several.
- **2.2.6** When a file is linted alone with a source root, its call and field
  records are the subset of the whole-root export belonging to that file.

## 3 Navigation, end to end

### 3.1 Requirements
- **3.1.1** Ctrl-click on a method name at a call site opens that method's
  declaration.
- **3.1.2** Ctrl-click on a field name at an access site opens that field's
  declaration.
- **3.1.3** An inherited member resolves to the ancestor that declares it, not
  to the receiver's class.

### 3.2 Use cases
- **3.2.1** When Ctrl-clicking `value` in `c.value()` at
  `ClassesDemo.cajeta:18`, `Counter.cajeta:29` opens. This is the reported case
  and is the acceptance trigger.
- **3.2.2** When Ctrl-clicking `v` in `this.v` inside `Counter.bump()`,
  `Counter.cajeta:14` opens.
- **3.2.3** When Ctrl-clicking a method inherited from `DemoClass`, the
  declaration in `DemoClass.cajeta` opens.
- **3.2.4** When the index has no edge for a symbol, nothing happens — never a
  jump to a wrong target.

## 4 The acceptance gap itself

### 4.1 Requirements
- **4.1.1** Every relation the IDE consumes must have an acceptance check that
  runs it through the path the IDE uses. A count taken from another path does
  not count.
- **4.1.2** A relation that is empty across a whole sample project must fail
  that check rather than pass silently.

### 4.2 Use cases
- **4.2.1** When a relation the plugin reads is empty for `samples/tour`, the
  check fails and names the relation.
- **4.2.2** When the lint and full-build exports of `samples/tour` disagree on
  any relation, the check fails and names the difference.

## 5 Impact on ide-symbol-index

### 5.1 Requirements
- **5.1.1** `ide-symbol-index` acceptance items 7.3.1 and 7.3.2 cannot pass
  while this defect stands: "Ctrl-click works across files" is true for types
  and false for every method and field.
- **5.1.2** `ide-features` Units 2–5 (find usages, call hierarchy, rename, safe
  delete) all read the call and reference relations. They must not be started
  against an index that carries neither.

## 6 Open questions

- **6.1** ~~What suppresses field references under lint?~~ **RESOLVED
  2026-08-24 — same cause as calls** (§1.4): body `resolveTypes` runs only
  inside `Method::generateCode`. One fix, not two.
- **6.2** **How does body resolution become available to lint?** This is the
  load-bearing decision and the spec does not settle it. Three shapes:
  - **(a) Decouple body `resolveTypes` from `Method::generateCode`** and run it
    as a lint pass. Both relations then fall out of the existing recording code
    with no change to `CallSiteScope`, `noteResolvedCall`, or `recordFieldXref`.
    Truest to the design — resolution is a semantic pass and belongs before
    codegen — and the largest change. The risk is cost per keystroke and any
    behaviour that silently depends on resolution happening under an active
    module.
  - **(b) A dedicated AST walk over parsed bodies**, in the shape of the
    existing `xref::captureStaticReceivers`, recording calls and field accesses
    without full body resolution. Smallest and precedented, but it must resolve
    overloads to name a callee's overload key, which is most of what body
    resolution does — so it risks reimplementing resolution badly, and a WRONG
    edge is explicitly worse than a missing one (ide-symbol-index §1.3).
  - **(c) Feed the IDE a build export** for the whole-root index and keep lint
    for per-edit declaration freshness. No compiler change, but it makes the
    index require a successful build, and per-edit shards would then carry
    strictly less than the shards they overwrite.
  **Recommendation: (a)**, measured for cost against the lint-server budget
  before committing. (b) is the fallback if (a) proves too expensive per edit;
  (c) is a last resort because a partial per-edit shard clobbering a complete
  whole-root shard is the failure mode the ingest code already warns about.
- **6.3** **What is the per-edit cost budget?** `lint-server` recorded warm lint
  at ~3.5 s median (Debug, 2026-08-18). Body resolution's added cost must be
  measured against that number before (a) is chosen, and the answer decides
  whether per-edit shards carry edges or only the whole-root export does.
- **6.4** **Do the two comments' promises get reconciled?** Whichever shape
  wins, both docstrings in `Compiler.cpp` state the contract and currently
  contradict each other (§1.4.1). They are part of the deliverable.
- **6.5** **Does this affect published `.cja` archives?** Archives carry
  `meta/reflection-keep.v1` and class sources; whether any carries xref records
  produced by a build path is unverified.
