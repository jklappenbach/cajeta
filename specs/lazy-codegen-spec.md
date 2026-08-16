# Lazy codegen — emit method bodies on demand

## 1. Definition

### 1.1 Purpose
JIT hosts generate LLVM IR for every method they know about before running any
user code. A Jupyter cell computing `20 + 42` emits **12,379 method bodies**,
11,015 of them the stdlib's. This capability makes body emission demand-driven:
a method body is generated when something asks for its symbol, and not before.

### 1.2 The problem, measured
`bench/kernel-startup/FINDINGS.md`, `project-with-deps`, first cell:

    [prime]  stdlib                     ~5.8 s   12%
    [ingest] deps + lazy drain           5.0 s   10%
    [cell]   codegen method bodies      27.4 s   55%
    [cell]   verify + JIT materialize   16.0 s   32%
                                        ------
    cell 1                              ~50   s

87% of a first cell is back-end work on code the cell never calls. Per body the
cost is ordinary (~2.2 ms); there is nothing pathological to optimise. The only
lever is emitting fewer bodies.

### 1.3 Why the eager loop exists
It compensates for a gap, not a requirement. `KernelSession` emits the stdlib
because otherwise "the cell fails to materialize on `cajeta.lang.Object::drop`
and friends" — a symbol-resolution failure. The session installs only
`DynamicLibrarySearchGenerator` (process symbols); **no Cajeta-aware
`orc::DefinitionGenerator` exists**, so an unemitted body is an unresolvable
symbol rather than a request to emit one. A "Symbols not found" error is a bug
report about the lazy path.

### 1.4 Scope
Both JIT hosts — `CajetaKernelSession` and `CajetaJitHost`. They share the
eager fixpoint and can share one generator.

### 1.5 Non-goals
- **AOT (`cajeta build`) stays eager.** Emitting what you link is a
  reachability/DCE problem, not a materialization one, and ORC is not involved.
- Not a change to *how* a body is generated. `generateCode()` is unchanged;
  only *when* it runs moves.
- Not front-end laziness. Lazy stdlib parsing already exists and is separate.

### 1.6 Constraints
- **`generateCode()` is idempotent.** Measured 2026-08-16: emitting the stdlib
  twice per fixpoint iteration costs no measurable time. On-demand calls are
  therefore safe to repeat.
- **The compiler is single-threaded over global state** (`CajetaType::getCanonicalMap()`,
  active module, substitution stack). ORC may call a generator from a
  materialization thread.
- **Emission cascades.** Generating one body can reference further undefined
  symbols and can instantiate templates, adding methods. The existing eager loop
  is a fixpoint for exactly this reason.

## 2. The emission boundary

What may be lazy is decided by reachability, not by preference: a definition can
be generated on demand only if something **looks up its symbol**. Definitions
reached without a lookup have no interception point.

- **2.1** When a definition is reached only through a symbol lookup, it is
  emitted on demand.
- **2.2** When a definition is reached by a dylib-initialisation constructor, it
  is emitted eagerly. `finalizeClassObject()` emits a global ctor calling
  `__cajeta_register_class`; nothing looks that up, so a lazy `#ClassObject`
  would simply never register and reflection would fail silently at runtime —
  a worse failure than "Symbols not found", because it is quiet.
- **2.3** When a class's static initialisers run at dylib init, they are emitted
  eagerly, for the same reason.
- **2.4** Reflective thunks (`__cajeta_<canonical>_reflect_invoke` / `_new`) are
  named symbols referenced from the `#ClassObject` initialiser, so they are
  emitted on demand.
- **2.5** The eager remainder is per-class (~430 classes), not per-method
  (~12,379), so the boundary preserves the bulk of the win.

## 3. On-demand generation

- **3.1** When the JIT cannot resolve a symbol that names a Cajeta method, that
  method's body is generated and delivered, and the lookup succeeds.
- **3.2** When a symbol names no known Cajeta method, resolution falls through
  to the existing generators unchanged, and an genuinely absent symbol still
  fails as it does today.
- **3.3** When a session starts, every known method is indexed by mangled symbol
  name. Indexing replaces emission as the startup cost.
- **3.4** When generating a body references further undefined symbols, those
  resolve through the same path, to any depth.
- **3.5** When generating a body instantiates a template and so defines new
  methods, those methods become resolvable without restarting the session.
- **3.6** When two lookups need the same body, it is generated once; a repeated
  call is harmless but must not produce a duplicate definition.
- **3.7** When the generator runs, it does so under the compiler's
  single-threaded discipline; concurrent materialisation never re-enters the
  compiler in parallel.

## 4. Correctness

- **4.1** When a cell or unit runs under lazy emission, its observable result is
  identical to the same input under eager emission.
- **4.2** When reflection invokes a method that has never been called directly,
  the thunk and the target body are generated and the invocation succeeds.
- **4.3** When a class is redefined in a later cell, lazily generated bodies
  follow the same generation rules as eagerly generated ones and stale bodies
  are not served.
- **4.4** When a drop thunk is needed, it resolves; the passes that assume a
  complete module set — `backfillDropFunctions`, `pinDropFunctionDefinitions`,
  `demoteInstantiationsToWeakODR` — continue to hold, or are restated to hold
  over the lazily materialised set.
- **4.5** When a module is verified, verification covers what has actually been
  emitted; an empty or partial module is not an error.

## 5. Control and diagnosis

- **5.1** When `CAJETA_EAGER_CODEGEN=1` is set, eager emission is restored
  exactly, so any regression can be A/B'd in one run. This is a **permanent
  supported control** (Julian, 2026-08-16), not a migration aid — the question
  "is it the lazy path?" stays answerable for the life of the feature.
- **5.1.1** Because it is permanent, the eager path stays exercised: it is
  covered by tests rather than left to rot. An escape hatch that is never run
  is broken by the time it is needed, and the day it is needed is a day
  something else is already wrong.
- **5.2** When `CAJETA_PRIME_TIMING=1` is set, the number of bodies generated on
  demand and the time spent generating them are reported, so the eager and lazy
  paths are comparable on the same instrument.
- **5.3** When a lookup fails for a method that is indexed but could not be
  generated, the error names the method and the reason, and is distinguishable
  from an ordinary missing symbol.

## 6. Acceptance

- **6.1** When the full battery runs under lazy emission, it is green.
- **6.2** When a trivial cell runs in a session with one dependency, it emits
  far fewer than 12,379 bodies, and the count is reported.
- **6.3** When first-cell latency is measured before and after, the numbers are
  recorded in `bench/kernel-startup/FINDINGS.md` and reviewed. No programmatic
  performance threshold is asserted in a test.
- **6.4** When the 6-line `import cajeta.nucleo.column.DynCol` repro is built,
  it remains correct and is used as the fast iteration loop.
