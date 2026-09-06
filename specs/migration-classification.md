# docs/specs migration classification — review before moves (docs-refactor 5.2.1)

68 files. Verdicts: **schema** → `specs/schemas/` · **done** → `specs/archive/`
(fully-checked plans also archive to `agents/archive/`) · **active** / **spec-only**
→ `specs/` with INDEX rows · **other** → see notes.

## schema (8)
| file | evidence |
|---|---|
| action-catalog-v1.json | contract JSON |
| capabilities-v1.json | contract JSON |
| lockfile-v1.json | contract JSON |
| manifest-v1.json | contract JSON |
| extension-api-v1.md | versioned API contract |
| repository-protocol-v1.md | versioned protocol contract |
| schema-versioning.md | versioning policy |
| toolchain-registry-v1.md | versioned registry contract |

## done (46) — archive
| file | evidence |
|---|---|
| buildtool-widget-spec.md | plan 47/0 checked |
| cajeta-mcp-spec.md | plan 33/0; tools/mcp/ shipped |
| cajeta-process-spec.md | plan 36/0; runtime process/ shipped |
| cajeta-search-spec.md | plan 25/0; runtime search/ shipped |
| carrier-quiesce-spec.md | plan 21/0 checked |
| skill-discovery-spec.md | plan 113/0; samples/skill-discovery/ |
| yaml-frontmatter-spec.md | plan 38/0; ratified |
| cajeta-ifx-spec.md | runtime/src/cajeta/ifx/ (~25 files) |
| cajeta-accel-spec.md | xpu accel classes shipped |
| cajeta-ir-spec.md | src/cajeta/ir/Cir*.cpp shipped |
| compiler-optimization-spec.md | c4a2d8ad; Optimizer.cpp |
| compiler-threadsafe-spec.md | APPROVED; merged 6db9e3fb |
| mock-codegen-spec.md | SynthesizedMockClass.h; fb412779 |
| nep50-promotion-spec.md | 97db03f8; DType.cajeta |
| olla-local-repo-spec.md | OllaStore.{cpp,h} |
| thinlto-target-features-spec.md | ThinLTO in Optimizer/Compiler |
| ternary-string-concat-spec.md | 15e7bbff |
| frame-arena-spec.md | 3959a402 |
| string-builder-sso-spec.md | StringBuilder.cajeta |
| string-hash-xxh3-spec.md | XXHash3.cajeta |
| swisstable-hashmap-spec.md | HashMap.cajeta (swisstable) |
| sort-perf-spec.md | a034172d; Sort.cajeta |
| sort-adversarial-spec.md | 58888ba9 |
| test-harness-suite-split-spec.md | 315627df |
| skill-authoring-spec.md | a7ce3cac (168 skills) |
| idea-build-toolwindow-spec.md | 04465b51 |
| o0-loop-alloca-spec.md | 9e45a6e3 |
| benchmark-fidelity-spec.md | 7f51f06c; bench/ |
| benchmark-gap-sweep-spec.md | 648bc0a8 |
| gpu-f16-register-blocked-gemm-spec.md | a0d2eeb9 |
| gpu-f16-torch-parity-spec.md | 17960e62 |
| gpu-f16-torch-recipe-spec.md | 7d44c959 (probe concluded) |
| gpu-gemm-occupancy-spec.md | 65f12030 |
| gpu-matmul-profiling-spec.md | 34a287f6; samples/matmul-kernel/ |
| gpu-vulkan-f64-spec.md | 10b6c65e |
| amd-wmma-mi-native-lds-spec.md | be3eaf30; src/cajeta/xpu/amd/ |
| amdgpu-constant-folded-lds-spec.md | 75dcd1eb |
| kernel-occupancy-autotune-spec.md | be3eaf30 |
| kernel-vector-loadstore-spec.md | 8b90e8e9 |
| xpu-cdna-backend-spec.md | AmdgpuBackend/HipDriver shipped |
| xpu-coopmatrix-wide-b-read-spec.md | 7d3fa496 |
| xpu-device-profile-spec.md | DeviceProfile.{cpp,h} |
| xpu-device-vectorized-staging-spec.md | 1c251f5d |
| xpu-gfx-migration-spec.md | gfx/ + xpu/ split shipped |
| xpu-kernel-scheduling-hints-spec.md | fa77cb6f, 22151529; Schedule.cajeta |
| xpu-pagecache-fieldoffset-spec.md | PageCache.cajeta; 6db9e3fb |

## active (4) — specs/ + INDEX row `active`
| file | evidence |
|---|---|
| cja-source-view-spec.md | plan 0/70 |
| embedded-targets-spec.md | plan 0/39 |
| memory-viewer-spec.md | plan 0/39 |
| profile-spec.md | draft pending approval; U1 scaffold dce8365d |

## spec-only (10) — specs/ + INDEX row `draft`
| file | evidence |
|---|---|
| llm-kernel-scheduling-spec.md | research corpus + spec only — **archived 2026-09-06**, superseded by `xpu-tile-workload-profiles-spec.md` §3 |
| robotics-kernel-scheduling-spec.md | research corpus + spec only — **archived 2026-09-06**, folded into `xpu-tile-workload-profiles-spec.md` §6 |
| xpu-scan-primitive-spec.md | research corpus + spec only |
| xpu-pipelined-gemm-primitives-spec.md | spec only (AsyncCopy/CoopStage may partially satisfy — recheck at planning) |
| xpu-kernel-scheduling-spec.md | gap catalog only — **archived 2026-09-06**, superseded by `xpu-tile-scheduling-spec.md` (its plan, 0/117, is in `agents/archive/`) |
| xpu-gfx-streaming-geometry-spec.md | research corpus + spec only |
| xpu-build-ergonomics-spec.md | DRAFT rough-edge catalogue |
| simd-numeric-kernels-spec.md | DRAFT |
| ternary-int-codegen-spec.md | soundness analysis; impl status unresolved — keep draft |
| cajeta-ir-phase-b-spec.md | keep as draft: §4 resolved (pdqsort), §5 probe NO-GO, but §2 forwarding chains + §3 capturing lambdas unimplemented; stack-promotion attempt reverted (36cdbeef) |

## other (2)
| file | proposal |
|---|---|
| research-platform-roadmap-spec.md | keep at specs/ root as the standing roadmap (no INDEX row) |
| tour-build-your-first-package.md | teaching content → docs/ (guide absorbs it in unit 7); 5 inbound links fixed |
