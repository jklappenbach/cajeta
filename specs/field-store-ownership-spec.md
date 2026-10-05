# Field store ownership (spec)

Decided with Julian 2026-10-04. Supersedes stdlib-ownership-convention §2.4
and §4.2. Takes over diagnostic-origin Unit 6, whose premise (spec 5.3 there)
this replaces.

## 1. Definition

### 1.1 Purpose

A store into a field or an array slot is where a value starts to outlive the
statement that produced it. Today the same spelling means different things by
type, and two of the meanings are silent use-after-frees. Measured on v0.34.0,
caller passing `#x` (section 8 has the numbers):

- `#Cell p; this.c = p` stores a borrow, and the formal frees the object at
  return. It compiles with no diagnostic.
- `#String p; this.v = p` compiles to the same IR as `#=`. The plain `=` is a
  hidden move.
- `String p; this.v = p` is a half move: the field claims the pointer and the
  formal still frees it. SIGSEGV.
- `Cell p; this.c = p` is CAJETA_ERROR_CAPTURED_BORROW_PARAM, which forbids a
  legitimate borrow.
- `Cell local = heap Cell(1); this.c = local;` stores a borrow of an owner that
  dies at return. It compiles with no diagnostic.

The language has three spellings and they should each mean one thing. `=` is a
borrow. `#=` records whatever mode the source holds: a transfer when it owns, a
borrow when it does not. `#x` and `#T` transfer. This spec makes field and slot
stores follow those meanings for every type, adds the one marker needed to say
"this parameter is only ever borrowed", and makes the compiler reject, at
compile time, every store that would dangle on every call.

### 1.2 The rule

1. A formal may be kept with `=` only if it is spelled `^T`. A `^T` formal is a
   borrow on every call, and its callers cannot transfer into it.
2. A `T` or `#T` formal that is kept is stored with `#=`.
3. An owned local stored into a field or slot that outlives it is stored with
   `#=`.
4. A value read out of a `T` or `#T` formal, or out of an owned local, is never
   stored into a field with `=`. For a formal, spell it `^T` so the caller
   keeps the root alive. For either, `#=` takes the title the slot held.
5. Strings follow every rule here, like every other type. `=` into a String
   field is a borrow.
6. `=` never moves. There is no last-use move: storing an owned local is
   rule 3, spelled `#=`.
7. A producer written in place takes the title with `=`: a literal, a
   primitive, `null`, a `heap T(...)`, a call, or a String `+`. A local
   binding `T b = a;` lends, and an argument `f(a)` lends.
8. A local holder that escapes (returned, stored, or passed with `#`) is a
   field for these rules. A holder that stays local is exempt.

Principle (Julian 2026-10-05): where a dangling reference can be detected at
compile time, detect it. Not everything can be. What matters most is that one
spelling always means one thing.

### 1.3 Non-goals

- The lifetime of a real lend. When a caller lends `x` to a holder that
  outlives `x`, the holder dangles. That needs lifetime information the
  language does not have, and it is not solved here.
- A runtime check. Julian ruled it out. Every check in this spec is static.
- `^T` anywhere else. It already exists on returns and is added on formals
  only. It is not a type, and it does not appear on locals or fields.
- Aliasing a sibling field (`this.a = this.b`) and then replacing `b`. The
  alias is a legal borrow. The replacement frees what it points at, and the
  compiler does not track that (Julian 2026-10-05, card 7).

## 2. Borrow-only formals

### 2.1 Requirements

A formal may be spelled `^T`. The callee receives a borrow on every call. A
call site may pass a name, a field read or another borrow. It may not pass a
transfer or an owned temporary. An override's `^` marks must match the method
it overrides. A `^T` formal carries no runtime ownership flag, since its mode
is known. A varargs parameter may not be `^`: the call site packs a fresh
array for it, so nothing outlives the call to be borrowed from.

### 2.2 Use cases

- **2.2.1** When a method declares `void keep(^Block b)` and a caller writes
  `h.keep(blk)`, the call compiles and `b` borrows `blk`.
- **2.2.2** When a caller writes `h.keep(#blk)` against a `^Block` formal, the
  call is a compile-time error that names the formal and says it is borrowed.
- **2.2.3** When a caller writes `h.keep(heap Block())` against a `^Block`
  formal, the call is a compile-time error, because the temporary has no
  owner to outlive the call.
- **2.2.4** When a `^T` formal is moved inside the callee (`return #b`,
  `other(#b)`), the move is CAJETA_ERROR_MOVE_OF_BORROW.
- **2.2.5** When a `^T` formal is passed on as a plain argument or stored with
  `#=`, it is passed or recorded as a borrow.
- **2.2.6** When an override declares `T` where the overridden method declares
  `^T`, or the reverse, the override is a compile-time error.
- **2.2.7** When a call reaches a `^T` formal through an interface or a
  virtual call, the call site is checked against the declared signature.
