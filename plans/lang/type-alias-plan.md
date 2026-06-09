# Transparent type aliases (`alias Name = Type;`)

A name-resolution-layer feature that lets a long type spelling be referred to by a
short name: `alias Vec4 = Vector<float32, 4>;` then write `Vec4` anywhere
`Vector<float32, 4>` is legal. The motivation is **verbosity** — the worst
offenders are the GPU value types (`Vector<float32,4>`, `Matrix<float32,2,2>`,
`CooperativeMatrix<float16,16,16,0>`, `Buffer<float32>`), which repeat all over
kernels and the Tour.

This plan deliberately mirrors the **default type-parameter** increment
(`class Foo<T = float32>`, landed 2026-06-08, `test/parser/DefaultTypeParamTests.cpp`):
the same grammar-arm-plus-regenerate workflow, the same **two parse sites**
(visitor + module-less prescan), the same **store-as-text / resolve-lazily-against
`canonicalMap`** trick, and the same auto-discovered parser test file. Where the
default-param work added a *tail* to an existing rule, aliases add one new
top-level declaration rule and one resolution hook — nothing deeper.

Checkbox legend: `[x]` landed+tested · `[~]` partial (sub-bullets) · `[ ]` not started.
**Working agreement:** one increment at a time, tests + commit checkpoint; never a
miscompile; **commit only when asked**; **no attribution trailer**; regenerate the
parser + rebuild the embedded stdlib after any grammar/runtime `.cajeta` change.

---

## What this IS, and what it is NOT

**IS — a transparent alias.** `alias Vec4 = Vector<float32,4>` makes `Vec4` resolve
to *the same* `CajetaType` identity as `Vector<float32,4>`: interchangeable in both
directions, one cache entry, one canonical name underneath. This is C `typedef` /
C++ `using` / Rust `type` / TS `type`. It is sugar at the name-resolution layer — no
new type, no conversions, no ABI, no codegen, no runtime, no borrow/storage
interaction. `stack Vec4(...)` works because the alias resolves to
`Vector<float32,4>` *before* the storage-class operator (`stack`/`heap`/`#`) applies.

**IS NOT — a nominal newtype.** A `newtype Meters = float32` that creates a
*distinct* type (so `Meters` ≠ `float32` and the checker rejects mixing them) is a
**separate feature** motivated by type-safety, not brevity. It has different
semantics (wrapping/unwrapping, method forwarding policy) and must not be conflated.
Out of scope; revisit independently if wanted.

