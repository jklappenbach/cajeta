@td-project-workflow.md

# Ownership: borrow and transfer

**Read this before writing or reviewing any cajeta that moves a value.**
Every statement below was MEASURED against the compiler (test named in
brackets), not inferred. The simplified model — "`=` lends, `#`
transfers" — is wrong in three separate ways and cost a full day of
wrong conclusions on 2026-08-14; each correction is marked.

## 1. Who decides, by position

Ownership is **runtime-conditional on both sides of a call**. What
differs by position is *who decides*:

| Position | Decided by | Carried in | Spelling |
|---|---|---|---|
| name → name | the **spelling** | statically | `=` lends, `#=` transfers |
| call argument | the **caller** | the transfer word | `f(x)` lends, `f(#x)` transfers |
| return | the **callee** | the return-flag TLS | plain `T` may STILL carry a title |
| slot store | the **source's mode** | per-slot bit, via `#=` | a lend stays a lend |

Only the first row matches the one-line summary. Do not reason from it
alone.

## 2. The three corrections

**2.1 A plain (non-`#`) return is NOT statically a borrow.**
A plain-return wrapper that tail-calls a `#` method rides the inner
flag through, and `Stream.fold<R>` does it through its callback's `#R`
— genuinely runtime-variable, since the callback is a parameter.

```cajeta
public static #Cell fresh()    { return heap Cell(7); }
public static Cell  viaPlain() { return D.fresh(); }   // returns a TITLE
```

[`SignatureAbiTests.tailCallThroughPlainReturnKeepsTitle`]
So `T x = someCall()` is not a lend: the local's drop entry is armed
from the arriving flag (`LocalVariableDeclaration.cpp:821`,
`ownership::titleFlag(initShape, module)` under `initIsFlaggedCall`).

**2.2 `#x` on a borrow does NOT transfer — it forwards the mode it was
handed.** The lender keeps title and frees on drop, so a receiver that
OUTLIVES the lender reads reused memory. Measured, both kinds:

| payload | read back | expected |
|---|---|---|
| array | `-83968` | `8247` |
| class | `107800` (= the churn allocation) | `8100` |

[`OwnershipArrayCanaryTests`] This is a use-after-free, not a stylistic
issue.

**2.3 `#=` is MODE-CARRYING, not a transfer.** It records whatever mode
the source actually holds — a lent source records a BORROW. It makes no
claim of title, so it is always safe, and it is the correct spelling for
a deliberate non-owning alias (`Cache`'s LRU links, `Channel`'s slots).
Its own desugar says so: *"when no title was tendered the store records
a borrow."*

## 3. Conventions to follow

- **Producer** — materializes a new value (`asString`, `toBytes`,
  `readBytes`): return **owned** `#T`. Never hand back a window into
  another object's interior from a conversion-shaped call.
- **View** — exposes interior state (`keyAt`, `get(i)`): return plain
  `T`, and return ONLY interior reads, so the flag is always borrow.
  The caller copies if the value must outlive the container.
- **Sink** — a container whose job is holding values: take plain `T`
  and store with `#=`, so `add(v)` lends and `add(#v)` transfers. This
  is the `ArrayList` model and the only genre where the caller chooses.
- **Keeping a value in a field or slot.** Decided with Julian
  2026-10-04 (specs/field-store-ownership-spec.md). This REPLACES the old
  "non-sink keeping a parameter: spell it `#T`" rule, which was wrong:
  `#T` with a plain `=` store still dangles. The compiler enforces rules 1
  to 4 before codegen (CAJETA_ERROR_KEEP_NEEDS_SHARP_STORE,
  CAJETA_ERROR_INTERIOR_KEEP_NEEDS_BORROW_PARAM,
  CAJETA_ERROR_TRANSFER_INTO_BORROW_PARAM), and CAPTURED_BORROW_PARAM is
  retired. Rule 5 (String `=` is a plain borrow) is not done yet: String
  `=` still resolves a copy.
  1. `=` is a borrow, `#=` records the mode that arrived (a transfer when
     the source owns, a borrow when it does not), `#` transfers. This
     holds for every field type, String included.
  2. A formal kept with `=` must be spelled `^T` (borrow only: callers
     cannot pass `#x`). This is the explicit way to keep a borrow.
  3. A `T` or `#T` formal that is kept is stored with `#=`. A sink
     (`add(T v) { slot #= v; }`) is this case: the caller chooses.
  4. An owned local stored into a field or slot is stored with `#=`.
  5. Storing an interior read of a `T`/`#T` formal (`b.child`) with `=` needs
     a `^T` formal, so the caller keeps the root alive. `#=` records the
     title the slot held. `^` is an error on a primitive formal and on a
     type-parameter declaration, and legal on a formal typed by one
     (`void foo(^T p)`).
  6. Plain `=` stays right for local bindings, arguments, literals,
     primitives, `null`, and a `heap T(...)` or call written in place.

  | Store | Today (v0.34.0, measured) | Rule |
  |---|---|---|
  | `#T p; this.f = p` | class: silent use-after-free. String: hidden move | error, use `#=` |
  | `T p; this.f = p` | class: CAPTURED_BORROW_PARAM. String: SIGSEGV on `#x` | error, use `#=` or `^T` |
  | `Cell l = heap ..; this.f = l` | silent use-after-free | error, use `#=` |
  | `this.f #= p` / `#= l` | correct in every case | correct |

  A `=` store that compiles and reads back correctly is NOT evidence it
  is right: the String field path hides a move. Check the IR.
