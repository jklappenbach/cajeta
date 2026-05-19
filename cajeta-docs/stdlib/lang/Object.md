# `cajeta.lang.Object` — universal root

Every class implicitly extends `Object` via the auto-extend pass in
`CajetaLlvmVisitor::visitClassDeclaration` — there is no syntax to
opt out, and there is no parallel root hierarchy. The four methods
declared on `Object` are inherited by every user class, overridable
per the rules below.

```cajeta
public class Object {
    public boolean operator==(Object obj);    // default: identity
    public int64   hash();                     // default: identity
    public String  toString();                 // default: null
    public Object  clone();                    // default: null
}
```

Status: implemented for implicit extension, identity `hash()`, and
the operator==/hash() override-pair check. `toString` / `clone`
defaults and the structural synthesis surface tracked in
[Features.md](../../../Features.md).

---

## Defaults are identity, not structural

Two distinct instances with the same field values compare unequal
and hash differently by default — same shape as Java's
`Object.equals` / `Object.hashCode`. Structural value-equality is
opt-in via `@AutoHash` (see [Hashing.md](../Hashing.md)) or by
overriding `operator==` and `hash()` manually.

```cajeta
Foo a = heap Foo();
Foo b = heap Foo();
boolean same = (a == b);   // false — identity, not structural
```

This is deliberate. Java's experience with structural defaults
hidden behind `==` (and the resulting `==` vs `.equals()` foot-gun)
informed the choice to keep `==` as the language-level operator that
*always* means "this comparison goes through `operator==`" — what
varies is what `operator==` itself does in each class. The default
on `Object` is identity; user overrides flip it to value-equal.

`String` (see [String.md](./String.md)) is the most notable
override: `String.operator==` is **value equality** (`"abc" == "abc"`
returns `true` regardless of allocation identity). That choice is
locked because Java's identity-`==`-on-String design caused an
enormous fraction of real-world Java bugs.

---

## The equal-implies-same-hash contract

If two values compare equal under `operator==`, they MUST also hash
equally under `hash()`. HashMap / HashSet rely on this invariant —
violating it means an inserted key becomes un-findable.

**The lint pass surfaces unpaired overrides.** A class that
declares one of `operator==` / `hash()` but inherits the other from
`Object` triggers the `equals-hash-pair` lint warning (see
[LintRules.md](../../LintRules.md) § `equals-hash-pair`). Build
proceeds; the warning makes the likely bug visible at compile time
without blocking iteration. Suppress via
`@SuppressLint("equals-hash-pair")` for the rare case where the
class isn't a map / set key.

A class that defines neither inherits the identity defaults from
`Object` — both halves are consistent because identity-equal implies
identity-hash.

A class that defines `operator==` for value equality but inherits
identity `hash()` from `Object` is the common Java foot-gun: equal
values produce different bucket indices and stored entries become
un-findable. The lint warning surfaces this at compile time rather
than at the misbehaving HashMap operation later. (Earlier drafts
specified this as a compile error; demoted to lint 2026-05-18 — the
runtime safety net + visible diagnostic is enough; users shouldn't
be blocked from compiling while iterating.)

---

## `hash()` — pluggable algorithm

The default `Object.hash()` returns a pointer-identity hash mixed
with the per-process seed (see
[`cajeta.hash.Hash.identity`](../../../runtime/src/cajeta/hash/Hash.cajeta)).
Subclasses override `hash()` directly when they want different
semantics — or apply the `@Hash` annotation to pick a standard
algorithm without writing the body.

### Algorithm-class registry

The standard algorithms live in `cajeta.hash` as concrete classes
implementing a uniform `Hasher` contract. Users can add their own —
nothing in the design is closed.

