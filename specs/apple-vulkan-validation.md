# Apple Vulkan — validation runbook

> For the rented-Mac window (spec §7.3). Everything in units 1–4 that could be
> written without Apple hardware **is** written; this is the ordered list of what
> that costs to confirm, and what each step closes. Work top to bottom — later
> phases assume earlier ones passed.
>
> EC2 Mac bills a 24 h dedicated-host minimum (~$29.52), so the constraint is
> *sequencing*, not wall-clock. Budget half a day.

## Preconditions

- **macOS 26** AMI, **bare metal** (`mac2-m2.metal` or newer). A paravirtual GPU
  reports Apple5 with no simdgroup matrix and invalidates phases 2–4.
- Xcode + command line tools.
- `brew install cmake ninja antlr4-cpp-runtime openjdk@21 googletest glog zstd
  zeromq xxhash coreutils vulkan-headers vulkan-loader molten-vk mesa`
- The `cajeta-llvm-23-r10-macos-arm64.tar.zst` fork toolchain.

## Phase 0 — does the build even come up

| # | Run | Expect | Closes |
|---|---|---|---|
| 0.1 | `./setup.sh -DCAJETA_REQUIRE_VULKAN=ON` | `Found Vulkan headers -> …` and Configure succeeds | 1.3.2 |
| 0.2 | `cmake --build build -j` | links | 1.3.2 |
| 0.3 | `./build/test/cajeta_test --gtest_filter='XpuAbiContractTests.*'` | `vulkanAvailabilityAgreesAcrossCompilerAndRuntime` passes with **both sides true** | 1.3.2 |

If 0.3 passes with both sides *false*, the headers were found for `cajeta_lib`
but not for `CAJETA_RT_FLAGS` — the exact split Unit 1 exists to prevent.

## Phase 1 — which driver got picked

Both ICDs installed. `brew list --versions molten-vk mesa` first; if `mesa`'s
formula did not build KosmicKrisp on this host, phase 1.2 is not runnable and
MoltenVK is the only candidate.

| # | Run | Expect | Closes |
|---|---|---|---|
| 1.1 | `vulkaninfo --summary \| grep -i driverID` | two ICDs listed | — |
| 1.2 | `CAJETA_XPU_BACKEND=vulkan CAJETA_EXPECT_VK_DRIVER_ID=28 ./build/test/cajeta_test --gtest_filter='XpuVulkanDriverSelect.*'` | green — KosmicKrisp wins | **2.3.1** |
| 1.3 | `VK_LOADER_DRIVERS_DISABLE='*kosmickrisp*' CAJETA_XPU_BACKEND=vulkan CAJETA_EXPECT_VK_DRIVER_ID=14 …` | green — MoltenVK wins | **2.3.2** |
| 1.4 | `CAJETA_XPU_VK_DRIVER=moltenvk CAJETA_EXPECT_VK_DRIVER_ID=14 …` | green — override beats policy on real ICDs | 2.1.5 (live) |
| 1.5 | `CAJETA_XPU_VK_DRIVER=nosuchdriver …` | falls back to policy, does not crash | 2.1.5 (live) |

The `reportsTheSelectedDriver` test prints `driverId`, `name` and `info` to
stderr on every run — capture that line for the record either way.

## Phase 2 — does a kernel actually run

| # | Run | Expect | Closes |
|---|---|---|---|
| 2.1 | `CAJETA_XPU_BACKEND=vulkan ./build/test/cajeta_test --gtest_filter='*Vulkan*'` | pass or *explicit* skip; **no crashes** | 2.3.1/2.3.2 |
| 2.2 | same under `VK_LOADER_DRIVERS_DISABLE='*kosmickrisp*'` | MoltenVK's result — expect more skips | 2.3.2 |

Record the pass/skip/fail split per driver. A large MoltenVK skip set is the
expected outcome, not a failure: it is missing extensions, and it is the
argument for the upstream work.

## Phase 3 — the contested atomics question

`specs/schemas/metal-atomic64-probe.swift` — 15 isolated probes separating
void-returning `atomic_min_explicit` from returning `atomic_fetch_min_explicit`.
Apple's MSL spec and its Feature Set Tables disagree about 64-bit atomics; this
settles it on an M3/M4.

