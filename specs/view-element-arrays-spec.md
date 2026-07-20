# View Var-Size Element Arrays (view v1.1) — Spec

> Status: **APPROVED** (2026-07-19, design skill). Plan: `agents/view-element-arrays-plan.md` (shared agents repo). Specs **arrays of variable-size
> elements in `view` declarations** — `V[]` fields where `V` is a (possibly
> var-size) view type, and `String[]` fields — with **O(1) element access** via
> a construction-time offset table built during the already-mandatory
> length-prefix validation sweep. This is the "deferred to v1.1" item at
> [`Views.md`](../docs/specification/lang/Views.md) §Variable-size fields.
> Companions: [`Views.md`](../docs/specification/lang/Views.md) (view v1),
> S5/S5b (multi var-size `String` fields, primitive `T[]` fields,
> post-variable fixed fields — `test/parser/MultiVarSizeViewTests.cpp`,
> `test/parser/PostVariableFieldTests.cpp`). First consumer:
> **cajeta-gossip's wire format** (`~/code/cpp/cajeta-gossip/docs/CajetaGossip.md`).
> Plan lives at `agents/view-element-arrays-plan.md` once approved.

---

## 1. Definition

### 1.1 Purpose

Let a `view` declare a **repeated-record field** — a length-prefixed sequence
of elements that each carry their own variable-size content — and read any
element in **constant time** with zero copies. This is the missing shape for
real protocol frames: a SWIM gossip datagram's piggyback delta list, DNS
resource-record sections, TLV option lists, Avro-style record blocks.

### 1.2 Scope (v1.1)

- **`V[]` view fields** where `V` is any view type — fixed-size or var-size
  (containing `String` / primitive `T[]` / nested view fields).
- **`String[]` view fields** (the primitive var-size element case).
- **O(1) element access** — `outer.field[i]` — backed by a per-instance offset
  table built during the construction validation sweep (no extra pass).
- **Recursive validation** — the constructor's length-prefix sweep descends
  into every element; a malformed inner prefix throws `ParseException` at
  construction, preserving view v1's "bounds-check-free after construction"
  guarantee.
- **Fixed-size-element fast path** — when `V` is fixed-size, element offsets
  are stride math; the table carries no per-element entries for that field.

### 1.3 Problem

View v1 (S5/S5b) supports multiple var-size `String` fields, primitive `T[]`
fields, and fixed fields after them — but **no arrays of var-size elements**.
A repeated `{fixed fields + name}` record — the piggyback list at the heart of
gossip's wire format — cannot be declared; consumers are forced into fixed-K
padded slots, parallel arrays, or a hand-rolled codec, forfeiting the zero-copy
+ validated-once model views exist to provide. S5b's walk-the-prefixes access
scheme was the right call for one or two trailing fields, but per-access
walking over an N-element array makes iteration O(N²) — a hidden quadratic
trap. The fix: the constructor **already** walks every length prefix to
validate the buffer; recording element start offsets during that same sweep
buys O(1) access for the cost of a table write per element.

---

## 2. Use cases

1. **Gossip piggyback delta list (the driving consumer).** The declaration
   gossip's spec calls for compiles and round-trips as written:

   ```cajeta
   @BigEndian
   view Delta {
       int8   state;              // MemberState ordinal
       int64  incarnation;
       int64  addrHi;             // 16-byte address (v4-mapped or v6)
       int64  addrLo;
       uint16 port;
       String name;               // var-size element ⇒ Delta is var-size
   }

   @BigEndian
   view GossipMessage {
       int32   magic;
       int8    version;
       int8    msgType;           // PING/ACK/PING_REQ/SYNC/LEAVE/USER
       int64   senderIncarnation;
       String  senderName;
       Delta[] deltas;            // u32 count + elements back-to-back
       int8[]  payload;           // USER messages only
   }
   ```

   Decode: `GossipMessage m = GossipMessage(datagram);` validates the whole
   frame once; `m.deltas[i]` is an O(1) borrow; `m.deltas.length` is the
   count. No allocation on the receive path.

2. **Repeated records in binary formats generally** — DNS RR sections, TLV
   option lists, container-format block indexes: any `count + self-delimiting
   records` layout.

3. **`String[]` directly** — e.g. a frame carrying a name list, without
   wrapping each string in a one-field view.

## 3. Requirements

### 3.1 Declaration + layout

