# Runtime `@Inject` override hook (test-only DI substitution)

`@Inject` resolution is normally fully static. The compiler picks a provider
and emits a direct call to that provider's synthesized `__cajeta_inject()`. This
makes production injection zero-overhead, and it leaves no way for a test to
substitute a mock or fake for an injected dependency, because there is no
runtime seam.

This hook adds that seam, only in test builds, so a test framework such as
cajeta-unit's `TestContext` can bind a substitute for a type and have any
`@Inject` site of that type resolve to it.

## Design

- **Gated on `--profile=test`.** The override check is emitted in
  `ComponentInjectMethod::generateCode()` only when
  `CajetaModule::getActiveProfile() == "test"`. Production builds emit exactly
  the old static path. There is no lookup, and the registry is not even linked
  unless it is used. This keeps production injection zero-cost and embedded-lean.

- **Keyed by the type's `reflect.Class` object, by pointer identity.** At each
  overridable `@Inject` site the compiler references the field type's
  `<type>#ClassObject` global, the same named, linker-unified global that
  `T.class` lowers to. The runtime registry is keyed by that pointer, so a test
  binding `Foo.class` and the codegen check for a `Foo`-typed field meet at the
  same address. There is no string compare.

- **Emitted shape** (per overridable field, in test builds):

  ```
  depPtr = <static provider path>                       // unchanged
  ovr    = __cajeta_inject_override_get(&Foo#ClassObject)
  depPtr = (ovr != null) ? ovr : depPtr                 // llvm select
  <store depPtr into the field slot>
  ```

  This is a `select` rather than a branch, so the static path still runs when
  overridden. For a singleton that just constructs the cached, shared real
  instance once and ignores it, which is cheap and side-effect-free.

## Runtime registry

Three C helpers in `runtime/native/cajeta_runtime.c`, guarded by a mutex,
hold borrowed pointers. The test owns the substitutes, and `clear` forgets
them without freeing them:

| Symbol | Purpose |
|---|---|
| `__cajeta_inject_override_bind(void* classObj, void* instance)` | bind / replace a substitute for a type |
| `__cajeta_inject_override_get(void* classObj)` | lookup, NULL if unbound |
| `__cajeta_inject_override_clear(void)` | drop all bindings |

cajeta-unit's `org.cajeta.unit.TestContext` is the cajeta-level front door:
`TestContext.bind(Foo.class, mock)` / `TestContext.clear()`.

## v1 scope & limitations

- **Singleton-mode, class-typed fields only.** Mock a type by subclassing it
  and overriding its virtual methods. The substituted pointer dispatches through
  the subclass vtable. Verified end-to-end (cajeta-unit's
  `selftest/inject/Service`).
- **Interface-typed `@Inject` fields are not yet overridable.** An interface
  slot is a 24-byte fat pointer (data + vtable + kind). A correct override must
  supply the mock's interface vtable, which needs a fat-pointer-aware registry.
  This is deferred.
- **Owner and Transient sites, scoped components and `Scoped<T>` handles** are
  not overridden (only Singleton sites). To substitute a scoped component, a
  test provides the substitute inside the scope with `Components.provide(#fake)`
  before the first injection.
- **Ownership:** the registry borrows. A substituted singleton field is itself
  borrow-semantics, since the holder does not drop singletons, so there is no
  double-free. The test must keep its mock alive for as long as the injected
  graph uses it.

## Why this lives in the compiler, not a library

`@Inject` is the language's neutral DI point. Making its resolution
runtime-overridable lets any framework or test harness intercept it, and
primavera's container can use the same hook. The alternative, a parallel
library-level service locator, would not override `@Inject`-annotated code at
all. See `docs/specification/lang/AspectModel.md`. The DI substrate is core,
and primavera owns the policy layer.
