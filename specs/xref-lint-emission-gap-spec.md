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

### 1.4 Cause — field references (not yet established)
`DotExpression::recordFieldXref` is called from `DotExpression::resolveTypes`,
which `--lint` **does** run — so 1.3's explanation does not cover it, and the
two must not be assumed to share a fix. What is measured is only that the
records do not appear. See 6.1.

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

- **6.1** **What suppresses field references under lint?** `recordFieldXref`
  sits on a path lint runs, so the calls explanation does not apply. Candidates:
  the receiver's type is unresolved during lint so `findProp` is never reached;
  `idLine`/`getSourceFile()` are unset on the node; or the records are produced
  and dropped later. **This must be measured before the plan is written** — it
  determines whether this is one fix or two.
- **6.2** **Where should the call site come from?** Two shapes:
  (a) open the `CallSiteScope` in a pass lint runs (e.g. `MethodCallExpression`'s
  resolve phase) as well as in codegen, de-duplicating in `drainCalls` as it
  already does for the resolve/codegen double-resolution; or
  (b) drop the ambient-scope mechanism and pass the position explicitly from the
  AST node into `resolveMethod`.
  (a) is smaller and reuses a merge path that already exists and is tested;
  (b) removes a thread-local ambient dependency that is the reason this class of
  bug is possible at all. **Recommendation: (a) for the fix, and record (b) as a
  follow-up**, so the defect is closed without a refactor of every resolution
  site in the same change.
- **6.3** **Is a full-build export still wanted?** If lint and build must agree
  (2.1.1), the cheapest guarantee is one recording path used by both. Confirm no
  consumer needs build-only records.
- **6.4** **Does this affect published `.cja` archives?** Archives carry
  `meta/reflection-keep.v1` and class sources; whether any carries xref records
  produced by a build path is unverified.
