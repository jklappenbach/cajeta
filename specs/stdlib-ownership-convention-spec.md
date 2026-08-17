# stdlib-ownership-convention — spec

Authored 2026-08-14, from evidence gathered implementing `cajeta-llama`
Units 11–13. Four ownership bugs in one unit, one root cause, and only
one of the four caught at compile time.

**Revised 2026-08-14, after implementation contradicted it three times.**
The original drew its requirements from one unit's experience and stated
a simplified language model as settled fact; every subsequent error
descended from that. This revision (a) replaces the model statement with
the measured one (§1.2), (b) grounds the requirements in an external
codebase that hit the same categories independently (§1.4), (c) adds the
two requirements the original lacked — operation-bound view lifetimes
and compiler fidelity to `#` (§3.6, §3.7) — and (d) removes an
acceptance criterion that asserted a count nobody had verified (§6.2).
Claims below are marked *measured* where a test establishes them.

## 1. Definition

### 1.1 Purpose

Fix a **single, uniform convention** for who owns data crossing a
library API boundary in the cajeta standard library and first-party
libraries, and add the two compiler checks that make the convention
enforceable rather than aspirational.

### 1.2 The problem, from evidence

**The language model, stated correctly.** This spec's first draft opened
"plain `=` lends; `#` transfers title", called that model settled in
§1.4, and reasoned from it. Three conclusions in this document were
wrong as a direct result, each caught by a gate or a measurement rather
than by review. The simplification is the single largest source of error
in the work so far, so it is replaced here rather than patched
downstream.

Ownership in cajeta is **runtime-conditional on both sides of a call**.
What differs by position is *who decides*:

| Position | Decided by | Carried in | Spelling |
|---|---|---|---|
| name → name | the **spelling** | statically | `=` lends, `#=` transfers |
| argument | the **caller** | the transfer word | `f(x)` lends, `f(#x)` transfers |
| result | the **callee** | the return-flag TLS | plain `T` may still carry a title |
| slot store | the **source's mode** | per-slot bit, via `#=` | a lend stays a lend |

Two consequences that the simplification hides, both measured:

- **A plain (non-`#`) return is not statically a borrow.** A plain-return
  wrapper that tail-calls a `#` method rides the inner flag through
  (`SignatureAbiTests.tailCallThroughPlainReturnKeepsTitle`), and
  `Stream.fold<R>` does it through its callback's `#R` — genuinely
  runtime-variable, since the callback is a parameter. `=` from such a
  call is therefore not a lend: the local's drop entry is armed from the
  arriving flag (`LocalVariableDeclaration.cpp:251`).
- **`#x` on a borrow does not transfer.** It forwards the mode it was
  handed. The lender keeps title and frees on drop, so a receiver that
  outlives the lender reads reused memory (§3.1, measured).

Only the **name → name** row matches the original one-line summary. The
compiler proves that row within a function body —
`CAJETA_ERROR_DANGLING_LEND` is precise and helpful. What it cannot see
is an ownership contract that lives in another library's
*documentation*, and today stdlib APIs disagree about that contract with
no signal at the call site.

Observed while writing ~2,600 lines against the stdlib in one unit:

| API | Stance | Consequence |
|---|---|---|
| `ProtobufCursor.readBytes(slot)` | returns owned `#int8[]` | `#` correct |
| `JsonValue.asString()` | returns owned `#String` | correct |
| `JsonObject.keyAt(j)` | returns a **borrow** of internal storage | `heap String(#kb, kl)` gave the String title to memory the object still frees — keys read back as garbage |
| `JsonArray.add(#JsonValue)` | **forces** transfer | correct for a sink |
| `ArrayList.add(T)` | **caller's choice**, `#=` internally | the intended model |
| `JsonValue.setString(String)` | **conditionally** borrows | every interpreter value dangled |

The last row is the sharpest edge. `setString(String)` stores the
String's buffer as a borrow *unless* the string is SSO-backed or a
non-zero-offset slice, in which case it silently copies instead
(`JsonValue.cajeta:221-229`). Identical-looking call sites are therefore
safe or catastrophic depending on a representation detail the caller
cannot see. No convention survives an API whose ownership depends on the
runtime shape of its argument.