**IS NOT (v1) — generic / parameterized aliases.** `alias Vec<N> = Vector<float32, N>`
(a type-level function — partial application of the target's params) is where the
real resolution machinery lives. v1 ships **fully-applied, non-generic** aliases
only. The grammar is shaped to admit the generic form later (see A1) without a
breaking change, but A3 rejects a non-empty alias parameter list for now.

**Non-goals (v1):** nominal newtypes; generic aliases; aliasing a *partial* generic
(`alias Buf = Buffer` where `Buffer<T>` still has a free `T`) — rejected with a clear
diagnostic, exactly as a bare default-less template reference degrades today.

---

## Design — mirror `TypeParameter::defaultType`

The default-type-param feature stores the default as **raw text**
(`TypeParameter::defaultType`, `Templates.h:52`), captured at both parse sites, and
resolves it **lazily at instantiation time** against `CajetaType::canonicalMap`
(`TemplateInstantiator.cpp:115`). Forward references across files therefore "just
work" because the archive prescan populates `canonicalMap` before any module is
visited.

Aliases reuse that shape exactly:

- An alias is captured as `{ canonicalName, rhsText }` — `rhsText` is the verbatim
  `typeType` source of the right-hand side (e.g. `"Vector<float32,4>"`). Stored as
  text, **not** a resolved type, so it can name types parsed later or in other files.
- The alias name is registered into the same registries class names use
  (`canonicalMap` + the archive short/canonical maps), so every existing
  type-resolution path finds it with **zero per-call-site change**.
- On first resolution of the alias name (via `CajetaType::of` / `fromContext`), the
  stored `rhsText` is resolved once through the normal `fromContext` path and the
  result is **cached** — and the alias entry returns the *same* `CajetaTypePtr` as
  its target (transparent identity). Subsequent lookups hit the cache.
- Because resolution is transparent, a use site like `Buffer<Vec4>`, a field
  `Vec4 position;`, a param `(Vec4 v)`, a return `Vec4 f()`, and a construction
  `stack Vec4(...)` all resolve through the *same* seam that already resolves
  `Vector<float32,4>` — no new code at the use sites.

Alias → alias chains (`alias A = B; alias B = Vector<...>`) resolve naturally
(lazy resolve of `A` resolves `B`, which resolves the concrete type); a **visited-set
with a depth cap** guards against cycles (`alias A = B; alias B = A;` → clear
`CAJETA_ERROR_ALIAS_CYCLE`).

---

## Increments

### [ ] A1 — Grammar: `ALIAS` token + `aliasDeclaration` rule
Mirror the `viewDeclaration` shape (`CajetaParser.g4:95` —
`VIEW identifier typeParameters? classBody`), but RHS-assigned and statement-terminated.

- **Lexer** `antlr4/CajetaLexer.g4`: add `ALIAS: 'alias';` near the other declaration
  keywords (`CLASS:43`, `VIEW:103`, `HEAP:79`, `STACK:92`). A hard keyword — confirmed
  **no `.cajeta` source uses `alias` as an identifier** (28 mentions, all comments), so
  reserving it breaks nothing.
- **Parser** `antlr4/CajetaParser.g4`:
  - Add the rule (alongside `classDeclaration:82` / `viewDeclaration:95`):
    ```
    aliasDeclaration
        : ALIAS identifier typeParameters? ASSIGN typeType ';'
        ;
    ```
    `typeParameters?` is admitted for forward-compat (generic aliases later) but A3
    rejects a non-empty list in v1. RHS is `typeType` (`:917`) — the same type-reference
    grammar a field/param uses, so `Vector<float32,4>`, `Buffer<float32>`,
    `(int32)->int32` function types, and array types are all valid RHS for free.
  - Add `aliasDeclaration` to the `typeDeclaration` alternation
    (`:50` — `(classDeclaration | viewDeclaration | enumDeclaration | interfaceDeclaration | annotationTypeDeclaration)`)
    so aliases are top-level package members (`classOrInterfaceModifier*` already
    precedes the alternation → `public alias …` works).
  - **Nested/member aliases (optional, v1.1):** also adding `aliasDeclaration` to
    `classBodyDeclaration` would allow class-local aliases; defer unless wanted (keeps
    v1 to top-level, the common verbosity win).
- **IDEA mirror** `ide-plugins/idea/src/main/antlr/CajetaParser.g4` (and its lexer):
  apply the identical token + rule (the file is kept byte-in-sync with `antlr4/`).
- **Regenerate:** the build re-runs `antlr_target` (`src/CMakeLists.txt:200-210`); no
  manual codegen step.
- **Test (parse-only):** a snippet `alias Vec4 = Vector<float32,4>;` at package scope
  parses without error (a hard parse/AST check before any resolution wiring).

### [ ] A2 — Capture + register at BOTH parse sites
Mirror the default-type capture (`CajetaLlvmVisitor.h:238-239` visitor +
`Compiler.cpp:250-251` prescan) and the class-registration path
(`registerAndRecurse` → `CajetaType::registerArchive`, `Compiler.cpp:214`).

- New small carrier `struct TypeAlias { string canonicalName; string rhsText;
  list<TypeParameter> params; }` (params empty in v1) — alongside `TypeParameter`
  in `src/cajeta/type/Templates.h`.
- **Prescan** `ArchivePrescanVisitor::visitAliasDeclaration` (`Compiler.cpp`, sibling
  of `visitClassDeclaration:125`): compose the canonical name from package + enclosing
  nesting (reuse `registerAndRecurse`'s naming), capture `rhsText = ctx->typeType()->getText()`,
  and register the alias name (canonical + short) so cross-file uses that parse
  *before* the alias resolve. Store the `{canonical → rhsText}` alias table on the
  archive next to `registerArchiveTemplate` (`Compiler.cpp:270`).
- **Visitor** `CajetaLlvmVisitor.h::visitAliasDeclaration` (sibling of
  `visitClassDeclaration:79`): same capture for the per-module walk; install the alias
  into `CajetaType::canonicalMap` as a lazily-resolving entry (A3).
- No codegen — an alias declaration emits **nothing**.

### [ ] A3 — Resolution: alias name → target type (the one real hook)
The single behavioral change. `CajetaType::of` / `fromContext`
(`CajetaType.cpp:444` / `:487`) is where a type name string becomes a `CajetaTypePtr`,
falling back to `canonicalMap` (`:479`). Add: when the resolved canonical entry is an
**alias**, resolve (once, cached) its `rhsText` through `fromContext` and return that
target `CajetaTypePtr` — the alias and its target share identity from then on.

- Represent the alias entry so a single `canonicalMap` lookup transparently yields the
  target. Two viable shapes — pick during impl:
  1. **Lazy alias node:** a tiny `CajetaTypeAlias` that, on first `getLlvmType()`/
     identity query, resolves `rhsText` and forwards everything to the target (a
     delegating pointer). Most faithful to "transparent."
  2. **Eager rewrite at registration:** resolve `rhsText` when the alias is first
     *referenced* and **rebind** the `canonicalMap[aliasName]` slot to the target's
     existing `CajetaTypePtr`. Simplest — after the first hit, `Vec4` and
     `Vector<float32,4>` are literally the same map value.
  Prefer (2) for true identity (one shared pointer; `==`/cache/error all natural),
  with a "currently-resolving" sentinel to detect cycles.
- **Cycle / chain guard:** a visited-set + depth cap; `alias A = B; alias B = A;` →
  `CAJETA_ERROR_ALIAS_CYCLE`. Alias-to-alias chains resolve transitively.
- **v1 generic rejection:** if the alias declared `typeParameters` (non-empty) OR the
  resolved target is a template instantiation with **free** parameters (a partial
  generic, e.g. `alias Buf = Buffer`), emit `CAJETA_ERROR_ALIAS_INCOMPLETE` — exactly
  how a bare default-less template reference degrades today, just with a clearer
  message steering to the fully-applied form.
- **Inert otherwise:** the alias branch is only reached when a name hits an alias
  entry; class/interface/primitive resolution is untouched (no regression risk to the
  existing 100+ stdlib structures).

### [ ] A4 — Construction through an alias (`stack Vec4(...)`)
Should fall out of A3 for free — `NewExpression::resolveTypes` (`NewExpression.cpp:100+`)
resolves its type name through the same path, and `defaultedInstantiation`
(`NewExpression.cpp:79-98`) already fills a default-bearing template. Because the alias
resolves to a *fully-applied* `Vector<float32,4>`, construction sees a concrete type
directly. **Add an explicit test** (don't assume): `stack Vec4(1,2,3,4)` and
`heap Vec4(...)` construct identically to `stack Vector<float32,4>(...)`, and
`# Vec4` move-out behaves the same.

### [ ] A5 — Tests: `test/parser/AliasTests.cpp`
Mirror `DefaultTypeParamTests.cpp` exactly — auto-discovered by the
`test/CMakeLists.txt:3` glob; compile a source string via `CajetaJit::compile(src, "test.A")`,
`jit->lookup<int32_t(*)()>("run")`, `EXPECT_EQ`. Cases:
1. **aliasToPrimitive** — `alias I = int32;` then `I x = 7; return x;` → 7.
2. **aliasToGenericInstantiation** — `alias Vec4 = Vector<float32,4>;` construct +
   read a lane back through the alias name; bit-exact vs the spelled-out type.
3. **aliasEqualsTargetIdentity** — a value of type `Vec4` passes to a function
   declared `Vector<float32,4>` (and vice-versa) — same type, accepted (the
   default-param `bareEqualsExplicitDefaultIdentity` analog).
4. **aliasAtFieldParamReturn** — alias used as a field type, a param type, and a
   return type, all resolving.
5. **aliasAsTypeArgument** — `alias Vec4 = …; Buffer<Vec4> b = …;` (alias *inside*
   another generic's argument list).
6. **aliasInConstruction** — `stack`/`heap Vec4(...)` (A4).
7. **aliasChain** — `alias A = B; alias B = int32;` resolves transitively.
8. **crossFileAliasViaImport** — alias declared in one source, used in another via
   `import` (the archive-prescan path; mirrors how cross-file class refs are tested).
9. **Negatives** — unknown RHS type → clear error; duplicate alias name → error;
   `alias A = B; alias B = A;` → `CAJETA_ERROR_ALIAS_CYCLE`; partial-generic RHS
   (`alias Buf = Buffer;`) → `CAJETA_ERROR_ALIAS_INCOMPLETE`.

### [ ] A6 — (deferred) stdlib aliases + generic aliases
- **Optional convenience module** `cajeta.xpu.aliases` (or `cajeta.math`) shipping
  `Vec2/Vec3/Vec4`, `Mat2/Mat3/Mat4`, `Quat`, etc. **Decision pending** (see below) —
  lean *user-defined first*, ship a blessed set only once a naming convention settles,
  to avoid prematurely blessing one. If shipped, rebuild the embedded stdlib.
- **Generic aliases** `alias Vec<N> = Vector<float32, N>` — the type-level-function
  form (the grammar already admits `typeParameters?` from A1). Real resolution work:
  substitute the alias's params into the target before instantiation. A separate
  follow-on; only if demand appears.

---

## Decisions / open questions

- **Spelling — `alias` (recommended).** Unambiguous, no clash with a future
  reflection `type` or with `using`-as-import. Reads as a declaration. (Alternatives:
  `type X = …` (Rust/TS, risks a `type` reflection clash); `using X = …` (C++, reads
  like an import).)
- **Error display:** when a type error involves an alias, show **both** names —
  `Vec4 (= Vector<float32,4>)` — so diagnostics stay legible without hiding the alias.
- **Scope of v1:** top-level (package-member) aliases only; nested/member aliases
  (A1's optional `classBodyDeclaration` arm) deferred.
- **Blessed stdlib aliases:** ship or not (A6) — owner call.

## Risks

- **Keyword reservation:** `alias` becomes a hard keyword. Verified zero identifier
  uses today; mirrors the existing `view` reservation, so the precedent and the parse
  machinery exist.
- **Transparent-identity correctness:** the whole feature's safety rests on the alias
  yielding the *same* `CajetaTypePtr` as its target (A3 shape 2). Get that right and
  every downstream path (cache keys, `==`, codegen, borrow check, error text) is
  automatically correct because they never see an "alias type" — only the target.
- **Forward refs / ordering:** handled by the store-as-text + lazy-resolve pattern the
  default-param feature already proved across files; no new ordering hazard.
- **Cycles:** bounded by the visited-set/depth guard in A3.
- **Regenerated parsers drift:** the `antlr4/` and `ide-plugins/idea/` grammars must
  stay in sync (same discipline the default-param + every prior grammar change
  followed).

## Reference anchors (the seams this mirrors)

| Layer | Default type-param | Alias (mirror) |
|---|---|---|
| Lexer keyword | `CLASS:43` / `VIEW:103` | add `ALIAS` |
| Parser rule | `typeParameter` tail `(ASSIGN typeType)?` `CajetaParser.g4:104` | new `aliasDeclaration`; add to `typeDeclaration:50` |
| Carrier (text) | `TypeParameter::defaultType` `Templates.h:52` | `TypeAlias::rhsText` (new, `Templates.h`) |
| Parse site 1 (visitor) | `CajetaLlvmVisitor.h:238-239` | `visitAliasDeclaration` |
| Parse site 2 (prescan) | `Compiler.cpp:250-251` | `ArchivePrescanVisitor::visitAliasDeclaration` |
| Registry | `CajetaType::canonicalMap` `CajetaType.cpp:28`; `registerArchive` `Compiler.cpp:214` | same |
| Resolution | `fromContext` `CajetaType.cpp:487`; default-fill `TemplateInstantiator.cpp:115` | alias branch in `of`/`fromContext` (A3) |
| Construction | `NewExpression::defaultedInstantiation` `NewExpression.cpp:79-98` | resolves for free; explicit test (A4) |
| Tests | `test/parser/DefaultTypeParamTests.cpp` (glob `test/CMakeLists.txt:3`) | `test/parser/AliasTests.cpp` |

Related: `plans/grammar-new-removal.md` (the `new`→`heap`/`stack` removal, same
grammar-edit+regenerate workflow); `cajeta-docs/.../UnifiedClasses.md`,
`Views.md` (the `view` keyword precedent).
