# Spec: One ownership-title classifier for every consumer position (`ownership-title-classifier`)

**draft** — filed 2026-09-07, for review with the developer. Measured basis:
the classifier survey of the working tree on 2026-09-07 (six copies of the
same shape classifier, a dozen disagreements; table in §3), the conditional
(`c ? a : b`) defect family it produced
(`ternary-local-double-free`: a local, a `#=` store, a `#` return, a call
argument and a re-assignment each had to be fixed separately, in
`27bd6c78` and the commit that follows), and probes
`tmp/probe-emit/src/probe/{T,U,V,W,X,Y}.cajeta`.

## 1. Definition

### 1.1 Purpose

Every position that consumes an expression's value has to answer one question
before it can act: **what title does this value carry?** A borrow that someone
else frees, a fresh owned value, a title moved out of a local, a title that
rides a callee's runtime flag, or a mode decided at runtime by a formal's
transfer-word bit or a slot's own-bit.

Today that question is answered by an if/else chain over AST node types
written separately at each consumer: the local declaration, the `#=` store
wrapper (`MoveExpression`), the field / element / local assignment paths in
`BinaryOpExpression`, the return statement's static checks and its flag
composition, the call-argument transfer word and its after-call reclaim, the
constructor-argument twins in `CreatorRest`, and the conditional's own per-arm
helper. Six copies. They disagree on a dozen shapes (§3), and every new shape
has to be added to each one — the conditional was added to five of them over
two days, one measured double-free at a time.

This spec defines **one classifier** with two entry points — a static shape
verdict usable before any IR exists, and a runtime flag materialised once
after the value's codegen — and migrates every consumer onto it, so a shape is
classified in one place, a disagreement is a decision recorded once, and a
consumer that needs a static answer and a runtime answer for the same node
reads both from the same source.

### 1.2 Scope

- **In:** the `TitleShape` static classification and its diagnostic label;
  the runtime flag materialisation (drop-entry flag, return-flag TLS,
  transfer-word bit, conditional phi, slot own-bit / element take); the
  consumer policy table (which answer each consumer role acts on); cast
  peeling and conditional recursion as classifier concerns; arena eligibility
  as a classifier concern; the migration of every site listed in §3; a
  measured-equivalence method (IR corpus diff per migrated site); an
  exhaustiveness test over `Expression` subclasses.
- **Out:** the ownership *model* itself (what lends, what transfers — CLAUDE.md
  §1–3 stands); the runtime protocols (the return-flag TLS, the transfer word,
  field own-bits, drop entries, the String dual-role protocol 3.4.3); the
  diagnostic codes and their user-facing text (the labels move, the codes do
  not); new syntax.

### 1.3 Non-goals

- No behaviour change on any site whose current answer agrees with the table
  in §3. Where a site's answer is measured wrong (a ⚠ cell), the change is a
  decision recorded in §5, with the probe that shows the leak or double-free.
