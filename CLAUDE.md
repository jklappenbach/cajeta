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
from the arriving flag (`LocalVariableDeclaration.cpp:251`).

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
- **Non-sink keeping a parameter** — spell it `#T`. A plain parameter
  that is quietly captured is invisible at the call site.
- **Deliberate non-owning alias** (back-pointers, intrusive links,
  view handles) — store with `#=`, which records the borrow faithfully.

## 4. Traps that have actually bitten

- `JsonValue.asString()` returns escapes **VERBATIM** — `\n` stays two
  characters. Decoding needs `JsonReader.currentDecodedString`. Six
  cajeta-llama tests looked like engine bugs and were one harness bug.
- `JsonObject.keyAt(j)` returns a **borrow** of interior key storage.
  `heap String(#kb, kl)` on it corrupts.
- `JsonValue.setString(String)` stores the buffer as a borrow. Use
  `setStringOwned(#bytes, len)`.
- `Optional.get()` returns a borrow; `Optional.take()` is the owned
  counterpart. `#opt.get()` is a transfer that silently does nothing.
- int64 `*` **traps on signed overflow** (`imul`+`jo`→`ud2`), so
  in-language multiplicative hashing (FNV) is impossible — use
  `Cajeta.hashBytes` (XXH3-64).
- `view` is a reserved word.

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