| Class          | Algorithm                | Use case                                      |
|----------------|--------------------------|-----------------------------------------------|
| `Murmur3`      | MurmurHash3-x64-128      | fast general-purpose; trusted-key scenarios   |
| `SipHash`      | SipHash-1-3 (keyed)      | HashDoS-resistant default for user-input keys |
| `FNV1a`        | FNV-1a 64-bit            | tiny / embedded use, well-known               |

(Earlier drafts named `XXHash3`, `RapidHash`, `MD5`, `DefaultHasher`
— retired 2026-05-18 in favor of the three open well-known
algorithms. The cryptographic family — SHA-2, SHA-3, BLAKE2/3 — is
out of scope for `cajeta.hash` and lives in the future
`cajeta.crypto` peer library.)

### `@Hash` annotation

```cajeta
@Hash(Murmur3.class)
public class Point {
    int32 x;
    int32 y;
}

@Hash                                            // bare — default algorithm
public class Session {
    String userId;
    String token;
    @Hash.Exclude Instant lastSeenAt;            // skip transient field
}
```

Two forms:

- **`@Hash(AlgorithmClass.class)`** — explicit algorithm pick.
  `@Hash(Murmur3.class)`, `@Hash(SipHash.class)`, `@Hash(FNV1a.class)`.
- **`@Hash` (bare)** — synthesize structural hashing with the
  *default* algorithm.

Both forms synthesize a `hash()` override on the annotated class.
The synthesized body walks the class's fields in declaration order,
hashes each via the chosen algorithm's primitive fast-paths, and
combines them via `Hash.combine`.

The earlier `@AutoHash` annotation is retired (2026-05-18) — the
single `@Hash` annotation covers both the explicit and shorthand
cases.

**Field-level `@Hash.Exclude`** skips a field from the synthesized
walk — matches Java's `@EqualsAndHashCode.Exclude`. Useful for
cached timestamps, transient connection handles, anything that's
part of the runtime state but not the value identity.

**Default algorithm** for bare `@Hash` is **SipHash-1-3**
(2026-05-18 lock). Rationale: HashDoS exposure of a fast-but-
attackable default surfaces as a security bug the first time user-
controlled input lands as a HashMap key — a guaranteed eventuality
(HTTP headers, JSON field names, query params). The fix-after-the-
fact path is painful; 2× throughput on the non-critical hashing
path is a cheap price for safe-by-default. Users explicitly opt into
Murmur3 (or FNV1a) when they own all the keys and need the cycles
back.

### `String.hash()` — the polynomial default

`String` overrides `hash()` with the Java-style polynomial mix
(`h = 31*h + c` over the UTF-8 bytes; cheap, well-distributed for
non-adversarial use). This is the historical Java choice and is
HashDoS-attackable — that's accepted as a v1 trade-off; the
security-level enforcement story (forcing SipHash for HashMaps keyed
on attacker-controlled input — web sessions, HTTP headers, query
params) is a separate discussion (see *Open question: security
level enforcement* below).

---

## `operator==` — value semantics, except where overridden

`Object.operator==` returns identity (pointer equality) by default.
Subclasses override it for value semantics; the override-pair check
ensures `hash()` is overridden in lockstep.

```cajeta
public class Money {
    public int64 cents;
    public String currency;

    public boolean operator==(Object obj) {
        if (!(obj instanceof Money)) { return false; }
        Money other = (Money) obj;
        return this.cents == other.cents
            && this.currency == other.currency;
    }

    public int64 hash() {
        int64 h = this.cents.hash();
        h = Hash.combine(h, this.currency.hash());
        return h;
    }
}
```

`String`'s override is **value equality** — locked, not identity.
See [String.md](./String.md) § Equality.

---

## `toString()` — deferred

Default returns `null` until the structural synthesizer lands AND
the `cajeta.lang.String` construction surface stabilizes. The
synthesized form will follow `TypeName(field1=value1,
field2=value2, ...)` — close to Rust's `#[derive(Debug)]`, sufficient
for `println(x)` during development.

