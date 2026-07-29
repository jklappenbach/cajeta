# `@Mock` compile-time mock generation — implementation spec

_Branch: `feature/compiler-mocking`. Adds compiler hooks so a mock subclass is
**generated at compile time** (gomock/mockall model), since Cajeta is AOT with no
runtime proxy. The generated mock forwards each call to a runtime engine; the
ergonomic `when/verify/matcher` surface already ships in **cajeta-unit**
(`dev.cajeta.unit`, see its `docs/mockito-aot.md`) — this feature generates the
hand-written-mock subclass that today's users write by hand._

## Goal

```cajeta
@GenerateMock                     // on the target type (class or interface)
public class Gateway { public Coin charge(int64 amount) { ... } public void refund(int64 a) {} }

// elsewhere, in a test:
MockGateway gw = heap MockGateway();                       // <-- generated class
Mock.when(gw.engine, "charge").thenReturn(#(heap Coin(50)));
Gateway g = gw;                                            // is-a Gateway
Assert.that(g.charge(1999).cents).isEqualTo(50);
MockVerify.times(gw.engine, "charge", 1);
```

The compiler generates `Mock<SimpleName>` (`MockGateway`) extending the target,
holding a `dev.cajeta.unit.MockEngine engine`, with a no-arg ctor that inits the
engine and an override of every virtual method that forwards to the engine.

## Why `@GenerateMock` on the target (not field-level `@Mock`) first

Field-level `@Mock Gateway gw;` (Mockito style) additionally needs the field's
type rewritten to the generated mock and the field auto-initialized in the
enclosing class's constructor. **Auto-init is the hard part** — inline field
initializers do not run today, so it requires injecting `this.gw = heap
MockGateway()` into the enclosing no-arg ctor. That's a separable, trickier layer.
`@GenerateMock` on the target generates a **user-nameable public class** the user
constructs normally — no field rewrite, no ctor injection — exactly mirroring how
`@Builder` generates a related class. Field-level `@Mock` is the Phase-2 sugar on
top.

## Generated class shape

For target `T` (package `p`, simple name `S`), generate `p.MockS`:
- `extends T` (class) or `implements T` (interface).
- field `public MockEngine engine;` (`dev.cajeta.unit.MockEngine`, resolved from
  the classpath — `@GenerateMock` requires `dev.cajeta.unit` on the classpath).
- no-arg ctor: `this.engine = heap MockEngine();`.
- for each **virtual, non-static, non-final, non-constructor** method `m(p1..pn)
  -> R` of `T` (walking the inheritance chain, most-derived wins): an override
  that
  1. boxes each argument into an `Object` (reference args pass through; primitives
     box via the stdlib box factory, e.g. `Int64.of`),
  2. builds an `Object[]` local of those boxes,
  3. calls `R' = this.engine.handle("m", args)` (transferring the array),
  4. if `R` is `void`: ignore `R'`; else downcast `R'` to `R` and return it
     **inline** (binding to an owned local would free the stub value — see the
     cajeta-unit ownership notes).

## Compiler extension points (all grounded in existing synthesis machinery)

1. **Annotation detection** — `@GenerateMock` is recognized bare (canonical
   `code.GenerateMock`, like `@Test`), matched by `findAnnotation("GenerateMock")`
   on the target `CajetaClass`. No stdlib annotation declaration required.

2. **`CajetaClass::synthesizeMock()`** (new, in `type/CajetaClass.cpp`; declared in
   `CajetaClass.h`) — mirrors `synthesizeBuilder()`
   (`type/CajetaClass.cpp:1980`). Called from `generatePrototype()` next to
   `synthesizeBuilder()` (`type/CajetaClass.cpp:1226`). It:
   - returns early unless `findAnnotation("GenerateMock")`,
   - resolves `dev.cajeta.unit.MockEngine` from `CajetaType::getCanonicalMap()`
     (error if absent),
   - builds `QualifiedName{typeName: "Mock"+S, package: p}`,
   - `make_shared<CajetaClass>(module, qn, extends={T}, implements={})`,
   - registers in `getCanonicalMap()` + `module->getStructures()` +
     `getStructureToModule()`,
   - adds the `engine` property (`addProperty`),
   - adds a `SynthesizedMockConstructorMethod` (no-arg, inits engine) — or reuse
     `SynthesizedConstructorMethod` + a small engine-init step,
   - for each virtual method of `T`, adds a `SynthesizedMockMethod`,
   - `mock->generatePrototype()`.

3. **`SynthesizedMockMethod : public Method`** (new `method/SynthesizedMockMethod.*`)
   — like `SynthesizedToStringMethod`. Ctor takes `(module, mockClass,
   targetMethod, engineField, engineHandleMethod)`. `generateCode()` emits the
   forwarding IR using the documented primitives:
   - read `this.engine`: `CreateStructGEP(mockStructTy, this, engineIdx)` +
     `CreateLoad` (field-read pattern, `SynthesizedToStringMethod.cpp:335`),
   - build `Object[]`: alloc via `__cajeta_alloc`, set count slot, store each
     boxed arg (array pattern, `SynthesizedBuilderMethods.cpp:98`),
   - box a primitive: `CreateCall` to the box type's static `of` (`Int64.of`,
     etc.) resolved from the canonical map,
   - method-name `String` constant: build a `cajeta.lang.String` constant the same
     way string literals lower (reuse the literal-lowering helper),
   - call `engine.handle`: resolve `MockEngine`'s `handle` `Method`, get its
     `llvm::Function*` via `CajetaModule::ensureFunctionInModule`, `CreateCall`
     with `{enginePtr, nameConst, arrayPtr}`,
   - return: `void` → `CreateRetVoid`; reference `R` → `CreateBitCast` + `CreateRet`;
     primitive `R` → unbox (call `((Box)res).value()`).

