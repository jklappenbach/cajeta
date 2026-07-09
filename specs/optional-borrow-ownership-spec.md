# optional-borrow-ownership — `Optional<T>` cannot borrow, so wrapping a field double-owns it

## 1. Definition

### 1.1 Purpose
`Optional<T>` can only be constructed as an **owning** container. Its sole
constructor is

```cajeta
public Optional(boolean present, #T value)      // runtime/src/cajeta/lang/Optional.cajeta:71
```

and the `#` sigil **transfers ownership** of `value` into the Optional. There is no
construction path that wraps a value the caller still owns. Any accessor that returns
`Optional<T>` over a field it does not relinquish therefore creates **two owners** of
the same object, and the Optional's scope-exit drop frees it out from under the field.

This is the exact failure `element-ownership` describes for containers generally —
"a map built over borrowed items, processed, and the *structure* discarded; `put` must
take a plain borrow with no copy and no drop" (`element-ownership-spec.md` §, "As
**scratch**"). `Optional` is the smallest such container and the first to ship the bug.

### 1.2 Scope
- `Optional<T>`'s construction surface, for class-typed `T`.
- Every stdlib accessor that returns an `Optional` over a **borrowed** field. Today
  `Exception.getCause()` is the known instance; the sweep in 3.1 must find the rest.
- The two shipped consumers of the cause chain: `Throwable.toJson()` and
  `Throwable.printStackTrace()`.

### 1.3 Non-goals
- Redesigning `#` / element-ownership. This spec **consumes** the semantics that
  `element-ownership` already defines; it does not extend them.
- Borrow-checker enforcement of the hazard. Field-store and capture of a borrow are
  already accepted by the compiler and deferred to lint + debug-runtime.
- `Optional<T>` for primitive `T` (no ownership, unaffected).

### 1.4 The defect, precisely
`Exception.getCause()` (`runtime/src/cajeta/error/Exception.cajeta:62-67`) returns

```cajeta
return stack Optional<Throwable>(true, this.cause);   // `this` still owns `cause`
```

Its doc-comment claims "Wraps the borrowed field (no transfer)". The `#T` ctor
contradicts that: the Optional takes ownership. While the Optional is a **method-scope**
local its drop lands after last use and the aliasing is invisible. When it is
**loop-scoped**, it drops every iteration and frees the object the walker just aliased.

Verified on `main` @ `5f527bea`, via `cajeta jit-run`:

| cause-chain links | `toJson()` | `printStackTrace()` walk |
|---|---|---|
| 0 | ok | ok |
| 1 | ok | ok |
| **2** | **SIGSEGV** | **SIGSEGV** |

The crash lands on the second loop iteration, immediately after the first
`Optional<Throwable> nxt = c.getCause();` goes out of scope. The identical straight-line
walk with method-scope Optionals completes cleanly at depth 3 — that difference is the
proof it is the Optional's drop and not the chain itself.

### 1.5 Severity
`Throwable.toJson()` is **shipped and reachable from user code** (`@EntryPoint`). It is
the `--diag-format=json` serialization path. Any program that serializes an exception
carrying two or more nested causes crashes. CI does not see it: the only nested-cause
test, `DiagnosticJson.causeChainSerializesNestedCauses`, builds a chain exactly **one**
link deep.

---

## 2. Optional's construction surface

### 2.1 Requirements
`Optional<T>` must be constructible in both element modes, so an accessor can hand back
a view of a field without claiming it.

### 2.2 Use cases
- **2.2.1** As a stdlib author, when I return an `Optional<T>` over a field my object
  still owns, then the Optional must **not** drop that field at scope exit, and the
  object's own drop remains the single owner.
- **2.2.2** As a stdlib author, when I move a freshly-constructed value into an
  `Optional<T>`, then the Optional owns it and drops it at scope exit, exactly as today.
- **2.2.3** As a caller, when I bind `Optional<T>` locals inside a loop body over a
  borrowed chain, then each iteration's drop must be a no-op with respect to the
  borrowed payload, and the walk must be safe to arbitrary depth.
- **2.2.4** As a caller, when I take an owning `Optional<T>` and let it drop without
  calling `get()`, then the payload is released exactly once (no leak, no double free).

---

## 3. Stdlib accessors returning Optional over borrowed state

### 3.1 Requirements
Every stdlib accessor that wraps a field in an `Optional` must be audited and moved to
the borrowing construction path. The audit is part of the work, not an assumption: the
optional-absence migration (`8ed20e07`) converted the error package to `Optional` and may
have introduced this pattern in more than one place.

### 3.2 Use cases
- **3.2.1** As a maintainer, when I grep the stdlib for `Optional<` constructions whose
  argument is a field access (`this.x`), then each is either borrowing or provably
  transfers a value the enclosing object no longer owns.
- **3.2.2** As a maintainer, when an accessor's doc-comment claims "no transfer", then
  the code must actually not transfer.

---

## 4. Cause-chain consumers

### 4.1 Requirements
Both shipped walkers must survive an arbitrarily deep chain, and a cycle must not hang.

### 4.2 Use cases
- **4.2.1** As a user, when I call `toJson()` on an exception with N >= 2 nested causes,
  then I get a `causeChain` array of N entries and no crash.
- **4.2.2** As a user, when I call `printStackTrace()` on an exception with N >= 1 nested
  causes, then each cause prints on its own line prefixed `Caused by: `, after the
  wrapping throwable's message and frames (ExceptionReview 5.7, currently blocked here).
- **4.2.3** As a user, when a cause chain contains a cycle, then the walk terminates
  rather than spinning or overflowing.