- `V[] f;` is legal in a view when `V` is a view type; `String[] f;` is legal.
  Multiple such fields per view are allowed, in any position (same rule as
  S5b's var-size fields).
- Wire layout: `u32 count`, then `count` elements back-to-back, each laid out
  exactly as `V` (endianness/alignment inherited from the outer view unless
  `V` declares its own — same inheritance rules as nested views today).
- The element count prefix participates in the outer view's var-size offset
  arithmetic exactly like existing length prefixes.

### 3.2 Access semantics

- `outer.f[i]` yields a **view value of type `V` borrowing the outer buffer**
  (no copy). Lifetime: bounded by the outer view's lifetime, per the existing
  borrow rules for nested views.
- `String[]` element access returns the same thing a `String` view field read
  returns today (consistency rule — whatever S5's `String` field accessor
  produces, the element accessor produces).
- `outer.f.length` (count) is O(1). Out-of-range index throws
  `IndexOutOfBoundsException`, consistent with array indexing elsewhere.
- Access cost: **O(1)** for var-size `V` (offset table), stride math for
  fixed-size `V`.

### 3.3 Construction + validation

- The existing construction sweep extends: on reaching a `V[]`/`String[]`
  field it reads the count, then per element recursively validates the
  element's internal prefixes, recording each element's start offset in the
  instance's offset table as it goes. One pass, no re-walking.
- Any prefix overrun at any depth throws `ParseException` from the
  constructor, carrying the view type name, field, and element index.
- Both construction forms (borrow `V(buf)` / owning `V(#buf)`) work unchanged.

### 3.4 Representation (the implementation crux)

- Views that declare **no** var-size element array keep today's single-pointer
  value representation and ABI — zero regression, no calling-convention change
  for existing code.
- Views that do declare one carry a per-instance offset table. The table's
  size is dynamic (depends on the decoded count), so it cannot be a fixed
  alloca; the plan decides the mechanism (fat two-pointer value + dynamically
  sized stack allocation at the construction site is the expected shape).
  Whatever the mechanism: construction on the happy path performs **no heap
  allocation**.

### 3.5 Mutation

- Writing a **fixed** field of an element (`m.deltas[i].state = 2`) writes
  through to the buffer — allowed, same as any view fixed-field write.
- Reassigning an element, the array, or any var-size field remains a
  **static error** (view v1 rule unchanged). Encode stays writer-side; see
  §5.

## 4. Constraints

- All existing view tests stay green (S1–S5b, `PostVariableFieldTests`,
  `ViewSafeConsumerTests`, …).
- No new syntax beyond the two field forms — construction, `#`, borrow rules,
  endianness annotations all unchanged.
- `ParseException` remains the single malformed-input surface; no partial
  construction states.

## 5. Out of scope (registered follow-ups)

- **Encode-side builder** (`GossipMessage.builder() … seal()` → owned buffer):
  a new language surface deserving its own spec; var-size assignment through
  views stays a static error, and producers keep using writers.
- **Arrays of arrays** (`V[][]`, `String[][]`): no known consumer; the
  recursion would mostly fall out, but it is not specified, not tested, and
  rejected in v1.1.
- **Iterator integration** (`for (d in m.deltas)`): follows the stdlib
  Iterator/Optional design (`ToDo.md` "Up next"), not this spec.

## 6. Acceptance criteria

1. The §2.1 `Delta`/`GossipMessage` pair compiles and a golden datagram
   round-trips (construct → field reads → element reads) in a JIT test.
2. Parser: `V[]`/`String[]` fields accepted in any position; `V[][]` rejected
   with a located diagnostic.
3. Validation: truncated count, truncated element, and inner-prefix overrun
   each throw `ParseException` from the constructor (golden-vector tests, all
   depths).
4. O(1) access is pinned structurally (the emitted access path for
   var-size-element `V[]` contains no per-access prefix walk — asserted the
   same way S5b's tests pin the walk).
5. Fixed-size-element `V[]` uses stride math (no table growth with count).
6. Endianness: a `@BigEndian` outer with default-endian `V` byte-swaps
   element fields; an element view with its own annotation keeps it.
7. Existing view suites pass unchanged; single-pointer ABI confirmed intact
   for views without element arrays.
8. Downstream proof: cajeta-gossip's `GossipWireTests.roundTripAllMessageTypes`
   (G1) passes against the built toolchain — the unblocking event for the
   gossip plan.
