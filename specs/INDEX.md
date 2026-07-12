# Active work

One row per in-flight spec. Rows are added when a spec is created (`draft`),
flipped to `active` on approval, and removed when the plan closes (spec →
`archive/`, plan → `agents/archive/`). See `td-project-workflow.md`.
`research-platform-roadmap-spec.md` holds the long-horizon view.

| Spec | Plan | Status |
|------|------|--------|
| [external-debug](external-debug-spec.md) | [plan](../agents/external-debug-plan.md) | active — Units 1–6 delivered and green; one item (5.1.7 cycle-render test) is untestable, awaiting a call on whether to close |
| [optional-borrow-ownership](optional-borrow-ownership-spec.md) | [plan](../agents/cajeta/optional-borrow-ownership-plan.md) | active |
| [docs-refactor](docs-refactor-spec.md) | [plan](../agents/docs-refactor-plan.md) | active |
| [cja-source-view](cja-source-view-spec.md) | [plan](../agents/cja-source-view-plan.md) | active |
| [embedded-targets](embedded-targets-spec.md) | [plan](../agents/embedded-targets-plan.md) | active |
| [memory-viewer](memory-viewer-spec.md) | [plan](../agents/memory-viewer-plan.md) | active |
| [diagnostic-engine](diagnostic-engine-spec.md) | — (engine + lint collect-and-continue landed; remaining scope unverified) | active |
| [element-ownership](element-ownership-spec.md) | [plan](../agents/element-ownership-plan.md) | blocked — type-argument layer superseded by title-tracking (Units 1–7 shipped; Unit 8 will not proceed as written) |
| [title-tracking](title-tracking-spec.md) | [plan](../agents/title-tracking-plan.md) | active |
| [cajetadoc-model-fidelity](cajetadoc-model-fidelity-spec.md) | — (docs-refactor 15.1) | draft |
| [stack-return-transfer-error](stack-return-transfer-error-spec.md) | — (docs-refactor 15.4) | draft |
| [net-server-shutdown-wake](net-server-shutdown-wake-spec.md) | — (docs-refactor 15.6) | draft |
| [matrix-element-callarg](matrix-element-callarg-spec.md) | — (docs-refactor 15.8) | draft |
| [kernel-device-call-diagnostic](kernel-device-call-diagnostic-spec.md) | — (docs-refactor 15.9) | draft |
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