Three of the four bugs produced **no diagnostic at all**: one surfaced
as garbage bytes in unrelated output, one as `SIGABRT` inside
`JsonValue::asString()` several frames from the mistake, one as a
segfault in a later test. The compile-time/runtime asymmetry is the
cost being paid.

### 1.3 Scope

- The `cajeta.*` standard library, and first-party `dev.cajeta.*`
  libraries.
- Two compiler checks (§4).
- A documented migration for APIs that violate the convention today.

### 1.4 External evidence

The §1.2 table is one unit's experience with one library, which is thin
ground for a convention. `llama.cpp` — a mature, widely-deployed C/C++
inference engine, and `cajeta-llama`'s reference implementation —
independently hit every category this spec names, and its workarounds
are the strongest available evidence that these are real requirements
rather than local taste.

- **1.4.1 A sink marked only by prose.**
  `llama_sampler_chain_add(chain, smpl)` carries the comment
  *"important: takes ownership of the sampler object and will free it
  when llama_sampler_free is called"* (`include/llama.h:1290`). Two
  identically-typed pointer parameters, one captured and one not,
  disambiguated by a comment that had to shout **important:** because
  the signature could not carry the fact. This is §2.4 and §7.4, found
  in production C rather than argued from first principles.

- **1.4.2 The same type with opposite ownership, decided by
  provenance.** `llama_batch_init(...)` allocates buffers the caller
  must release with `llama_batch_free`, while
  `llama_batch_get_one(tokens, n)` wraps a **caller-owned** array and
  must not be freed (`include/llama.h:911-928`). One struct type, two
  constructors, opposite obligations, distinguishable only by which
  function produced the value. This is §2.6's failure mode — ownership
  contingent on something the type does not express — in a widely-copied
  API.

- **1.4.3 Views whose lifetime is bound to an OPERATION, not a scope.**
  `llama_get_logits_ith(ctx, i)` returns a raw `float *` into context
  memory that the next decode overwrites, and
  `llama_adapter_lora_init` documents *"The adapter is valid as long as
  the associated model is not freed"* (`include/llama.h:643`). Neither
  bound is a lexical scope. Cajeta's model cannot currently express
  either — see §3.6, a requirement this spec previously lacked.

- **1.4.4 Data ownership split from metadata ownership.** `ggml`'s
  `no_alloc` context flag (`ggml/include/ggml.h:662`) builds tensors
  that describe memory they do not own — the mmap'd weight file being
  the motivating case. A descriptor pointing at foreign memory that must
  never be freed is the borrow-return case at its most consequential,
  and it is why §2.2 is a default rather than a preference.

Read together these say the convention is not cajeta-specific
bookkeeping: it is the set of distinctions any system passing buffers
across an API boundary is forced to make, and that C makes only in
comments.

### 1.5 Non-goals

- Changing the language's ownership model. The runtime-conditional model
  described in §1.2 is the language as it stands, and this spec does not
  propose altering it — only describing it accurately and making its
  contracts legible at the call site.
- Lifetime *inference* or a borrow checker in the Rust sense.
- Reworking collections. `ArrayList`'s model is the target, not the
  problem.

## 2. The convention

Ownership is decided by **who may legitimately outlive the caller's
scope**. Only sinks may. Every other API is producing a value or
lending a view, and each has one correct default.

- **2.1** When an API materializes a new value — a conversion, a decode,
  a format, a copy (`asString`, `toBytes`, `readBytes`, `encode`) — it
  returns **owned** (`#T`). A conversion-shaped call never hands back a
  window into another object's interior.
- **2.2** When an API exposes interior state for reading (`keyAt`,
  `asBytes`, `get(i)`) it returns a **view**: spelled plain `T`, and its
  body returns *only* interior reads, so the return flag is always
  borrow. It never takes title, and the caller copies if the value must
  outlive the container.

  The second clause is not pedantry. Plain `T` alone does not mean
  borrow (§1.2) — it means the callee decides at run time. A view is the
  case where the callee always decides "borrow", and that is a property
  of the BODY, not of the signature. It is also exactly what a compiler
  can check (§4.1), which is why the convention is stated this way
  rather than as "plain `T` means borrow".
