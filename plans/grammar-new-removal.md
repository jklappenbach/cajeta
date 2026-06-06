# Removing the `new` allocation keyword (in favor of `heap` / `stack`)

_Small report per request: where `new` lives in the grammar, where it's used,
and how to remove it. Context: the unified-class model (UnifiedClasses.md)
makes `heap` / `stack` the mandatory placement prefixes; `new` is a
transitional holdover the grammar still accepts._

## Where it is in the grammar

**Lexer** — `antlr4/CajetaLexer.g4:85`
```
NEW: 'new';
```

**Parser** — `antlr4/CajetaParser.g4`, all inside the `expression` rule:

| Line | Form | Notes |
|---|---|---|
| `732` | `\| NEW creator` | The main allocation form. |
| `719` | `\| NEW nonWildcardTypeArguments? innerCreator` | Qualified inner-class creation (`obj.new Inner()`) — Java holdover. |
| `782-783` | `typeType '::' (… \| NEW)` / `classType '::' typeArguments? NEW` | `Type::new` constructor method-references. |
| `739-745` | `HEAP (creator \| aggregateInitializer)`, `STACK …`, `SHARED …` | The intended placement prefixes. |

`creator` (`:874`) is the shared sub-rule:
```
creator : nonWildcardTypeArguments createdName classCreatorRest
        | createdName (arrayCreatorRest | classCreatorRest) ;
```
So **`NEW creator` covers BOTH** `new T[n]` (`arrayCreatorRest`) **and**
`new ClassType(args)` (`classCreatorRest`). `HEAP`/`STACK` use the same
`creator`, so they too cover arrays and objects.

**`#new` is not its own rule.** `:753 | REFERENCE expression` lets `#`
(ownership transfer) prefix any expression, so `#new X()` parses as `#`
applied to `(NEW creator)`.

**Compiler** — every prefix lowers through the same node:
`src/cajeta/asn/expression/NewExpression.cpp` (+ `CreatorRest.cpp`), with a
`stackAlloc` flag separating stack from heap. The grammar comment
(`CajetaParser.g4:736-738`) states it outright: *"`heap` is for now a synonym
for `NEW creator`."* **`new X()` and `heap X()` emit identical code.**

## Where it is used

| Surface | `new ClassType<…>(…)` (objects) | `#new` | `new T[…]` (arrays) |
|---|---:|---:|---:|
| `runtime/src` | ~51 | 4 (all in `Cache`/`DnsCache`) | 296 |
| `test/` | ~156 | 0 | many |

> The 4 `#new` sites have now been migrated to `heap` (Cache backing map,
> CacheNode, DnsCache store). Note: this did **not** change behavior — `heap` ≡
> `NEW creator` — and did **not** fix the DnsCache crash, confirming the crash
> is unrelated to the allocation keyword.

## How to remove it

Removing `new` is a **syntax/consistency cleanup, not a behavior change** (the
lowering is shared). Two scopes:

### Option A — drop object `new`, keep `new T[]` arrays (recommended, surgical)
1. **Grammar:** narrow `:732` from `NEW creator` to array-only, e.g.
   `| NEW createdName arrayCreatorRest`. Remove the inner-creator form (`:719`)
   and the `Type::new` method-refs (`:782-783`) — or keep method-refs if still
   wanted.
2. **Compiler:** in `NewExpression`/`CreatorRest`, reject a `classCreatorRest`
   reached via `NEW` (clear diagnostic: "use `heap`/`stack`").
3. **Migrate** ~51 runtime + ~156 test `new ClassType(…)` → `heap`/`stack`
   (mechanical; `heap` is the drop-in for the current default). Arrays
   (`new T[]`, 296 sites) are untouched.

### Option B — remove `new` entirely
As A, plus migrate **all** `new T[]` array sites (296 in runtime + tests) to
`heap T[]` / `stack T[]`. Much larger; only worth it if arrays should also
carry an explicit placement prefix.

### Either way
- Add a deprecation diagnostic first (accept `new`, warn, point at `heap`) so
  the migration can land incrementally before the grammar tightens.
- Regenerate the parser (build re-runs `antlr_target`) and rebuild the embedded
  stdlib after migrating runtime `.cajeta` sites.
