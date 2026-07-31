# placeholder-owned-field — compiler crash on an owned field of a not-yet-parsed user class

## 1. Definition (defect)

`--emit=cja` multi-file AOT SIGSEGVs (corrupted-stack jump into non-text
memory) when a class declares an **owned field whose type is a user class
still in archive-placeholder state** at the declaring class's walk, and
initializes it in a constructor (`this.inner #= heap Inner(...)`).
Discovered 2026-07-31 building dev.cajeta.ml 0.3.0-dev: `Lasso`
(`private ElasticNet inner;`) crashed the build. Pre-existing — reproduces
with the pre-table-fit compiler (verified by A/B at commit `8eca6d0a`).

**Order-dependent, hence intermittent-looking:** module parse order in the
AOT loop is `recursive_directory_iterator` order — readdir/inode hash, not
alphabetical. The ml package's physical order puts `Lasso.cajeta` 3rd and
`ElasticNet.cajeta` 18th, so Lasso walks while ElasticNet is a placeholder.
A structurally IDENTICAL pair under different file names (`ProbeWrap`/
`ProbeInner`, different hash order) compiles clean — that is the smoking
gun for order dependence.

Narrowing (all on the same tree):
- either class's `implements Predictor` removed → compiles;
- trivial `fit` body / no static helper / no extra members → still crashes
  (skeleton pair under the real names crashes);
- same skeleton pair under probe names → compiles.

So the minimal ingredients are believed to be: conformer wrapper class,
owned field + ctor `heap` init of a placeholder sibling conformer, wrapper
parsed first. An in-repo deterministic repro should use the JIT
multi-source harness (map order is deterministic) or a forced parse order.

## 2. Requirements

- 2.1 Deterministic in-repo repro (order-controlled), then root cause: the
  suspected paths are the owned-field drop-chain / ctor-overload resolution
  against an unfilled placeholder during the declaring class's codegen.
- 2.2 Fix: same doctrine as table-fit §2 — a placeholder must be
  materialized (or the walk deferred) before code is EMITTED against its
  layout/ctors/drop; at minimum, an unresolvable state must be a loud
  diagnostic, never a corrupted-pointer crash.
- 2.3 Regression pins for both file orders.
- 2.4 Ships with cajeta v0.13.0 (scheduled: cajeta-ml-v2 plan U7 alongside
  `nucleo.sparse`).

## 3. Status: FIXED 2026-07-31 (root cause found via the deterministic pin)

Root cause — a CROSS-MODULE CONSTANT: `synthesizeInterfaceVTables` pushed
raw `Function*` entries (the implementer's methods and its drop thunk) into
the per-(impl, iface) vtable initializer. On the DEFERRED re-synthesis pass
(the interface was a lazy/archive placeholder at the implementer's
declaration walk, so `pendingIfaceVTables` re-runs synthesis during the
codegen fixed-point) those functions live in a DIFFERENT `llvm::Module`
than the vtable's — and once `Linker::linkModules` consumed the donor, the
initializer held a dangling constant. Symptoms explained: the JIT's flaky
verifier crash (the Verifier faulted PRINTING the freed value), the LLJIT
"Symbols not found: __cajeta_<type>_drop" (a second layer: the test harness
also lacked the buildJit backfill/pin pair before its own merge — added),
and the AOT wild jump (the emitted binary called through the dangling
pointer). Order/hash dependence made it look readdir-flaky.

Fix: vtable entries route through `CajetaModule::ensureFunctionInModule`
(local extern decl instead of a foreign pointer), plus the harness
backfill/pin parity, plus two defensive placeholder-at-use guards
(resolveMethod / ClassCreatorRest materialize-or-fail-loud). Pins LIVE:
`PlaceholderOwnedFieldTests.{ownedFieldOfLaterParsedClass,
nullIntoOwnedInterfaceFormal}`; the AOT split-pair shape builds 3/3; ASAN
2/2.

## 4. Workaround (still in place downstream until the fix RELEASES)

Co-locate the wrapper AFTER its inner class in one compilation unit.
`dev.cajeta.ml/ElasticNet.cajeta` keeps Lasso co-located until the ml
toolchain pin reaches a release carrying this fix (v0.13.0) — CI builds on
the released .deb, not this tree.