- **2.3** When an API is a **sink** — a collection or other container
  whose stated job is to hold values — it takes a plain `T` parameter
  and stores with `#=`, so `add(v)` lends and `add(#v)` transfers, with
  the mode recorded per slot. This is `ArrayList`'s existing model and
  it is the only genre where the developer chooses.
- **2.4** When a non-sink API stores a parameter beyond the call, the
  parameter is spelled `#T`. A plain parameter that is quietly captured
  is invisible at the call site and is the `setString` failure.
- **2.5** When an API could either copy or alias, it **copies**, and the
  aliasing variant is a separately-named method. The safe spelling is
  the unmarked one; the sharp spelling is explicit. `setString` /
  `setStringOwned` is exactly backwards today and inverts.
- **2.6** When ownership would depend on a runtime property of the
  argument that **neither side can see or state** — its representation,
  its length, its provenance — the API is **non-conforming**.

  This is narrower than the "ownership is a static contract or it is not
  a contract" first drafted here, which the language itself falsifies:
  the transfer word and the return flag make ownership runtime-carried
  by design (§1.2), and `ArrayList.add` is conforming precisely because
  the caller chooses at run time. The distinction that matters is
  whether the decision is **carried and attributable**:

  | | Decided at | Visible to the caller | Verdict |
  |---|---|---|---|
  | `f(#x)` / `f(x)` | run time | yes — the caller wrote it | conforming |
  | `#=` slot store | run time | yes — mode came from the source | conforming |
  | `setString(s)` | run time | **no** — SSO vs slice, internal | non-conforming |
  | `llama_batch` (§1.4.2) | construction | **no** — depends which ctor ran | non-conforming |

  Runtime-conditional ownership is fine. Ownership conditioned on a
  property the caller cannot observe, and that no party recorded, is
  not.
- **2.7** When a borrow-returning accessor exists, its documentation
  states the lifetime bound in one line, and its name does not suggest
  materialization (`viewOf`, `bytesAt`, `...At` read as views; `to...`,
  `as...`, `read...` read as producers).
- **2.8 The three return stances.** *(Decided 2026-08-15. The count it
  was decided against: **12 of 164** plain-return, class-returning
  methods compiled across five library builds can carry a title —
  7.3%, of which only two escape a title through a signature that
  implies otherwise. Measured by the compiler's own `returnTitleFlag`,
  not classified from source; inventory in
  `docs/stdlib/return-title-audit.md`.)*

  | spelling | meaning | flag | enforced |
  |---|---|---|---|
  | `T f()` | **transparent carry** — hands out whatever ownership state the FRAME holds | runtime | nothing to enforce: it claims nothing, so nothing it says can be false |
  | `#T f()` | **forced transfer** | const 1 | callee must establish a title at every return; the receiving lvalue must be `#=` or it is an error |
  | `^T f()` | **forced borrow** | const 0 | body restricted to borrow sources: `this`, interior reads, other `^T` results |

  Plain `T` is the default *because it is the common case, and the
  common case should carry no syntax*. It also needs no migration: a
  view's frame holds no title, so `T` carries a borrow and the caller
  receives a borrow — the 152 borrow-returning methods in the
  measurement stay exactly as written. `^T` is opt-in, for APIs that
  want the guarantee checked rather than described.

  **"Carries" means the mode the FRAME holds, not the mode a source
  slot recorded.** This is what keeps `peek`-shaped accessors safe with
  no annotation: `Heap.peek`'s `return this.data[i]` is an interior
  read the frame does not own, so it carries a borrow, while
  `Heap.pop` reaches the slot's recorded mode deliberately through the
  body (`T out #= this.data[i]; ... return #= out;`, the shape
  `HashMap.cajeta:384-387` already uses). Same signature, different
  bodies, each transparent about what it actually holds. Today those
  two are both spelled `T` with opposite contracts and the difference
  lives only in `collection/skills/collection-Heap.md` — §7.4's failure
  mode, which is what 2.8 exists to end.

  **A plain parameter is never a legal `^T` return source.** It can
  still arrive owned (`f(#x)`), so returning it as a forced borrow
  leaves the frame dropping it at return and the caller holding a
  dangle. `Optional.orElse` is therefore `T`, not `^T`: interior state
  on one path, the caller's own fallback on the other.

  `^` was chosen by elimination, measured against the grammar: `&` is
  the intersection-type separator (`CajetaParser.g4:143`) and `~` is
  the destructor sigil (`public ~Channel()`), so both read wrong in
  type position. `^` is infix xor only and free as a prefix.