## Constraints the generated IR must honour (from cajeta-unit's probing)

These are why a *naive* generator miscompiles — encode them:
- box primitives with stdlib `Box.of`; build the `Object[]` as a local.
- `#`-transfer the array into `handle` (the engine owns the recorded args).
- return `handle(...)` inline — never via an owned local (it would free the stub's
  value before the next call).
- never `instanceof`; init fields in the ctor (no inline initializers).

## Forward-reference ordering (solved in M1)

Class processing is **entry-driven**: the entry class is resolved before the
`@GenerateMock` target is visited, so registering `MockS` in the target's
`visitClassDeclaration` is too late — a reference like `heap MockGateway()`
aborts with `unresolved type` first. Fix (mirrors how source forward refs work):
- the **prescan** (`Compiler.cpp` `ArchivePrescanVisitor::visitClassDeclaration`)
  registers the `MockS` name in the archive registry when it sees `@GenerateMock`,
  so a forward reference resolves to a **placeholder** `CajetaClass`;
- `synthesizeMock` then **fills that same placeholder** via
  `fillFromDeclaration(module, MockS, extends={T}, {})` (clears the placeholder
  flag, sets the superclass) + adds the ctor + `generatePrototype`. Because
  `fillFromDeclaration` mutates the shared_ptr the reference already bound to, the
  reference stays valid.

## Staged plan

- [x] **M1** — recognize `@GenerateMock`; generate `MockS extends T` with a no-arg
      ctor; register so `heap MockS()` resolves and is-a `T`. Forward-reference
      ordering solved (prescan archive + placeholder fill). Validated: `heap
      MockGateway(); Gateway g = m; g.charge(7)` → 7 (inherited; forwarding is M3).
- [x] **M2** — `engine` field + ctor init, via **source generation**: the mock
      body is emitted as Cajeta source and re-parsed into the class (reusing the
      front-end instead of hand-writing IR — `method/SynthesizedMockClass.cpp`).
      Validated: generated `MockMailer.engine` is a real `dev.cajeta.unit.MockEngine`
      and `Mock.when(m.engine, ...)` compiles.

      > **Architecture note:** M2/M3 generate the mock as **Cajeta source** and
      > re-parse it (the same mechanism templates use — `visitClassBody` with the
      > structure stack rooted at the mock), NOT hand-written LLVM IR. This reuses
      > all of type resolution, boxing, ownership, and codegen, and naturally
      > references cajeta-unit's `MockEngine` (resolved from the classpath). The
      > original IR-per-signature plan in the extension-points section is
      > superseded by this.

- [x] **M3** — forwarding overrides for **reference-param, void/reference-return**
      methods (no boxing). Each override boxes args into an `Object[]`, calls
      `engine.handle(name, #a)`, and returns the answer inline. Methods touching
      primitives are skipped (left inheriting the real impl) until M4. Validated
      end-to-end against cajeta-unit: a generated `MockMailer` stubs (`send ->
      "stubbed"`), verifies (`times`/`once`), and captures args.
- [x] **M4** — primitive arg boxing (`<Box>.of(p)`) + primitive return unboxing
      (`((<Box>) handle(...)).value()`) in the generated source, for all scalar
      primitives with a `cajeta.lang` box (int8..int64, uint8..uint64,
      float16..float128, boolean, char; int128/uint128 skipped). Validated: a
      generated mock stubs+forwards `int64 charge(int64)` (in 99 → out 4242) and
      `boolean isOpen()` (→ false). NB: capturing a boxed arg must be done inline
      (`((Int64) engine.lastArgOf(...)).value()`) — binding it to an owned local
      frees the engine-owned arg (the cajeta-unit ownership footgun, not an M4
      bug).
- [ ] **M5** — interface targets (`implements`); inherited-method walk; final/static
      filtering; method-name collisions.
- [x] **M6** — field-level `@Mock Gateway gw;` (`CajetaClass::synthesizeMockFields`,
      run before the ctor synthesizers): for each `@Mock` field, rewrite its type
      to `Mock<T>` (creating the placeholder if the target hasn't been processed)
      and synthesize a no-arg ctor that auto-inits it (`this.gw = heap MockGateway()`).
      The init must live in an **explicit** ctor — the synthesized *default* ctor
      does not run field initializers. Adding the ctor before `ensureDefaultConstructor`
      makes it the class's no-arg ctor. Registration goes through `setClassBody`
      (the walk alone doesn't register the parsed member). Member resolution of
      `gw.engine` happens at codegen, so the type rewrite at `generatePrototype`
      is in time. Validated: `@Mock Gateway gw;` → `gw` auto-inited (non-null),
      `gw.engine` usable with `Mock.when`/`MockVerify`. Requires `@GenerateMock` on
      the target (so `Mock<T>` gets filled). Caveat: consuming a stubbed return
      must avoid binding it to an owned local (the cajeta-unit footgun) — same as
      any AoT mock use.

## Test fixture

A throwaway project under the compiler's test tree (or cajeta-unit selftest with
`@GenerateMock` on `Gateway`) that builds with cajeta-unit on the classpath and
exercises stub + verify through the generated `MockGateway`.
