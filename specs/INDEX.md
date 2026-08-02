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
| [lint-server](lint-server-spec.md) | [plan](../agents/lint-server-plan.md) | active — Units 1–4 code delivered and green (reuse parity; NDJSON `--lint-server` daemon; warm sibling reuse; plugin LintServerCore/Client + routeLint + settings toggle, 885d200d); remaining 4.3.1/4.3.2 live per-edit latency + recorded numbers, batched with the plugin reinstall |
| [run-config-ergonomics](run-config-ergonomics-spec.md) | [plan](../agents/run-config-ergonomics-plan.md) | active — spec approved 2026-07-18; plan awaiting approval; Units 1-6 unstarted |
| [fast-debug-launch](fast-debug-launch-spec.md) | [plan](../agents/fast-debug-launch-plan.md) | active — Units 1-7 delivered 2026-07-21, tour warm launch 42.6s→0.45s; edit loop spun off to resident-debug-server; remaining: live passes 2.3.1/10.3.x |
| [resident-debug-server](resident-debug-server-spec.md) | [plan](../agents/resident-debug-server-plan.md) | active — Units 1-5+8 delivered 2026-07-21 (tour via one resident server: cold 41.3s / no-edit 0.65s / one-edit 34.8s); next: Unit 6 design review (stdlib digest churn first), Unit 7 live pass |
| [debugger-variable-inspection](debugger-variable-inspection-spec.md) | [plan](../agents/debugger-variable-inspection-plan.md) | active — Units 1–7 server-side delivered and green (leaf/String, array, object fields, DAP expansion, edit-through-refs, evaluate/hover, ArrayList+HashMap logical views; 912b5c57…e42a3285); only plugin-side items remain (4.2.5/4.3.3 page-size + presentation, 6.1.5/6.2.2/6.3.1 plugin evaluator + live hover) — all batched with the plugin reinstall; warm inspection unblocked (debug-type-sidecar closed 2026-07-28) |
| [cajeta-xgboost](cajeta-xgboost-spec.md) | [plan](../agents/cajeta-xgboost-plan.md) | active |
| [json-viewer](json-viewer-spec.md) | [plan](../agents/json-viewer-plan.md) | active — Units 1-4 delivered 2026-07-28 (doc model + lenient/mixed parsing; in-place console JSON view; diagnostic row navigation; .json/.jsonc structured editing), plugin installed to CLion2026.2; remaining: live passes 2.3.x/3.3.1/4.3.2 (Unit 6), Unit 5 binary JSON gated on Julian confirming the format |
| [silent-resolution-diagnostics](silent-resolution-diagnostics-spec.md) | [plan](../agents/silent-resolution-diagnostics-plan.md) | active |
| [docs-refactor](docs-refactor-spec.md) | [plan](../agents/docs-refactor-plan.md) | active |
| [cja-source-view](cja-source-view-spec.md) | [plan](../agents/cja-source-view-plan.md) | active |
| [embedded-targets](embedded-targets-spec.md) | [plan](../agents/embedded-targets-plan.md) | active |
| [memory-viewer](memory-viewer-spec.md) | [plan](../agents/memory-viewer-plan.md) | active |
| [diagnostic-engine](diagnostic-engine-spec.md) | — (engine + lint + full-compile collect-and-continue landed 2026-07-29; codegen-in-collect-mode is the open follow-up; remaining scope unverified) | active |
| [compile-cache](compile-cache-spec.md) | [plan](../agents/cajeta/compile-cache-plan.md) | blocked (parked after Unit 2, 2026-07-10; re-open trigger HIT 2026-07-31: plan D1 — warm-cache build drops newly-required template instantiations, link failure; cold build fine) |
| [cajetadoc-model-fidelity](cajetadoc-model-fidelity-spec.md) | — (docs-refactor 15.1) | draft |
| [net-server-shutdown-wake](net-server-shutdown-wake-spec.md) | — (docs-refactor 15.6) | draft |
| [matrix-element-callarg](matrix-element-callarg-spec.md) | — (docs-refactor 15.8) | — (docs-refactor 15.8) **PARTIAL 2026-08-01:** the plain-array extension (`f(arr[i])`) now works; the original `f(m[1][1])` Matrix case still SIGSEGVs. |
| [kernel-device-call-diagnostic](kernel-device-call-diagnostic-spec.md) | — (docs-refactor 15.9) | draft |
| [channel-ownership](channel-ownership-spec.md) | [plan](../agents/channel-ownership-plan.md) | draft |
| [view-element-arrays](view-element-arrays-spec.md) | plan: `agents/view-element-arrays-plan.md` | active (unblocks cajeta-gossip G1) |
| [profile](profile-spec.md) | — | draft |
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
| [json-tobytes-string](json-tobytes-string-spec.md) | — (defect, tour-quality stdlib review) | — (defect) **CONFIRMED 2026-08-01 on 0.14.0:** String-field round trip still SIGSEGVs (fault addr nil) after `toBytes` returns 60 bytes. |
| [float64-tostring-roundtrip](float64-tostring-roundtrip-spec.md) | — (defect, tour-quality logging review) | — (defect) **PARTIAL 2026-08-01:** the pinned `0.987` case now renders correctly, but rendering is still not shortest-round-trip — `1.0/3.0` prints `0.333333`, which parses back to different bits. Spec 2.1 unmet. |
| [classpath-diag-duplication](classpath-diag-duplication-spec.md) | — (defect, cosmetic) | — (defect, cosmetic) **CONFIRMED 2026-08-01 on 0.14.0:** still doubled, second line has an empty subject. |
| [classpath-signature-shortname-rebind](classpath-signature-shortname-rebind-spec.md) | — (defect; archive-entry-order-dependent wrong-package binding; workaround: xgboost TreeWalker rename) | draft |
| [owned-interface-return-fault](owned-interface-return-fault-spec.md) | — (defect, SIGSEGV; a `#<Interface>` return faults on first use once stored in a container field — container/index/ownership all ruled out in the spec; workaround: build inline or return the concrete type) | draft |
| [typeparam-cast-of-paren](archive/typeparam-cast-of-paren-spec.md) | — (defect) | FIXED 2026-08-02 (5a3ba7fb): `(Name)(expr)` now lowers as a cast. Reinterpreted at Expression::fromContext, not in the grammar, so cast precedence is untouched — `(T) a.b()` never parses as a postfix call and never reaches the new arm. Shape checked before resolution, so kernel.launch(d)(a) is excluded and a non-type name still falls through to a call. Documented split: `(D)(x).f()` binds cast-first for a type name, whole-chain for a primitive. |
| [linkedlist-class-pop](linkedlist-class-pop-spec.md) | — (defect) | **RE-SCOPED 2026-08-02: String ONLY, pop only** — not "any class-typed T". A user class pops fine surrendered, lent, and even when it OWNS AN ARRAY (so a spurious drop would be observable) — under a poisoned allocator too. String popped returns size 0 / garbage; String read via tail() is fine. Suspect String's tagged inline/heap representation, not early node drop. 6 bounding tests green + 5 DISABLED_ acceptance tests in test/collections/LinkedListClassPopTests.cpp. |
| [runtime-lost-wakeup-under-load](runtime-lost-wakeup-under-load-spec.md) | — (defect) | FIXED 2026-07-31 (18a78057 + 7d8cfd8c): closing a descriptor neither woke nor serialized against the fibers parked on it in the reactor — both interleavings fixed; http tour 0/6 -> 40/40. Open only for acceptance 2.2's >=100-run loop |
| [field-store-title-trap](archive/field-store-title-trap-spec.md) | — (NOT a defect) | CLOSED 2026-08-02: the compiler is correct. `heap X(...)` at a call site surrenders (the formal owns); `=` is a borrow and never inherits that contract, so the formal drops at return and the field dangles — the program asked to borrow from a value whose owner dies at the end of the call. Fix is `#=` at the STORE, formal stays plain (`#T` is a stronger, API-visible choice, not the remedy — the shipped language-ownership skill said otherwise and was corrected). Semantic proposal withdrawn. |
| [uniform-transfer-semantics](uniform-transfer-semantics-spec.md) | [plan](../agents/uniform-transfer-semantics-plan.md) | **active — plan written 2026-08-02, Unit 1 in progress.** A field should behave like a variable: `v #= obj.field.v` takes ownership, so `#` on the right of `#=` is redundant everywhere and `x #= #y` becomes a blanket error. Requires containers to OWN their elements. API-BREAKING, cannot be staged — the diagnostic and the container change land as ONE commit. Scope RE-MEASURED 2026-08-02 and ~5x the spec's §3 estimate: 20 fused claims, 11 element formals -> `#T`, 147 bare-identifier `.cajeta` call sites (124 of them in the stdlib), 73 embedded C++ fixture sites, plus ml + xgboost. Two rulings folded in: primitives are ALREADY exempt (no carve-out to write); String PARTICIPATES, so `list.add(s)` becomes `list.add(#s)` — the largest user-visible change in 0.15.0. |