## 3. Use cases

- **3.1** When a caller writes `#x` where `x` is a borrow, the compiler
  rejects it — surrendering title one does not hold is the mirror of
  `DANGLING_LEND`, and it is what turned `keyAt` into corruption.

  *Measured 2026-08-14 (`OwnershipArrayCanaryTests`).* `#x` on a borrow
  does not transfer the title; it forwards the mode it was handed. The
  lender keeps ownership and frees on drop, so when the receiver
  OUTLIVES the lender the receiver reads reused memory — an array
  payload came back as `-83968` instead of `8247`, and a class payload
  came back holding the churn allocation's `98,98`. Both kinds, not
  only arrays.

  This makes the check load-bearing rather than stylistic, and it
  corrects a weaker reading reached earlier from a `liveCount` probe
  that measured the same shape as "balanced". That probe kept everything
  in ONE scope, so nothing outlived its lender and nothing could dangle.
  A balanced count is consistent with correct ownership AND with a
  transfer that never happened; only data survival separates them.
  Counting answers the double-free question, not the ownership one.
- **3.2** When a library stores a plain (borrowed) parameter into a
  field, an element, or a container, the compiler rejects it and names
  the `#T` spelling as the fix.
- **3.3** When a developer reads a stdlib signature, the ownership of
  every argument and result is legible from the signature alone,
  without consulting prose.
- **3.4** When an existing API violates §2, it is migrated with its old
  spelling kept as a deprecated alias for one release, so downstream
  code fails loudly rather than silently changing meaning.
- **3.5** When a value is copied for safety in the common path, the cost
  is bounded and documented, and a zero-copy alternative exists where
  measurement justifies it — the tokenizer's `currentBytes()` zero-copy
  key matching (`cajeta-llama` spec 13.7) is the model: deliberate,
  named, and opt-in.
- **3.6** When a view's validity is bound to an **operation** rather
  than a scope — invalidated by the next mutating call, or living only
  as long as some other object — the API states that bound, and the
  convention does not pretend a scope-based rule covers it.

  *Requirement added from §1.4.3, and previously missing.* Every §2.2
  view in this spec is implicitly scope-bounded: copy it if it must
  outlive the container. `llama.cpp` shows two bounds that are not
  scopes at all — logits invalidated by the next decode, and an adapter
  valid only while its model lives. Cajeta cannot express either today,
  and this spec should not imply otherwise. What it requires now is
  honesty at the boundary: such an API documents the invalidating
  operation by name. Whether the language should carry it is deferred
  (§7.5) rather than quietly assumed away.
- **3.7** When a signature spells `#`, the compiler honours it —
  everywhere the spelling is legal.

  *Requirement added from a defect, not from theory.* Interface methods
  declared `#T` carried `returnsOwnership == false`: the interface
  member path builds its `Method` by hand and read only the return TYPE
  out of `typeTypeOrVoid`, dropping the `#` beside it, so callers of
  every `#`-returning interface method were told the result was a
  borrow. Four `@Native` `String` methods had the mirror defect from the
  other direction, declaring plain returns while transferring.
  A convention resting on signatures is worth exactly as much as the
  compiler's fidelity to them, so that fidelity is a requirement in its
  own right and not an implementation detail. See §7.4.

## 4. Compiler enforcement

Convention without enforcement decays; these are the two checks that
would have caught three of this unit's four bugs at the line rather
than as corruption.

