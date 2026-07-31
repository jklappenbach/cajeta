# Active work

One row per in-flight spec. Rows are added when a spec is created (`draft`),
flipped to `active` on approval, and removed when the plan closes (spec →
`archive/`, plan → `agents/archive/`). See `td-project-workflow.md`.
`research-platform-roadmap-spec.md` holds the long-horizon view.

| Spec | Plan | Status |
|------|------|--------|
| [external-debug](external-debug-spec.md) | [plan](../agents/external-debug-plan.md) | active — Units 1–6 delivered and green; one item (5.1.7 cycle-render test) is untestable, awaiting a call on whether to close |
| [ide-symbol-index](ide-symbol-index-spec.md) | [plan](../agents/ide-symbol-index-plan.md) | active — Units 1-9 implemented and green; 8.3.1 (dependency Ctrl-click) confirmed live 2026-07-18; remaining: live-IDE acceptance 8.3.2, 8.3.3, 9.3.1, 9.3.2 |
| [ide-features](ide-features-spec.md) | [plan](../agents/ide-features-plan.md) | active — refactoring, hierarchy, call graph, gutter nav; blocked until ide-symbol-index closes |
| [lint-server](lint-server-spec.md) | [plan](../agents/lint-server-plan.md) | active — Units 1–3 delivered and green (reuse parity; NDJSON `--lint-server` daemon; warm sibling-context reuse); Unit 4 (IntelliJ plugin client + lifecycle) next |
| [run-config-ergonomics](run-config-ergonomics-spec.md) | [plan](../agents/run-config-ergonomics-plan.md) | active — spec approved 2026-07-18; plan awaiting approval; Units 1-6 unstarted |
| [fast-debug-launch](fast-debug-launch-spec.md) | [plan](../agents/fast-debug-launch-plan.md) | active — Units 1-7 delivered 2026-07-21, tour warm launch 42.6s→0.45s; edit loop spun off to resident-debug-server; remaining: live passes 2.3.1/10.3.x |
| [resident-debug-server](resident-debug-server-spec.md) | [plan](../agents/resident-debug-server-plan.md) | active — Units 1-5+8 delivered 2026-07-21 (tour via one resident server: cold 41.3s / no-edit 0.65s / one-edit 34.8s); next: Unit 6 design review (stdlib digest churn first), Unit 7 live pass |
| [debugger-variable-inspection](debugger-variable-inspection-spec.md) | [plan](../agents/debugger-variable-inspection-plan.md) | active — spec+plan approved 2026-07-22; 7 units (bridge → arrays → objects → DAP/render → edit → hover → collections); Unit 1 next |
| [cajeta-xgboost](cajeta-xgboost-spec.md) | [plan](../agents/cajeta-xgboost-plan.md) | active |
| [cajeta-ml-v2](cajeta-ml-v2-spec.md) | [plan](../agents/cajeta-ml-v2-plan.md) | active — Lasso/ElasticNet, softmax, Pipeline, nucleo.sparse (v0.13.0), KMeans/kNN/PCA; spec+plan drafted 2026-07-31 |
| [silent-resolution-diagnostics](silent-resolution-diagnostics-spec.md) | [plan](../agents/silent-resolution-diagnostics-plan.md) | active |
| [docs-refactor](docs-refactor-spec.md) | [plan](../agents/docs-refactor-plan.md) | active |
| [cja-source-view](cja-source-view-spec.md) | [plan](../agents/cja-source-view-plan.md) | active |
| [embedded-targets](embedded-targets-spec.md) | [plan](../agents/embedded-targets-plan.md) | active |
| [memory-viewer](memory-viewer-spec.md) | [plan](../agents/memory-viewer-plan.md) | active |
| [diagnostic-engine](diagnostic-engine-spec.md) | — (engine + lint + full-compile collect-and-continue landed 2026-07-29; codegen-in-collect-mode is the open follow-up; remaining scope unverified) | active |
| [compile-cache](compile-cache-spec.md) | [plan](../agents/cajeta/compile-cache-plan.md) | blocked (parked after Unit 2, 2026-07-10; re-open trigger HIT 2026-07-31: plan D1 — warm-cache build drops newly-required template instantiations, link failure; cold build fine) |
| [cajetadoc-model-fidelity](cajetadoc-model-fidelity-spec.md) | — (docs-refactor 15.1) | draft |
| [stack-return-transfer-error](stack-return-transfer-error-spec.md) | — (docs-refactor 15.4) | draft |
| [net-server-shutdown-wake](net-server-shutdown-wake-spec.md) | — (docs-refactor 15.6) | draft |
| [matrix-element-callarg](matrix-element-callarg-spec.md) | — (docs-refactor 15.8) | draft |
| [kernel-device-call-diagnostic](kernel-device-call-diagnostic-spec.md) | — (docs-refactor 15.9) | draft |
| [channel-ownership](channel-ownership-spec.md) | [plan](../agents/channel-ownership-plan.md) | draft |
| [view-element-arrays](view-element-arrays-spec.md) | plan: `agents/view-element-arrays-plan.md` | active (unblocks cajeta-gossip G1) |
| [profile](profile-spec.md) | — | draft |
| [jit-run-parse-abort](jit-run-parse-abort-spec.md) | — | draft |
| [cajeta-ir-phase-b](cajeta-ir-phase-b-spec.md) | — (§4/§5 resolved; §2 forwarding, §3 captures remain) | draft |
| [llm-kernel-scheduling](llm-kernel-scheduling-spec.md) | — | draft |
| [robotics-kernel-scheduling](robotics-kernel-scheduling-spec.md) | — | draft |
| [xpu-scan-primitive](xpu-scan-primitive-spec.md) | — | draft |
| [xpu-pipelined-gemm-primitives](xpu-pipelined-gemm-primitives-spec.md) | — | draft |
| [xpu-kernel-scheduling](xpu-kernel-scheduling-spec.md) | — | draft |
| [xpu-kernel-scheduling-hints](xpu-kernel-scheduling-hints-spec.md) | — (U1 landed on origin: cajeta.xpu.Schedule surface + no-op seam; un-archived 2026-07-03) | active |
| [xpu-gfx-streaming-geometry](xpu-gfx-streaming-geometry-spec.md) | — | draft |
| [xpu-build-ergonomics](xpu-build-ergonomics-spec.md) | — | draft |
| [simd-numeric-kernels](simd-numeric-kernels-spec.md) | — | draft |
| [quaternion-vector-stdlib](quaternion-vector-stdlib-spec.md) | [plan](../agents/quaternion-vector-stdlib-plan.md) | draft |
| [ternary-int-codegen](ternary-int-codegen-spec.md) | — | draft |
| [tour-quality](tour-quality-spec.md) | [plan](../agents/tour-quality-plan.md) | active — Units 1-4 done; Unit 3 residue: nucleo.frame demo [~] on table-fit; Unit 4 (codec) 2026-07-31: tour 11→113 checks, 52/52 coverage, CI green on v0.12.1 (cajeta-codec 80831b1); next: unit 5 http |
| [placeholder-owned-field](placeholder-owned-field-spec.md) | — (defect; FIXED 2026-07-31 — cross-module vtable constant; pins live; ml co-location workaround stays until the fix releases in v0.13.0) | draft |
| [null-owned-interface-arg](null-owned-interface-arg-spec.md) | — (defect; FIXED 2026-07-31, pin live; noted follow-up: fat-aware interface `== null`) | draft |
| [json-tobytes-string](json-tobytes-string-spec.md) | — (defect, tour-quality stdlib review) | draft |
| [float64-tostring-roundtrip](float64-tostring-roundtrip-spec.md) | — (defect, tour-quality logging review) | draft |
| [cross-cja-exception-catch](cross-cja-exception-catch-spec.md) | — (defect, tour-quality unit review) | draft |
| [classpath-diag-duplication](classpath-diag-duplication-spec.md) | — (defect, cosmetic) | draft |
| [classpath-signature-shortname-rebind](classpath-signature-shortname-rebind-spec.md) | — (defect; archive-entry-order-dependent wrong-package binding; workaround: xgboost TreeWalker rename) | draft |
| [buildtool-dependency-classpath](buildtool-dependency-classpath-spec.md) | — (defect/gap, verify first) | draft |
| [linkedlist-class-pop](linkedlist-class-pop-spec.md) | — (defect, tour-quality unit 3; 12-line repro in spec) | draft |
| [runtime-lost-wakeup-under-load](runtime-lost-wakeup-under-load-spec.md) | — (defect, tour-quality unit 5; gdb signature in spec; fiber wakeup lost under CPU contention) | draft |
| [compiler-mcp](compiler-mcp-spec.md) | [plan](../agents/compiler-mcp-plan.md) | active — spec approved 2026-07-31; Units 1-8 unstarted |
