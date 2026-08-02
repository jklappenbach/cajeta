# classpath-signature-shortname-rebind — cross-package short-name capture in classpath re-parse (draft)

## 1. Definition (defect)

When two classpath archives (or an archive and the project) declare
same-SHORT-NAME classes in different packages, the classpath ingest's
ClassSource re-parse can bind a signature's short name to the WRONG
package's class — decided by archive entry order, so builds of identical
sources pass or fail depending on which .cja was built where.

Observed 2026-07-31 on v0.12.1: cajeta-xgboost's test compile with
`dev.cajeta.ml` + its own library on the classpath re-resolved
`Split.crossValScore(Predictor, …)`'s `Predictor` (declared in
`dev.cajeta.ml`, same package as Split) to
`dev.cajeta.xgboost.predict.Predictor` — a candidate list impossible from
the source: `crossValScore(dev.cajeta.xgboost.predict.Predictor, …)`.
The CI-built ml archive's entry order triggered it; a locally built
archive of the same sources did not.

Worked around by renaming the internal class (`TreeWalker`) — also the
right call editorially — but the hazard remains for ANY user whose class
shares a short name with a dependency's.

## 1a. Status update — 2026-08-02, after cajeta v0.14.1

**The nondeterminism is gone; the defect is not.** Read by code inspection,
not yet re-run — the repro in 2.2 is still owed.

v0.14.1 sorted the compiler's source walk (`listModulePaths`,
`prescanSourceRoot`). Archive `ClassSource` entries are staged in `modules`
order, `modules` is built by iterating `modulePaths`, and that list is now
sorted — so **archive entry order is deterministic and alphabetical**, the
same on every machine.

That kills the symptom this spec leads with: "the CI-built ml archive's entry
order triggered it; a locally built archive of the same sources did not" can
no longer happen. Two builds of one commit now produce byte-identical
archives (verified for `dev.cajeta.ml`, matching sha256).

What it does NOT do is satisfy 2.1. Short names still resolve through the
canonical map's global tier rather than the declaring module's package/import
context; the binding is simply now *consistently* right or *consistently*
wrong for a given input, decided by alphabetical order instead of by
whichever machine built the dependency. A latent wrong-package bind is still
reachable — it just no longer moves.

Two consequences for the work:

- **2.2's "both entry orders" is no longer reachable by rebuilding
  elsewhere.** The repro must construct the orders deliberately (name the
  classes so alphabetical order puts the decoy first, then second), rather
  than relying on a CI-vs-local archive difference.
- **Hypothesis for the root cause, untested.** The same shape as the
  lazy-stdlib bug fixed in v0.14.1: resolution running under a module that is
  NOT the declaring one, so `scopePackageOf(module)` and `module->getImports()`
  describe the wrong file. There, `dev.cajeta.ml.zoo.SmallCnn` imported
  `GradTape` and pulled its declaration in — and `GradTape`'s field types then
  resolved with SmallCnn's imports, which named nothing relevant. A classpath
  signature re-parse is the same situation by construction. Worth checking
  before assuming the fix belongs in the short-name tier: if the module
  context is wrong, no amount of tier reordering fixes it.

## 2. Requirements

- 2.1 A classpath ClassSource re-parse must resolve signature names with
  the DECLARING module's package/import context — a name declared in the
  same package as its use site must never bind to another package's class,
  regardless of canonical-map short-name key state.
- 2.2 Repro pin: two archives with same-short-name classes, both entry
  orders.
- 2.3 Related: silent-resolution-diagnostics (the resolver's short-name
  global tier); `CAJETA_DBG_RESOLVE` documents this hazard class.