- **4.1** Reject `#x` where `x` holds a borrow returned by a call the
  callee's body PROVES is an interior view.

  *Corrected 2026-08-14 by the routine gate, which rejected the first
  version of this item.* As first written — and as first implemented —
  this said the callee's declared return spelling is static truth and a
  plain (non-`#`) return hands back a borrow. **That is false, and the
  correction matters more than the original claim.**

  A plain return in cajeta carries a RUNTIME title flag, exactly as a
  plain formal carries its caller's title through the transfer word. A
  plain-return wrapper that tail-calls a `#`-returning method rides the
  inner flag through — `SignatureAbiTests.tailCallThroughPlainReturn-
  KeepsTitle` pins it (`static Cell viaPlain() { return D.fresh(); }`),
  and `Stream.fold<R>` does the same through its callback's `#R`. So
  ownership is conditional on BOTH sides of a call, and the symmetry is
  the design, not an accident:

  | Position | Spelled | Carries | `#` means |
  |---|---|---|---|
  | formal | `T p` | runtime, via the transfer word | forward the arrived mode |
  | result | `T f()` | runtime, via the return flag | forward the arrived mode |

  The compiler comment this unit set out to close — "a call-result local
  stays unchecked until the `#?` runtime-owner ABI can carry its role" —
  was therefore not a TODO. It was recording that the case is not
  statically decidable. Reading it as an unclosed gap is what produced
  the unsound first implementation.

  What IS decidable is the narrower question the check actually needs:
  does the callee's body prove the result is a window into the
  receiver's interior — every return a `this.field` read (or an index
  into one), and at least one? That shape covers every real instance
  (`JsonObject.keyAt`, `Optional.get`) and excludes every ride-through.
  Anything unproven is ALLOWED, per §7.2 — the check must never block
  valid code.

  Two further corrections from the same implementation:

  - The diagnostic is the EXISTING `CAJETA_ERROR_MOVE_OF_BORROW`, not a
    new code; a second code for one defect would fragment the
    diagnostics.
  - **Plain parameters are NOT covered, and must not be** — the formal
    half of the table above. Rejecting `#p` statically would outlaw
    every mode-forwarding wrapper. The existing check excludes formals
    deliberately.
  - Already-moved locals are already diagnosed by the existing
    `demoteToBorrow` tracking; no new work (verified by test 2.1.5,
    which passed against the unmodified compiler).

- **4.2** `CAJETA_ERROR_CAPTURED_BORROW_PARAM` — reject storing a plain
  parameter into a field, array element, or container beyond the call,
  naming `#T` as the fix. Sinks (§2.3) opt out by spelling the store
  `#=` on a parameter the signature already marks as caller's-choice.
- **4.3** Both checks report the *declaration* that created the borrow
  alongside the offending use, so the diagnostic names both ends.
- **4.4** Neither check fires on conforming existing code; the stdlib
  builds clean after §5's migration.
- **4.5** *(from §2.8)* **A `#T` return is checked at the CALLEE.** Every
  return in a `#`-declared method must establish a title; a plain return
  of a value the frame does not own is an error. Today the static mode
  asserts a title unchallenged, and `ParallelDriver.reduceParallelChain`
  is the live instance: declared `#T`, both returns plain
  (`return accY;` `:492`, `return acc;` `:541`), so an empty stream
  hands the caller a forged title over the seed it lent. The existing
  TITLE_MISS guard does not reach this — it only covers `return #x`
  with a runtime flag.