| # | Run | Expect | Closes |
|---|---|---|---|
| 3.1 | `swift specs/schemas/metal-atomic64-probe.swift` | a per-probe table | the §4 open question |
| 3.2 | `CAJETA_XPU_BACKEND=vulkan CAJETA_EXPECT_ATOMIC_INT64=0 ./build/test/cajeta_test --gtest_filter='*reportsAtomicInt64*'` | green — neither driver advertises it | 4.6 (live) |

If 3.1 shows working 64-bit atomics, the driver-side gap is *implementable*, and
the upstream KosmicKrisp spec gets a concrete first item. **Do not open an
issue or MR off this** — bring the numbers back first.

## Phase 4 — iOS / tvOS  🚧 BLOCKED

Unit 3 cannot be validated yet, and not for want of hardware:
`apple-targets-spec.md` is still a **draft** and none of Tier 1 exists — no
`--emit=staticlib`, no SDK/sysroot handling, no fiber port off `ucontext`. There
is no `arm64-apple-ios` build to link MoltenVK into.

What *is* written and inert until then:

- `scripts/fetch-moltenvk.sh` — vendors the pinned v1.4.2 **static** xcframework
- `src/CMakeLists.txt` — slice selection by triple, `MVK_USE_METAL_PRIVATE_API`
  hard gate, framework link set
- `cajeta_xpu_vulkan.c` — `CAJETA_RT_VULKAN_STATIC`: bind `vkGetInstanceProcAddr`
  directly, no loader, no ICD manifest

Cheap partial check while the Mac is up (proves the vendoring and the slice
layout, not the build):

```
./scripts/fetch-moltenvk.sh
ls External/MoltenVK/static/MoltenVK.xcframework
```

Expect `ios-arm64`, `ios-arm64_x86_64-simulator`, `tvos-arm64`,
`tvos-arm64_x86_64-simulator`, `macos-arm64_x86_64`. If those slice names differ
from what `src/CMakeLists.txt` composes, fix the CMake — that is the single most
likely thing to be wrong in speculative work.

## Phase 3b — the one KosmicKrisp answer only a Mac has

Two minutes, and it decides the shape of the whole cooperative-matrix
implementation (`kosmickrisp-upstream-spec.md` §6.1).

```
find /Applications/Xcode.app -name 'metal_simdgroup_matrix*' -exec grep -n \
    'thread_elements\|storage_type' {} +
```

If `thread_elements()` exists, per-lane element access is implementable and
KosmicKrisp can support the full extension — strictly better than MoltenVK,
which throws on it. If it does not, element access must be refused and the
extension is shape-restricted. **Do not start Unit 3's element-access code
until this is answered.**

## Phase 5 — CI

`.github/workflows/apple-vulkan.yml` has never run. It is `workflow_dispatch` +
PR-path-triggered, three ICD legs (`moltenvk`, `lavapipe`, `none`) on `macos-14`.

| # | Run | Expect | Closes |
|---|---|---|---|
| 5.1 | dispatch with `icds=none` | red only if the *no-ICD* leg fails to report `-9` | 4.1.1 |
| 5.2 | dispatch with `icds=all` | three legs green | 4.2.1–4.2.4 |
| 5.3 | dispatch with `gpu_family_probe=true` | records the macos-26 GPU family | **4.3.2** |

5.3 is the one that could change the plan: the Apple5 / no-simdgroup-matrix
evidence is from Tart VMs, not GitHub's fleet. If a `macos-26` runner reports
Apple7+, real MoltenVK functional coverage becomes free and §6.5's rented-Mac
leg gets much smaller.

## What this runbook does not cover

- **4.2.5** self-hosted/rented Mac CI leg — needs a decision on keeping the host.
- **3.3.1** a kernel on a physical iOS device — needs phase 4 unblocked *and* a
  provisioning profile.
- Upstream KosmicKrisp work (iOS support, int64 atomics, coop matrix). Separate
  spec, not written. Nothing goes to Mesa or MoltenVK without Julian's say-so.