- **2.2.8** When the formal is function-typed or belongs to a lambda, `^`
  means the same thing.

## 3. Storing a formal into a field or slot

### 3.1 Requirements

| Formal | `this.f = p` | `this.f #= p` |
|---|---|---|
| `^T p` | the field borrows | the field borrows |
| `T p` | error: use `#=`, or spell the formal `^T` | records what arrived |
| `#T p` | error: use `#=` | the field takes the title |

A slot store (`this.items[i] = p`) follows the same table. A store reached
through a nested path (`this.head.prev = p`) follows it too.

### 3.2 Use cases

- **3.2.1** When `void set(^Block b) { this.b = b; }` is compiled, it
  compiles.
- **3.2.2** When `void set(Block b) { this.b = b; }` is compiled, it is a
  compile-time error offering both fixes.
- **3.2.3** When `void set(Block b) { this.b #= b; }` is called with `blk`,
  the field borrows, and called with `#blk`, the field owns and frees it.
- **3.2.4** When `void take(#Block b) { this.b = b; }` is compiled, it is a
  compile-time error offering `#=`.
- **3.2.5** When the formal's type is String, an array, an interface or a
  closure, the same rules apply.
- **3.2.6** When the formal is a primitive or a value type, it may be stored
  with `=`, since it carries no title.

## 4. Storing a local or an interior read

### 4.1 Requirements

A local that may hold a title, stored with `=` into a field or slot that
outlives it, is a compile-time error with `#=` as the fix. A local may hold a
title when it was bound from `heap`, with `#=`, or from a call with a plain
`T` result. A local that is a borrow, for example one bound with `=` from a
`^T` formal or from a field read, may be stored with `=`.

An interior read of a `T` or `#T` formal (`b.child`, `b.items[i]`) stored with `=` into
a field or slot is a compile-time error. The fix is to spell the formal `^T`.

### 4.2 Use cases

- **4.2.1** When `Block l = heap Block(); this.b = l;` is compiled, it is a
  compile-time error offering `#=`.
- **4.2.2** When `Block l = heap Block(); this.b #= l;` runs, the field owns
  the block after the method returns.
- **4.2.3** When `Block l = b; this.b = l;` is compiled with `b` a `^Block`
  formal, it compiles.
- **4.2.4** When `void f(Block b) { this.c = b.child; }` is compiled, it is a
  compile-time error offering `^Block`.
- **4.2.5** When a holder is itself a local that does not escape
  (`Holder h = stack Holder(); h.b = l;`), no error is reported.
- **4.2.6** When a holder local escapes (`Node n = heap Node(); n.c = p;
  return #n;` with `p` a `Cell` formal), the store is a compile-time error
  offering `#=` or `^Cell`.
- **4.2.7** When `Person o = Persons.load(); this.name = o.name;` is compiled,
  it is a compile-time error offering `#=`.
- **4.2.8** When `String w #= s.substring(1, 9); this.v = w;` is compiled with
  `w` used for the last time, it is a compile-time error offering `#=`. No
  hidden move is made.
- **4.2.9** When a holder local declared in an outer block keeps an owned
  local from an inner block with `=` (`Node h = heap Node(); { Cell l = heap
  Cell(1); h.c = l; }`), it is a compile-time error offering `#=`, since the
  holder outlives the local.
- **4.2.10** When a holder is stored into another local holder, it escapes
  only if that holder escapes or is not the frame's own. A holder bound from
  a parameter, a field read or a call is not the frame's own, so a store into
  it is a field store.

## 5. String fields

### 5.1 Requirements

Decided with Julian 2026-10-05 (cards 3, 5, 6): Strings follow every rule in
this spec. The runtime forms of a String stay invisible to the author:

- A `#=` of a window (`substring`, `trim`) gives the field something it owns:
  a copy of a window up to 256 B, a shared stake on the root above that.
- A frame-arena String stays an optimization. Any escape of arena bytes
  copies, at any size.
- A String `+` written in place on the right of `=` takes the title, like
  `heap`.

A plain `=` into a String field records a borrow, as for every other type.
The codegen that turns it into a move or a copy is removed. Code that relied
on that is caught by sections 3 and 4 first.

### 5.2 Use cases

- **5.2.1** When `void f(^String s) { this.v = s; }` runs, the field borrows
  `s` and no copy is made.
- **5.2.2** When `this.v = "lit"` runs, the field borrows the static literal.

## 6. Diagnostics

### 6.1 Requirements

CAJETA_ERROR_CAPTURED_BORROW_PARAM is retired. The new errors are emitted
during the semantic pass, so `cajeta build`, `--lint` and the IDE all report
them. Each message names the store, the formal or local, and the exact
spelling that fixes it. No message suggests `#T` alone, since `#T` with `=`
still dangles.

### 6.2 Use cases

- **6.2.1** When any error in sections 2 to 4 fires, the message names a fix
  that compiles and is correct.
- **6.2.2** When a project is linted, the error appears without a full build.