- **4.6** *(from §2.8)* **A `#T` result must be received with `#=`.**
  Binding it with plain `=` is an error naming the transfer. This is
  what makes an acquisition visible at the call site, which is the whole
  point of the return-side redesign: the reader sees where title moves
  without opening the callee. Concretely, the two lines this separates:

  ```
  int8[] w = s.toBytes();     // `#int8[]` — w is yours to free
  int8[] w = s.root();        // plain     — the String still owns it
  ```

  Identical to a reader, opposite facts about who frees `w`.

  **Enforced first at the DECLARATION position** (`T x = f()`), which is
  the population §5.5's harvest measured — ~148 sites, 38 of them
  `String.toBytes`. The ASSIGNMENT position (`x = f()`, `this.f = f()`)
  is equally covered by the rule as stated and is *counted but not yet
  rejected*, pending its own measurement. That split is deliberate and
  is §5.5's rule applied to itself: a requirement shipped against an
  unmeasured population is what left Unit 3's acceptance open.

  Note what this rule is NOT claimed to be. Whether a plain `=` bind
  also leaks is a separate, measurable question, answered by probe
  rather than by reading the codegen — the inference route is how §4.8
  came to be filed on a leak that did not exist.
- **4.7** *(from §2.8)* **A `^T` body is restricted at compile time** to
  `this`, interior reads, and other `^T` results — never an owned local,
  a fresh allocation, a `#T` result, or a parameter. In exchange
  §4.1 becomes decidable from the signature rather than from
  per-local provenance: `#` applied to a `^T` result is an error on the
  spot. That is the `keyAt` bug this spec opened with, caught at the
  line that makes the mistake.

  **IMPLEMENTED 2026-08-17 (plan 8.2.8).** The sigil is `^` in
  return-type position (`(REFERENCE | CARET)? typeType`; infix xor is
  untouched — prefix position is unreachable in expressions). Five
  diagnostics, each pinned by `test/expression/BorrowReturnTests.cpp`:

  | shape | error |
  |---|---|
  | `x #= viewCall()` / `#viewCall()` (casts peeled) | `CAJETA_ERROR_TRANSFER_OF_VIEW_RESULT` |
  | `#T f() { return viewCall(); }` — direct, cast-wrapped, or parked in a local | `CAJETA_ERROR_OWNED_RETURN_OF_BORROW` |
  | `^T` body returns owned local / fresh alloc / `stack` / `#T` result / parameter / another object's field / `^` call on a non-`this`-rooted receiver | `CAJETA_ERROR_VIEW_RETURN_NOT_INTERIOR` |
  | `^int32` (any value-semantics return), interfaces included; template `^V` at a primitive DEMOTES instead | `CAJETA_ERROR_VIEW_RETURN_OF_VALUE` |
  | `^` on a static method (no receiver, no lifetime to ride) | `CAJETA_ERROR_VIEW_RETURN_STATIC` |
  | `^` on `operator#[]` (the title-EXTRACTING operator) | `CAJETA_ERROR_VIEW_ON_EXTRACTING_OPERATOR` |
  | method reference to a `^` method; `(P) -> ^R` function types | `CAJETA_ERROR_VIEW_REFERENCE_UNSUPPORTED` |
  | implementor's `^` stance differs from the interface's | `CAJETA_ERROR_VIEW_STANCE_MISMATCH` |

  The permitted body shapes are decided by `Method::exprIsInteriorRead` —
  the SAME oracle §4.5's provenance machinery trusts — so `this.f`,
  `this.f[i]`, bare `this`, `null`, and `^`-delegation on a `this`-rooted
  receiver chain all pass, and nothing else does.

  `#=` receipt of a `^` result is refused along with `#x`, deliberately:
  it would record a borrow and be memory-safe, but §4.6 spent this unit
  making `#=` mean "a title moved here" — a `#=` that records a borrow
  reintroduces the ambiguity the rule removed. A `^` result binds with
  plain `=`.

  **The original "emits no return-flag write at all" clause is REVISED:
  the callee-side write stays, by measurement of a hole rather than by
  preference.** A `^` method invoked through a method reference or a
  function-typed local loses its static stance at the call site, so
  that caller still reads the transfer word; a callee that skipped the
  write would hand it a stale flag. Until `^` has a stance story for
  references, the callee writes constant 0 (one TLS store) and direct
  call sites may fold to constant 0 as an optimization when `^` gains
  users. What IS statically true: the flag is a compile-time constant
  0, which is the §4.1 decidability this section exists for.
