# record-cross-type-return — defect

Found 2026-08-04 implementing `stdlib-completion` U6 (OKLab records).
**Blocks stdlib-completion U6.**

## 1. Defect

Adding a stdlib method that **returns a record of a type other than its
own class** mis-compiles the ENTIRE stdlib bundle: afterwards, *unrelated*
record code SIGSEGVs at jit codegen — `GfxColorTests` (which touch only
the long-stable `Color`) go 0/5 with a nil-fault. The poison is global to
the bundle, not local to the new method's callers.

Crash site (gdb): `Method::generateCode` → `Scope::putField` →
`ParameterField::getOrCreateAllocation` →
`llvm::Function::getArg(paramIndex)` → `BuildLazyArguments` →
`Value::setName` on garbage — i.e. `paramIndex` out of range for the
function's declared argument list: the prototype the body is generated
against disagrees with the formals. Likely the record-ABI decision
(by-value aggregate vs `ptr`; possibly an sret slot) is made from a
PLACEHOLDER state of the foreign record and never revisited, so the
whole bundle's record machinery is generated against stale signatures.

## 2. Empirical matrix (each row = full rebuild + GfxColorTests)

Healthy (5/5) with, in `cajeta.math`:
- **2.1** a bare `record Oklab` with three `float64` fields + ctor;
- **2.2** + accessor methods named like the fields (`l()`, `a()`, `b()`);
- **2.3** + `static float64 deltaE(Oklab, Oklab)` — record PARAMS are fine;
- **2.4** + `static Oklab fromLch(Oklch p)` — an **own-type** return
  taking a foreign record, reading its public fields;
- **2.5** a method CONSTRUCTING a foreign record internally
  (`stack Oklch(...)`) but returning `float64`.

Poisoned (0/5) by any of:
- **2.6** an INSTANCE method on `record Oklab` returning `Oklch`;
- **2.7** a STATIC on `record Oklab` returning `Oklch`;
- **2.8** a static on a plain **class** (`OklabOps`) returning `Oklch`;
- **2.9** the full bidirectional conversion set
  (`Oklab.fromColor(Color)` + `Color.fromOklab(Oklab)` +
  `Oklch.fromLab(Oklab)` + `Oklab.fromLch(Oklch)`) even with every
  return own-type and every foreign read a FIELD read — so own-type
  returns are safe only up to some cross-reference density; the 2.4
  single edge passed, the full cycle set did not.

The same shapes compile and run CORRECTLY when the records live in the
user package (test source), all files parsed in one unit — the defect is
specific to the stdlib bundle build/ingest path.

## 3. Impact

- `stdlib-completion` U6 (OKLab/OKLCh on `Color`) cannot land: color
  conversions inherently cross record types. The finished implementation
  and its 5-test suite are parked: tests at `test-archive/OklabTests.cpp`,
  implementation preserved in the U6 work session (Oklab/Oklch records +
  `Color.fromOklab`/`inSrgbGamut`; conversions as static factories on the
  destination type — the shape chosen to maximize own-type returns).
- Any future stdlib record graph (docs' document model, chart's geometry
  types) will hit this immediately.

## 4. Notes for the fix

- Compare `Method::generatePrototype`'s ABI decisions for record
  params/returns against what `ParameterField` assumes at body codegen;
  check whether a placeholder-era prototype is cached and reused after
  the record's layout resolves (the placeholder-fill family —
  cf. classpath-signature-shortname-rebind, fixed 2026-08-04).
- A hard error (or assert) when `paramIndex >= arg_size()` would turn
  this from a nil-fault into a diagnosable failure.
- While here: a missing member referenced from a stdlib file (the
  `Math.atan2` gap that preceded this find) also nil-faults instead of
  reporting CAJETA_ERROR_MEMBER_NOT_FOUND when reached via the bundle
  path — same diagnostic-hygiene neighborhood.

## 5. Acceptance

- **5.1** A stdlib record method returning a different record type
  compiles, and the bundle stays healthy (GfxColorTests green).
- **5.2** The full U6 conversion set (2.9) compiles; `OklabTests` moves
  back from `test-archive/` to `test/math/` and passes 5/5.
- **5.3** Records from the bundle and records from user source behave
  identically for every shape in §2.
