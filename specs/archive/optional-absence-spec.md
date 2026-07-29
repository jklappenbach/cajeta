# optional-absence — one absence idiom for the error package

## 1. Definition

### 1.1 Purpose
Make absence explicit at the type level in the error package: accessors that
can have nothing to return (`Throwable.getCause`, `hint`, `docUrl`) return
`Optional<T>` instead of a `null` reference, and `Optional.get()` becomes
checked so an unguarded unwrap fails loudly instead of yielding a zero value.

### 1.2 Problem
- The error package mixes absence sentinels today: `getCause()` returns
  `null` while its doc comment says `0`; `Exception` ctors assign
  `this.cause = 0`; `hint()`/`docUrl()` return `null` for unset. Callers
  have no type-level signal that a check is required.
- `Optional.get()` v1 returns the zero-initialized value on empty (its own
  doc calls this temporary), so wrapping in Optional today buys the
  signature without the guarantee.

### 1.3 Constraints
- `Optional` is a stack value; returning one costs a boolean plus a
  reference word by value — no heap object, no rc traffic. Precedent:
  `Cache.get` and every `Stream.next()` implementation.
- Wrapping a borrowed field is already supported plain
  (`stack Optional<V>(true, node.value)` — no `#`); only owned locals pass
  with `#`. No new construction form is needed.
- Breaking signature change to `Throwable`/`Exception`; acceptable now
  because `Throwable` landed 2026-07-03 and has no external users yet.

### 1.4 Non-goals
- No package-wide Optional sweep beyond the error package.
- No new Optional construction forms.
- No `try`-style sugar or throw-integration redesign; `get()` uses the
  existing throw machinery.

## 2. Checked `Optional.get()`

### 2.1 Requirements
1. `get()` on an empty Optional throws `NoOptionalValueException`
   (`extends RecoverableException` — an unwrap miss is catchable and the
   program continues; `UnrecoverableException` is reserved for panic),
   message naming the operation, stable diagnostic code = the class FQN.
2. `get()` on a present Optional is unchanged.
3. Every existing stdlib `get()` call site is audited: each is either
   guarded by `isPresent()`/`isEmpty()` or provably present; unguarded
   sites migrate to `orElse(...)` or gain guards.
4. `Optional`'s class doc drops the "temporary v1" caveat and documents the
   throwing contract.

### 2.2 Use cases
1. As a **developer**, when I call `get()` on an empty Optional, I get a
   typed, catchable throw naming the unguarded unwrap at the fault site —
   not a silent zero value that corrupts downstream state.
2. As a **stdlib author**, when I return `Optional<T>`, I can rely on the
   wrapper enforcing its own contract.

## 3. Error-package migration

### 3.1 Requirements
1. `Throwable.getCause()` returns `Optional<Throwable>` (empty when no
   cause), wrapping the borrowed field per the `Cache.get` pattern; the
   `Exception` override matches.
2. `Throwable.hint()` and `docUrl()` return `Optional<String>`.
3. `toJson()`'s cause-chain walk uses `isPresent()`/`get()` instead of
   null checks; output is byte-identical to today's.
4. The `null`/`0` sentinel mix in error-package sources and doc comments is
   gone: fields may keep null internally, but no public surface returns it
   and no doc comment says `0` for "none".
5. Ripple closed in the same change: `docs/stdlib/error/*.md` tables,
   guide chapter 20 examples, tour demos touching these accessors, and all
   doc gates green.

### 3.2 Use cases
1. As a **user**, when I catch a `Throwable`, `getCause()` forces me to
   handle the no-cause case at the type level.
2. As an **agent consuming diagnostics**, `toJson()` output is unchanged —
   the migration is source-level only.
