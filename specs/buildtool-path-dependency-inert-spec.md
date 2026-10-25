# buildtool-path-dependency-inert — a `{ "path": ".." }` dependency parses but resolves nothing (defect spec)

> Status: **draft**, filed 2026-10-25 by benchmark-reference unit 17.

## 1. Definition
On v0.46.3 a `cajeta.json` dependency written as `{ "path": ".." }` (the
checkout one directory up, the natural shape for a `bench/` or `test/`
project inside a library's repo) is accepted by the manifest parser and
resolves to nothing: the build proceeds as if the dependency were absent
and fails on the first import. Both `cajeta-ml/bench` and
`cajeta-xgboost/bench` therefore depend on the released archives from Olla
(0.10.6 and 0.7.0), which means a bench cannot measure the working tree.

## 2. Use cases
- **2.1** As a library author, when my `bench/` or sample project declares
  `{ "path": ".." }`, then the build compiles the parent checkout and puts
  its archive on the classpath, and a `cajeta.lock` records it as a path
  dependency, not a version.
- **2.2** As the build tool, when a path dependency does not contain a
  `cajeta.json`, then I fail by name instead of proceeding without it.
- **2.3** As the test suite, then a build-tool test covers both.

## 3. Acceptance
`cajeta-ml/bench` depends on `{ "path": ".." }` and measures the checkout;
the published run records the library version from the parent manifest.