Override manually for user-facing presentation. The synthesized
default is intentionally debug-shaped, not presentation-quality —
production user-facing strings are localization concerns and
shouldn't fall out of `toString` automatically.

Tracked in Features.md.

---

## `clone()` — deferred

Default returns `null` until the synthesizer pass can walk field
layouts and emit:

- **value-typed fields** (primitives, structs, enums) — copied by
  `memcpy` of the bits
- **class-typed fields** — copied by reference (both originals then
  point at the same heap instance — Java-shallow semantics)

Override `clone()` manually to deep-copy class-typed fields when
shallow isn't right. The base return type is `Object`; subclass
overrides narrow it to the declaring class at every site, so
call-site code doesn't need a cast (covariant return).

Tracked in Features.md.

---

## Locked in the 2026-05-18 hashing pass

- **`Hasher` interface** — one-shot only, three methods:
  ```cajeta
  public interface Hasher {
      int64 hash(byte[] bytes, int32 len);     // primary entry
      int64 hashInt32(int32 v);                 // primitive fast paths
      int64 hashInt64(int64 v);
  }
  ```
  No streaming `update()` / `finish()` — object APIs hand over data
  all at once.

- **Algorithm classes** — static-method utilities (not instantiable).
  `SipHash.hashBytes(buf, len, Hash.processSeed())` rather than
  `new SipHash(key).hash(...)`. The per-process key is threaded
  from the runtime via `Hash.processSeed()`.

- **`Hash.processSeed()`** — sourced from the OS cryptographic-
  entropy syscall (`getrandom` / `getentropy` / `BCryptGenRandom`)
  at process startup. On entropy failure: log + abort with
  `CAJETA_ERROR_ENTROPY_UNAVAILABLE`. No clock+pid weak fallback —
  silent degradation would defeat HashDoS protection.

- **`@Hash` annotation** — single annotation, two forms (bare for
  default algorithm, parameterized for explicit). `@AutoHash`
  retired. Field-level `@Hash.Exclude` skips transient fields.
  Default algorithm: **SipHash-1-3** (safe-by-default for the
  attacker-controlled-input case).

- **equal/hash override pair check** — lint warning, not compile
  error. Rule ID `equals-hash-pair` in
  [LintRules.md](../../LintRules.md). User can suppress per-class.

## Open questions

Still pending follow-up:

1. **`String.hash()` HashDoS mitigation.** Pure polynomial leaves
   String HashMaps attackable. Lean: mix `Hash.processSeed()` into
   the polynomial result by default (cheap, kills offline-precompute
   class of attacks); explicit `@Hash(SipHash.class)` on the
   containing class for full protection.

2. **Security-level enforcement mechanism.** How do we mark types
   / fields / collections as "must use a DoS-resistant hash"?
   - `@Hash.Secure(min=Strength.HIGH)` on HashMap declarations
   - capability annotation: `@requires("attacker-resistant-hash")`
   - runtime check at HashMap construction in `--release` mode
   Deferred to a dedicated session.

---

## Cross-references

- [Lang.md](../Lang.md) — the broader `cajeta.lang` overview
  (Object historically lived inline there; this doc is the
  authoritative spec going forward, Lang.md trimmed to a pointer).
- [String.md](./String.md) (pending — task #157) — String's
  overrides of `hash()` / `operator==` / `toString`.
- [Hashing.md](../Hashing.md) — the broader hashing doctrine,
  `cajeta.hash.Hash` namespace, `@AutoHash` / `@Hash` synthesizer
  design, per-process seed lifecycle.
- [MultiClassing.md](../MultiClassing.md) — how multi-parent
  hierarchies resolve the implicit `extends Object` (every class
  extends Object; multiple-inheritance shares the single Object
  sub-object).
- [`runtime/src/cajeta/lang/Object.cajeta`](../../../runtime/src/cajeta/lang/Object.cajeta)
  — the source declaration and its inline doc.