- No attempt to make ownership statically decidable where it is runtime state
  (§7.2 discipline: a check fires only on a proven borrow, never on "cannot
  prove").

### 1.4 Constraints

- Behaviour is **measured, never reasoned**: each migrated site ships with an
  IR diff over a fixed corpus (the stdlib, the ownership test programs, the
  probe set) showing identical output modulo folded constants, plus the
  ownership suites green, plus the fleet building.
- One site per commit, in dependency order (§4), so a regression bisects to a
  site.
- Every runtime read of the return-flag TLS happens **immediately** after the
  call whose flag it is (the next call clobbers it). The classifier reads it;
  consumers never do.

## 2. The classifier

### 2.1 Static shape (`TitleShape classify(expr, module)`)

Peels reference casts, then classifies the expression by provenance:

| Shape | Recognised as | Static answer |
|---|---|---|
| `TextLiteral` (String / text block) | `Literal` | Borrow (static alias) |
| `Identifier` | `LocalRead{hasEntry, isParam, isTransferredParam, arena}` | Borrow of the named slot; policy may treat a transferred formal as a frame title (the Exception-ctor idiom) |
| `this` | `This` | Borrow |
| `DotExpression` | `FieldRead` | Borrow (interior view) |
| `ArrayIndexExpression` | `ElementRead` | Borrow (interior view) |
| `NewExpression` / `AggregateInitializer` | `Fresh{heap, stack, shared}` | heap → Owned; stack → StackBound; shared → Borrow |
| `ArrayLiteral` | `Fresh{heap, stack, arena}` | heap → Owned; else StackBound |
| String `+` concat | `Concat{arena}` | arena → Borrow (frame arena); else Owned |
| `MoveExpression` | `Move{inner}` | Owned when the inner is a static owner; Runtime(DE / WORD / SLOT) otherwise |
| `MethodCallExpression` | `CallResult{stance: owned / plain / view / unknown}` | owned → Owned; plain → Runtime(TLS); view (`^`) → Borrow; unknown → Unknown |
| `CallExpression` (closure) | `ClosureCall` | Runtime(TLS) |
| `BooleanSwitchExpression` | `Conditional{arms}` | the join of the arms: equal static answers fold; otherwise Runtime(PHI) |
| anything else | `Unknown` | Unknown |

`Unknown` is an answer: a consumer with a diagnostic passes, a consumer with a
default keeps the default it has today, and an audit switch counts the
population so it can be sized (as `CAJETA_AUDIT_RETURN_TITLES` sizes returns).

The callee stance for a `CallResult` comes from a shallow, order-independent
resolution (the existing `resolveArgCalleeShallow`), never from codegen state
on the node — the owned-bind check's silence in `cajeta-llm` (§6) is what
reading `getResolvedMethod()` before codegen costs.

### 2.2 Runtime flag (`llvm::Value* titleFlag(expr)`)

For a `Runtime` answer, one i64 read, emitted right after the value's codegen
and cached on the node for the rest of that codegen:

| Source | Read |
|---|---|
| DE | `__cajeta_drop_entry_flag(entry)` of the named local |
| WORD | the enclosing function's transfer-word bit for the formal |
| TLS | `__cajeta_return_flag_get()` after the call |
| PHI | the conditional's `tern_title` phi (constant when both arms agree) |
| SLOT | the field's own-bit / `__cajeta_tail_elem_take_flag` |

Static answers materialise as `ConstantInt` 0 / 1 so every consumer can fold
its branch. The existing per-node fields (`MoveExpression::runtimeTitleFlag`,
`BooleanSwitchExpression::runtimeTitleFlag`, `MethodCallExpression`'s stashed
arg / flagged-title values) become this one cache.

### 2.3 Consumer policy

Consumers do not re-classify. Each declares its **role** and asks the policy
table for the action:

| Role | Borrow | Owned | Runtime | Unknown |
|---|---|---|---|---|
| bind (`T x = e`) | no entry | armed entry | flagged entry | today's default (owned) |
| `#=` store (String) | resolve a copy | move the wrapper | branch resolve / move | resolve |
| `#=` store (class field / slot) | own-bit 0 | own-bit 1 | own-bit = flag | own-bit 0 |
| `=` re-assign of a moved-out local | no re-arm | re-arm | re-arm with flag | no re-arm |
| return under `#T` | **error** OWNED_RETURN_OF_BORROW | flag 1 | flag + TITLE_MISS contract | flag from method mode |
| return under plain `T` | flag 0 | **error** FRESH_RETURN_NEEDS_TRANSFER | ride the flag | flag 0 |
| argument to plain formal | word bit 0 | class: word bit 1; String: reclaim after the call | class: word bit = flag; String: guarded reclaim | word bit 0 |
| argument to `#T` formal | **error** TRANSFER_REQUIRED (provable shapes) | pass | pass | pass |
| conditional arm | 0 | 1 | flag | 0 |

The labels for the two error rows ("a field read", "a literal", "a bare local
or formal (no `#`)", "a call returning a borrow") come from the shape, in one
table.

## 3. The sites, and where they disagree today

Rows are AST shapes, columns are the consumer sites (survey names). **B** =
borrow, **O** = owned, **RT:x** = runtime flag from x, **E** = compile error,
**—** = shape not recognised (site default). ⚠ marks a cell that disagrees
with the row's majority and is a decision in §5.

| Shape | LVD init (A8) | ternary arm (B1) | class field own-bit (C4) | tail elem (C5) | local re-arm (C8) | `#` return static (D4) | return flag (D10) | call word (E5) | `#T` check (E6) | reclaim (E7) |
|---|---|---|---|---|---|---|---|---|---|---|
| `#x` move | owned → RT | O / RT | O + RT | O / RT | move + RT | pass | O / RT | RT | skip | skip |
| `heap X()` | O | O | O | O | O | pass | n/a | O | skip | n/a |
| `stack X()` | stack-drop | B | B | B | B | E | n/a | B | skip | n/a |
| aggregate `heap` | O (default) | O | **B** ⚠ | **B** ⚠ | **B** ⚠ | pass | n/a | **B** ⚠ | — | n/a |
| String concat | O (default) | **O, no arena check** ⚠ | B ⚠ | B | B | pass | n/a | B (3.4.3) | — | O (drop) |
| call, `#R` callee | O + §4.6 error | RT:TLS | O | O | O | pass | RT:TLS | O | skip | O if String |
| call, plain class callee | RT:TLS | RT:TLS | **B** ⚠ | **`bindingTakesTitle`** ⚠ | **B** ⚠ | pass | RT:TLS | RT:TLS | E ⚠ | RT (receiver) |
| closure call | RT:TLS | RT:TLS | **B** ⚠ | **B** ⚠ | **B** ⚠ | pass | RT:TLS | RT:TLS | — | — |
| literal | B | B | B | B | B | E | n/a | B | — | — |
| identifier | B | B | B (O if `#` formal) | B | B | E | RT:DE (formals) | RT:DE / WORD | **B, no error** ⚠ | — |
| field read | B | B | B | B | B | E | RT:SLOT via `#` | — | E | — |
| element read | B | B | B | B | B | E | RT:SLOT via `#` | — | E | — |
| cast | **lvalue (A5)** ⚠ | **not peeled** ⚠ | B | B | B | **not peeled** ⚠ | peeled | not peeled | not peeled | not peeled |
| conditional | RT:PHI | RT:PHI | RT:PHI (fixed 2026-09-07) | **B** ⚠ | RT:PHI | per arm | RT:PHI | RT:PHI (class) | per arm | RT:PHI (String) |
| array literal `heap` | O (default) | **B** ⚠ | **B** ⚠ | B | B | pass | n/a | O | — | — |

Sites not in the table that carry their own two-shape rule and migrate too:
the interface-slot kind (LVD A6, BinaryOp C1: only `#x` counts as owned, so a
`heap X()` into an interface local is recorded borrowed), the closure-drop
rule (A11), the legacy `producesOwnedString` (A10, name-keyed), the
`operator[]=` word (C2: only `#x` contributes a bit), the String-array sidecar
and field-array element stores (C6, C7), and the four `CreatorRest` twins of
the call-argument sites.

## 4. Use cases

- **4.1** When a consumer needs a verdict before any IR exists (a return or
  argument diagnostic), the static shape answers Borrow / Owned / Runtime /
  StackBound / Unknown with a label, and the consumer never inspects node
  types itself.
- **4.2** When the answer is Runtime, the consumer asks for the flag once; the
  read is emitted immediately after the value's codegen and reused by every
  consumer of that node in the same codegen.
- **4.3** When both arms of a conditional classify the same way statically,
  the flag is a constant and the consumer's branch folds; no runtime test is
  emitted.
- **4.4** When a shape is Unknown, a diagnostic consumer passes and a
  defaulting consumer keeps today's default; the audit switch counts the
  site so the Unknown population can be measured and shrunk.
- **4.5** When a reference cast wraps the expression, it is peeled before
  classification, at every consumer.
- **4.6** When a new `Expression` subclass is added, an exhaustiveness test
  fails until the classifier names it.
- **4.7** When a migrated site's new answer differs from its old one, the IR
  corpus diff shows it, and §5 records the decision with the probe.
- **4.8** When a `CallResult` stance is needed before codegen, it comes from
  the shallow resolver, so the same source compiles to the same verdict
  regardless of entry point or file order (§6).
- **4.9** When the migration is complete, the per-node flag fields, the
  conditional's arm helper, and the per-site shape chains are gone; grepping
  `dynamic_pointer_cast<MoveExpression>` in a consumer finds only the
  classifier.

## 5. Decisions on the disagreements (proposed — to be reviewed)

- **5.1 Aggregate `heap` initialiser** stored to a class field, a tail slot or
  a re-armed local is Owned (today: borrow → leak).
- **5.2 Closure-call result** in the same three sites is Runtime(TLS) (today:
  borrow → a `#`-returning lambda's result leaks).
- **5.3 Plain-callee call result** in the field own-bit and re-arm sites is
  Runtime(TLS), the measured §2.1 rule (today: borrow); the tail-element site
  drops `bindingTakesTitle` (unknown ⇒ owned) for the same Runtime read.
- **5.4 String concat** in the conditional arm classifier gains the arena
  check; a frame-arena concat is Borrow.
- **5.5 Cast peeling** everywhere; the `initFromLvalue` retain rule keeps a
  cast as an lvalue only when the peeled inner is one.
- **5.6 Array literal `heap`** is Owned in the conditional arm and field
  own-bit sites (today: borrow → leak).
- **5.7 Interface-slot kind** and the `operator[]=` word compose from the
  classifier (today: `#x` only).
- **5.8 Identifier argument to a `#T` formal** — open. The `#T` check passes a
  bare local (the callee assumes a title the local's entry still holds); the
  `#` return check rejects the same shape. Options: (a) keep permissive
  (documented, measured 2026-08-09 for *plain* formals only), (b) reject when
  the local has an active entry (the return rule). Recommendation: (b), with
  the arena-local carve-out the check already has.
- **5.9 `bindingTakesTitle` (unknown ⇒ true)** survives only as the Unknown
  default of the bind role; no other consumer reads it.

## 6. Related finding: the owned-bind check was never order-dependent

`CAJETA_ERROR_OWNED_RESULT_NEEDS_TRANSFER` was recorded on 2026-09-06 as
"order-dependent" because a hand-built `--emit=cja` of `cajeta-llm` rejected
`SafetensorsFile.cajeta:199,214,221,228` while `run-tests.sh` built the same
sources clean. Measured 2026-09-07: the check fires on a first-generation
generic call in isolation (probe `Y.cajeta`), and `run-tests.sh` line 115
exports `CAJETA_OWNED_BIND=warn` (line 116: `CAJETA_CAPTURED_BORROW=warn`),
which demotes both checks to notes that the script's `>/dev/null` discards.
The same build with the notes captured reports **451 owned-bind and 9
captured-borrow sites** across `cajeta-llm` (main and test sources). The
check is deterministic; the harness is in migration mode. Two consequences
for this spec: the fleet oracle in §7 runs with both switches at `error`, and
the `cajeta-llm` migration (a `cajeta-llm` plan item, not this spec's)
precedes the first consumer migration here so the oracle is meaningful.

A second, unrelated defect surfaced from the same probes: two sibling files
each declaring a nested `static class Cell` bind the bare name `Cell` inside
one outer to the other file's class (`W.cajeta` + `X.cajeta` in one root:
`W.sinkCell(c ? heap Cell(8) : r.a)` fails `NO_MATCHING_OVERLOAD` with
candidate `sinkCell(probe.W.Cell)`; `W` alone compiles). That is the
nested-class short-name binding family, recorded there, not here.

## 7. Acceptance

- Every ownership suite green; the 18 `TernaryOwnershipTests` and every
  existing ownership diagnostic test unchanged.
- IR corpus diff per migrated site: identical modulo folded constants, or a
  §5 decision.
- Fleet builds (`cajeta-llm`, `cajeta-jinja`, `cajeta-logging`,
  `cajeta-unit`, `cajeta-xgboost`, `cajeta-ml`, `cabra`) compile; `cajeta-llm`
  cpu suite green.
- Emitted-instruction count on the corpus before and after, reported (the
  consolidation must not grow the output; the folded constants should shrink
  it).
- The exhaustiveness test exists and passes.