- **Deliberate non-owning alias** (back-pointers, intrusive links,
  view handles): store with `#=`, which records the borrow faithfully,
  or take a `^T` formal and store it with `=`.
- **Owned temporary.** A `#T` call result or a `heap X(...)` creator
  used as the receiver of a further call is an owned temporary. It dies
  at the end of its statement. In an `if`, `while`, `do` or `for`
  condition it dies right after the condition, so a loop frees it on
  every pass. A plain class, `String`, array or interface result that
  reaches it can be used inside the statement. Binding that result to a
  local (with `=` or `#=`), storing it in a field, or returning it is
  `CAJETA_ERROR_BORROW_OF_TEMPORARY`. The check walks back through plain
  intermediate calls. `#T` and primitive results always pass.
  `String s = Doc.parse(t).title();` is rejected and
  `int32 n = Doc.parse(t).words();` passes. There are three fixes. End a
  builder chain with its `build()` in the same statement
  (`HttpServer s #= HttpServer.builder().bind("0.0.0.0:8080").build();`). Bind the
  temporary to a local first (`Doc d #= Doc.parse(t); String s = d.title();`).
  Or call a `#T` method that returns an owned copy
  (`String s #= Doc.parse(t).titleCopy();`). [`StatementTempTests`]

## 4. Traps that have actually bitten

- `JsonValue.asString()` returns escapes **VERBATIM** — `\n` stays two
  characters. Decoding needs `JsonReader.currentDecodedString`. Six
  cajeta-llama tests looked like engine bugs and were one harness bug.
- `JsonObject.keyAt(j)` returns a **borrow** of interior key storage.
  `heap String(#kb, kl)` on it corrupts. Same for
  `ProcessResult.stdout()/stderr()`. The compiler now catches this class
  (`CAJETA_ERROR_MOVE_OF_BORROW`) — copy the bytes and transfer the copy.
- `JsonValue.setString(String)` COPIES as of the
  stdlib-ownership-convention migration (§2.5) — measured 2026-09-03 against
  `JsonValue.cajeta:232`. It used to store a borrow, which is what bit
  cajeta-llama. `setStringBorrowed` is now the sharp variant; reach for
  `setStringOwned(#bytes, len)` only to transfer a buffer you already own.
- `Optional.get()` returns a borrow; `Optional.take()` is the owned
  counterpart. `#opt.get()` is a transfer that silently does nothing.
- int64 `*` **traps on signed overflow** (`imul`+`jo`→`ud2`), so
  in-language multiplicative hashing (FNV) is impossible — use
  `Cajeta.hashBytes` (XXH3-64).
- `view` is a reserved word.
- `Stream.collect<R>` returns `#R`, so on an `ArrayList` the
  one-statement form is legal: `ArrayList<int32> out #= xs.stream().collect(c);`.
  A plain `=` is `CAJETA_ERROR_OWNED_RESULT_NEEDS_TRANSFER`. `findFirst`
  returns `Optional<T>` by value. A by-value result that holds no reference
  (`Optional<int32>`) cannot reach a temporary, so
  `Optional<int32> hit = xs.stream().findFirst(p);` passes. An
  `Optional<Doc>` from a temporary stream is checked like any plain class. `fold<R>` still
  returns plain `R`. The stream from an `ArrayList`'s `stream()` is an
  owned temporary, so a class `R` from `xs.stream().fold<R>(...)` bound
  to a local is `CAJETA_ERROR_BORROW_OF_TEMPORARY`. Bind the stream
  first (`ArrayStream<int32> s #= xs.stream();`) and fold on `s`. A
  primitive result such as `fold<int32>` still passes. On a primitive
  array the same bind is accepted.
- Two shapes that keep a temporary compile and then read freed memory.
  `Stream<int32> e #= xs.stream().filter(p);` passes because `filter` is
  `#Stream`, but the stage borrows the stream temporary, and using `e`
  later faulted (SIGSEGV) for both an array and an `ArrayList`.
  `arr.add(#heap JsonValue().setNumber(1))` passes because an argument is
  a use, and the array keeps a node that was freed at the statement end.
  Bind the source or the node to a local first.

## 5. Method

Ownership behaviour is **measured, never reasoned about**. Three wrong
conclusions in one day all came from arguing about `#` instead of
testing it, and each was caught by a gate or a probe rather than by
review. Two specific habits that worked:

- **Validate the instrument before trusting a null result.**
  `Cajeta.liveCount()` cannot see arrays (delta 0 across an allocation)
  and cannot see a lend that never dangles within one scope. A balanced
  count is consistent with correct ownership AND with a transfer that
  never happened.
- **A check needs tests that assert it FIRES and tests that assert it
  does NOT.** A predicate that silently disabled a whole check read as
  a clean run for an hour.