- **4.8** ~~*(prerequisite for §2.8's `T`)* Transparent carry must
  actually be transparent — a returned local's flag is forwarded only
  for `ParameterField`, so `T f() { T x = heap ...; return x; }`
  leaks.~~

  **RETRACTED 2026-08-15 — measured, and there is no leak.** The shape
  never reaches codegen: `CAJETA_ERROR_FRESH_RETURN_NEEDS_TRANSFER`
  (`Statement.cpp` ~2108) rejects it, with a diagnostic that already
  prescribes the fix this spec would have — *"returns owned local 'x'
  but its return type isn't marked `#` … Fix: change the return type to
  `#T`."* The claim came from reading the forwarding site and inferring
  a consequence instead of compiling the program, which is the failure
  mode CLAUDE.md §5 exists to prevent.

  **What is true, and it is a different thing.** The guard keys on the
  local having a DROP ENTRY, not on the local holding a title — and
  ownership is runtime state, so it cannot key on the latter. It
  therefore also rejects a local holding only a borrow
  (`T f(Bank b) { Cell m = b.get(); return m; }`, pinned by
  `SignatureAbiTests.plainReturnOfBorrowedLocalIsAlsoRejected`). The
  cost is narrow: a plain-return method cannot launder a borrow through
  a NAMED local. Both other spellings work — `return b.get()` rides the
  callee's flag, and `return #= m` ships the runtime bit (explicitly
  exempted from the guard at `Statement.cpp` ~2077, because the
  caller's `#=` receipt registers a drop only when the bit is 1).

  **Consequence for §2.8, and it sharpens the model rather than
  weakening it:** transparent carry is carried by the RETURN EXPRESSION
  — a formal, a call result, or `#= local`. A statically-owned local
  must say `#T`, which is the convention §2.1 asks for anyway. The
  language already enforces at the return the thing §2.8 asks the
  reader to see at the signature.

  **Consequence for the return-statement transfer word:** `return #= x`
  is the ONLY spelling that carries a local's runtime mode out of a
  plain-return method. Deleting it (plan 8.2.1) would leave that
  pattern unexpressible, so the word stays.

## 5. Migration

- **5.1** `JsonValue.setString(String)` copies unconditionally; the
  aliasing path becomes `setStringBorrowed(String)` with its lifetime
  bound documented. `setStringOwned(#int8[], int32)` is unchanged.
- **5.2** An audit of `cajeta.*` for the §2.6 pattern (ownership
  contingent on a runtime property) — `setString` is the known
  instance; the audit establishes whether it is the only one.
- **5.3** An audit of borrow-returning accessors for §2.7 naming and
  one-line lifetime documentation.
- **5.4** `cajeta-llama`'s `TplEval` drops its local workarounds once
  §5.1 lands.
- **5.5** **An ownership check whose blast radius is unknown lands as a
  WARNING first, is migrated against in one pass, and is then flipped to
  an error.** §3.4 says this for an API change; it holds at least as
  strongly for a check, and for a reason the API case does not have: a
  thrown diagnostic stops the build at the FIRST offending site, so the
  only way to see the second is to fix the first and rebuild. The
  enumeration costs one full compile per site and never shows a total.

  This is recorded from measurement, not preference.
  `CAJETA_ERROR_CAPTURED_BORROW_PARAM` landed error-first against a
  static audit's estimate of 10 non-exempt sites; the routine gate then
  found 76 failures (1354/76/7 against a 1426/0/11 baseline), including
  captures through straight-line locals that no source-shape pass
  recognises — the compiler sees every one, an audit sees the shapes it
  was taught. Unit 3's acceptance has been open ever since.

  The demotion is a MIGRATION INSTRUMENT with a defined end, not a
  permanent severity option: off by default, switched by environment
  (`CAJETA_CAPTURED_BORROW=warn`) rather than by source annotation so no
  code can opt itself out permanently, and the plan item that opens it is
  closed only by flipping back. Tests pin both positions, so "the check
  is an error again" is verified rather than asserted.

## 6. Acceptance

- **6.1** The two checks are implemented, with a test per rejection
  and per non-rejection (conforming code still compiles).
- **6.2** The `cajeta-llama` Unit 13 bugs are reproduced as compiler
  tests, and each is recorded as caught, uncaught-with-reason, or
  not-reproducible.

  *Re-grounded.* This previously asserted "three of the four now fail to
  compile" — a number written before the check existed and never
  verified against it. What is measured today: the `keyAt` shape is
  rejected (`TransferOfBorrowTests.llamaKeyAtShapeRejected`), and the
  same shape behind an unprovable accessor is NOT rejected and still
  corrupts (`OwnershipArrayCanaryTests`, DISABLED, §7.2). An acceptance
  criterion that states a count in advance invites the count to be
  defended; stating the disposition per bug does not.
- **6.3** The stdlib builds clean and the routine gate is green.
- **6.4** The convention is documented where developers meet it — the
  language-ownership skill and the stdlib overview — not only here.

## 7. Open questions

- **7.1** Does §4.1 need an escape hatch for genuinely-unsafe interop
  (a `Cajeta.assumeOwned(x)` intrinsic), or does its absence force
  better APIs? Recommendation: ship without one and add it only against
  a real case.
- **7.2** Is §4.2 decidable for a value passed through an intermediate
  local before being stored? Recommendation: track through
  straight-line locals; conservatively allow what it cannot prove, so
  the check never blocks valid code.

  *No longer hypothetical.* The same gap exists on the §4.1 side and is
  measured: an accessor written `T v = this.data; return v;` is not a
  provable view, so the check stays silent while the code corrupts
  identically to the shape it does catch (`OwnershipArrayCanaryTests`,
  shipped DISABLED for exactly this reason). Straight-line tracking
  closes both sides at once, and those two tests are its acceptance —
  they should pass with no edit.

- **7.5** Should the language express a view's validity bound (§3.6) —
  invalidated-by-operation, or valid-while-another-object-lives — or
  does documenting it suffice? No recommendation yet: `llama.cpp` proves
  the need exists in real systems (§1.4.3), but nothing in `cajeta.*`
  currently has this shape, and inventing lifetime syntax against zero
  in-tree instances is how a language grows features nobody uses. Revisit
  when the first in-tree case appears — the KV-cache and mmap'd-weight
  paths in `cajeta-llama` are the likeliest source, since they are where
  `llama.cpp` hit it.

- **7.3 CLOSED 2026-08-14 — no producer/consumer/sink annotation, in
  docs or in code.** Decided by the developer; the reasoning below is
  kept because it also sizes what remains.

  A separate question stays open and is NOT closed by this: whether the
  caller's-choice PARAMETER should get its own spelling (a language
  change, not an annotation). It is deferred behind §4.2 by the
  sequencing argument at the end of this item.

  Parameter position has two spellings and three meanings, and two of
  the three are spelled identically:

  | Spelling | Meaning | Visible at the call site? |
  |---|---|---|
  | `#T p` | must transfer | yes |
  | `T p`  | borrow; callee must not keep | — |
  | `T p`  | MAY keep; caller chooses (`ArrayList.add`) | **no** |

  A caller reading `add(T v)` cannot tell whether the value is kept.
  That ambiguity is what let `setString(String)` be a capture wearing
  the signature of a read. The audit counts 25 caller's-choice (`#=`)
  sites and 69 plain stores, so it is not a corner case.

  **No producer/view/sink annotation ships**, at class or method level.
  The audit settles the class-level form —
  `JsonValue` alone is producer (`asString`), view (`asArray`,
  `asObject`), sink (`setStringOwned`) and capture (`setString`), and
  `JsonObject` and `String` mix roles the same way; a class-level mark
  would be wrong more often than right. For RETURN position the
  language already carries the mark (`#T` vs `T`) — a second channel
  would only create something that can disagree with the signature, and
  it demonstrably would: four `@Native` `String` methods were already
  declaring plain returns while transferring ownership.

  Sequence instead: land §4.2 first, which makes a plain parameter MEAN
  "not kept" by enforcement rather than by convention, then re-run the
  audit and decide the third spelling against the migrated numbers. If
  the caller's-choice sites remain few and are all genuine containers,
  a new spelling may not pay for its migration cost (§Unit 7); if they
  are scattered across non-containers, it does.

- **7.4** Any signal added must be IN THE SIGNATURE, not in prose. The
  evidence from this spec's own origin: the author read `keyAt`'s
  signature and still got it wrong, and the documentation that would
  have prevented it sat in another file. A compiler check stops the
  mistake at the line; a doc page does not, and an unenforced signal
  rots — which is exactly what those four `@Native` declarations did.
