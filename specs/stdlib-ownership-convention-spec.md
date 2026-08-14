# stdlib-ownership-convention — spec

Authored 2026-08-14, from evidence gathered implementing `cajeta-llama`
Units 11–13. Four ownership bugs in one unit, one root cause, and only
one of the four caught at compile time.

## 1. Definition

### 1.1 Purpose

Fix a **single, uniform convention** for who owns data crossing a
library API boundary in the cajeta standard library and first-party
libraries, and add the two compiler checks that make the convention
enforceable rather than aspirational.

### 1.2 The problem, from evidence

Cajeta's plain `=` **lends**; `#` transfers title. The compiler proves
this within a function body — `CAJETA_ERROR_DANGLING_LEND` is precise
and helpful. It cannot see an ownership contract that lives in another
library's *documentation*, and today stdlib APIs disagree about that
contract with no signal at the call site.

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

### 1.4 Non-goals

- Changing the language's ownership model. `=` lends, `#` transfers,
  `#=` stores title. That model is sound and is not in question.
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
  `asBytes`, `get(i)`) it returns a **borrow** (plain `T`), never takes
  title, and the caller copies if the value must outlive the container.
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
  argument — its representation, length, or provenance — the API is
  **non-conforming**. Ownership is a static contract or it is not a
  contract.
- **2.7** When a borrow-returning accessor exists, its documentation
  states the lifetime bound in one line, and its name does not suggest
  materialization (`viewOf`, `bytesAt`, `...At` read as views; `to...`,
  `as...`, `read...` read as producers).

## 3. Use cases

- **3.1** When a caller writes `#x` where `x` is a borrow, the compiler
  rejects it — surrendering title one does not hold is the mirror of
  `DANGLING_LEND`, and it is what turned `keyAt` into corruption.
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

## 4. Compiler enforcement

Convention without enforcement decays; these are the two checks that
would have caught three of this unit's four bugs at the line rather
than as corruption.

- **4.1** Reject `#x` where `x` holds a borrow returned by a plain
  (non-`#`) call. Local, decidable, no inference required: the callee's
  declared return spelling is static truth.

  *Amended 2026-08-14 during Unit 2, from contact with the compiler.*
  Three corrections to this item as first drafted:

  - The diagnostic is the EXISTING `CAJETA_ERROR_MOVE_OF_BORROW`, not a
    new code. The compiler already rejects transfer-of-a-borrow; its own
    comment names the call-result case as a deliberate, documented gap
    ("stays unchecked until the `#?` runtime-owner ABI can carry its
    role"). Closing that gap needs no runtime ABI, and a second code for
    one defect would fragment the diagnostics.
  - **Plain parameters are NOT covered, and must not be.** A formal's
    ownership is fixed at the call site and carried at run time by the
    transfer word: `f(x)` lends, `f(#x)` transfers, and `#p` inside the
    callee forwards whichever mode arrived (conditional acquisition),
    with `#=` recording the forwarded mode per slot. Rejecting `#p`
    statically would break that design and outlaw every mode-forwarding
    wrapper. The existing check excludes formals deliberately.
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

## 6. Acceptance

- **6.1** The two checks are implemented, with a test per rejection
  and per non-rejection (conforming code still compiles).
- **6.2** The four `cajeta-llama` Unit 13 bugs are reproduced as
  compiler tests, and three of the four now fail to compile.
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
