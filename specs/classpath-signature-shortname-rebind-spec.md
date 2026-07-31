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

## 2. Requirements

- 2.1 A classpath ClassSource re-parse must resolve signature names with
  the DECLARING module's package/import context — a name declared in the
  same package as its use site must never bind to another package's class,
  regardless of canonical-map short-name key state.
- 2.2 Repro pin: two archives with same-short-name classes, both entry
  orders.
- 2.3 Related: silent-resolution-diagnostics (the resolver's short-name
  global tier); `CAJETA_DBG_RESOLVE` documents this hazard class.