## 7. Migration and documentation

### 7.1 Requirements

The stdlib and every fleet repository compile under the new rules before the
release that enforces them. Every place that teaches field stores states the
rule in section 1.2: CLAUDE.md, the language skills embedded in the compiler,
the user guide, the tour, the README, the language specification, the
cajeta.dev site, the release notes and the compiler's messages. Every example
in them compiles under the enforcing compiler. The
skill server's search finds a skill by its keywords, so a search for
"ownership" or "borrow" returns the ownership skill, and the server's
instructions route language questions to the language skills.

### 7.2 Use cases

- **7.2.1** When the release that enforces this ships, the stdlib compiles
  with no error from sections 2 to 4.
- **7.2.2** When an agent asks the skill server for "ownership", it gets the
  ownership skill.
- **7.2.3** When a reader looks up how to keep a parameter, every source gives
  the same answer.

## 8. Measurements (v0.34.0, 2026-10-04)

Probes in cajeta-six tmp/u6. Each stored the value inside a method, ran 2000
allocations, and read it back. The plan turns them into tests.

| Shape | Read back |
|---|---|
| `#Cell p; this.c = p` | 2015288503259401176 for 8100 |
| `#Cell p; this.c #= p` | 8100 |
| `String p; this.v = p`, caller `#s` | SIGSEGV |
| `String p; this.v = p`, caller `s` | correct |
| `#String p; this.v = p` | correct, IR identical to `#=` |
| `Cell l = heap ..; this.c = l` | garbage |
| `Cell l = heap ..; this.c #= l` | 8100 |
| `Cell l = plainCall(); this.c = l` | garbage |
| `this.c = plainCall()` | 8100 |
| `{ Cell sp = heap ..; out #= sp; }` | 8100 (transfer survives the block) |
| `{ Cell sp = heap ..; k.keep(sp); }`, keep stores `#=` | garbage (a lend outlived, section 1.3) |

## 9. Open questions

- **9.1** Error code names. Proposed: CAJETA_ERROR_KEEP_NEEDS_SHARP_STORE (3,
  4.1), CAJETA_ERROR_TRANSFER_INTO_BORROW_PARAM (2.2.2, 2.2.3),
  CAJETA_ERROR_INTERIOR_KEEP_NEEDS_BORROW_PARAM (4.1), and
  CAJETA_ERROR_BORROW_MARK_MISMATCH (2.2.6).
- **9.2** DECIDED (Julian 2026-10-04): `^` written on a primitive or
  value-type formal (`^int32 n`) is a compile-time error.
- **9.3** DECIDED (Julian 2026-10-04): `^` never marks a type-parameter
  declaration (`class C<^K>` is an error). A formal whose type is a
  parameter may be spelled `^K key`, as in `void foo(^T param)`. When the
  template is instantiated with a primitive the mark has nothing to act on and
  is ignored there (inferred from 9.2 and 9.3 together, not asked).
- **9.4** DECIDED (Julian 2026-10-04): rule 4 covers `=` only. An interior
  read stored with `#=` records the title the slot held. Background: rule 4 as written rejects an interior read of a `T`/`#T` formal
  stored with `#=`. Ten stdlib sites move element titles out of a consumed
  source that way (`ArrayList(#T[] items)`: `this.data[i] #= items[i]`,
  `appendAll`, BPlusTree splits, ImmutableList, ImmutableSet). Proposed:
  rule 4 applies to `=` only, and an interior `#=` records what the slot
  held.
- **9.5** Whether a store between slots of one formal array
  (`need[j] = need[j + 1]`) is exempt. It permutes the caller's container
  and keeps nothing new.
- **9.6** DECIDED (Julian 2026-10-05, card 1): an escaping holder is a field
  (rule 8, use case 4.2.6). Background: stores into a local holder's field (`Node n = heap Node(); n.x = p;
  return #n;`): 117 in the stdlib. Proposed: a holder that escapes (returned,
  stored, or passed with `#`) counts as a field, and 4.2.5 covers the rest.
- **9.7** DECIDED (Julian 2026-10-04): every documented borrow moves to `^T`
  in Unit 3, all 86 sites. Background: the 47 borrow-intent sites already spelled `#=` (CacheNode's key,
  Channel's slots, the stream and reader constructors) compile under the
  rule but accept a `#x` that turns a borrow slot into an owner. Whether
  they move to `^T` in Unit 3 (agents/field-store-ownership-census.md).
- **9.8** Whether `#=` of an owned window copies a small window. Measured
  2026-10-05: `String w #= s.substring(10, 26); k.v #= w;` moves the window
  with its shared stake, so 16 bytes pin the 64 byte root. Only resolve, run
  for a lent source, applies the copy at 256 B or below. The old `=` path
  resolved every String store, which is why `=` looked like a last-use move.
  Copying on the move adds a runtime call to every owned String `#=`. Decide
  in Unit 6.
