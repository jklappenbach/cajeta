# Apple Vulkan support — spec

> Status: **active** — approved 2026-09-01. Evidence: `apple-vulkan-findings.md`; §N references
> point there. Plan: `agents/apple-vulkan-plan.md`.
>
> Supersedes `apple-targets-spec.md` §3.4's claim *"No Vulkan on iOS/tvOS"* — that is
> wrong. MoltenVK ships iOS 15+ and tvOS 15+ slices. `apple-targets-spec.md` keeps its
> Tier 1/2 static-library and FFI scope; this spec owns the xpu backend on Apple.

## 1. Definition

### 1.1 Purpose
Make Vulkan the xpu backend on Apple platforms, via two Vulkan-on-Metal drivers, so
cajeta kernels run on macOS, iOS and tvOS instead of falling back to CPU.

### 1.2 Why two drivers
Neither covers the platform matrix alone.

| | KosmicKrisp | MoltenVK |
|---|---|---|
| macOS | 26+, Apple Silicon | 12+, incl. Intel |
| iOS / iPadOS / tvOS | — | 15+ |
| Conformance | Vulkan 1.3 (Khronos #958) | none |
| Shader path | SPIR-V → NIR → MSL | SPIR-V → SPIRV-Cross → MSL |

KosmicKrisp is spec-correct and where the investment is going; MoltenVK is the only
iOS/tvOS path and the only Intel path. LunarG ships both and recommends bundling both.

### 1.3 Scope
macOS, iOS and tvOS. Compute only — the runtime has no graphics pipeline path (§1).
Drivers are discovered at runtime on macOS and vendored statically on iOS/tvOS; cajeta
ships no macOS driver binaries.

### 1.4 Non-goals
- **Upstream driver work.** `VK_KHR_shader_atomic_int64`, `VK_KHR_cooperative_matrix`
  and KosmicKrisp iOS support are contributions to Mesa and MoltenVK, not cajeta
  changes. Separate spec.
- **visionOS, Mac Catalyst, Simulator.** MoltenVK ships slices; not required here.
- **Intel Macs.** `x86_64-apple-darwin` is already a dropped release target.
- **A native Metal xpu backend.** Vulkan-on-Metal is the whole approach.

### 1.5 Already delivered
Commit `505cb87e` fixed four spec violations that made any portability ICD unusable:
portability enumeration, `portability_subset`, device `apiVersion` gating, and
`vulkanMemoryModel`. Those were prerequisites, not part of this spec's units.

## 2. Build and detection

The Vulkan path has never compiled on macOS. Detection is `__has_include(<vulkan/vulkan.h>)`
against default system include paths, and no provisioning installs the headers, so every
macOS binary shipped to date has the whole path stubbed out (§1).

- **2.1** When cajeta is configured on macOS with Vulkan headers available, both the
  compiler-side driver and the runtime bitcode compile the Vulkan path rather than the
  stub set.
- **2.2** When headers are absent, configuration reports Vulkan as unavailable at
  configure time rather than silently stubbing.
- **2.3** When `CAJETA_REQUIRE_VULKAN` is set and headers are absent, configuration fails.
- **2.4** When the runtime bitcode is compiled, it receives the same Vulkan include path
  as the compiler-side driver, so the two cannot disagree about what is available.
- **2.5** When macOS provisioning runs, it installs the Vulkan headers and loader.
- **2.6** When Vulkan is unavailable on any platform, the xpu layer falls back to the CPU
  backend exactly as it does today.

## 3. Driver discovery and selection

Both drivers are ICDs behind the Khronos loader. KosmicKrisp exports only the three
`vk_icd*` symbols and cannot be linked directly (§2), so the loader is mandatory.

- **3.1** When both ICDs are installed on a machine that supports both, exactly one
  physical device is selected, and the choice is deterministic.
- **3.2** When selecting between them, the discriminator is
  `VkPhysicalDeviceDriverProperties::driverID`, never enumeration order — the loader
  performs no physical-device sorting on macOS (§6).
- **3.3** When both are usable, KosmicKrisp is preferred.
- **3.4** When KosmicKrisp is present but unusable — a pre-macOS-26 host, or an Intel
  Mac — MoltenVK is selected without error. A KosmicKrisp manifest being present is not
  evidence the driver works (§6).
- **3.5** When no driver enumerates a device, the failure distinguishes *no ICD found*
  (`VK_ERROR_INCOMPATIBLE_DRIVER`) from *ICD loaded, no device* (`VK_ERROR_INITIALIZATION_FAILED`).
- **3.6** When a developer needs to force a driver, an override selects one by name, and
  the override beats the automatic policy.
- **3.7** When the selected driver is reported in diagnostics, it names the driver and
  its `driverInfo` version string.

## 4. Platform coverage

- **4.1** When targeting `aarch64-apple-darwin`, both drivers are candidates and §3 picks.
- **4.2** When targeting `arm64-apple-ios` or `arm64-apple-tvos`, MoltenVK is the only
  driver, linked as the **static** xcframework slice — Apple rejects naked dylibs
  (ITMS-90171), and static matches `apple-targets-spec.md`'s Tier 1 artifact.
- **4.3** When linking MoltenVK statically, Vulkan entry points are called directly with
  no loader and no ICD manifest.
- **4.4** When building for the iOS or tvOS simulator, the corresponding simulator slice
  is used.
- **4.5** When MoltenVK is built, `MVK_USE_METAL_PRIVATE_API` is never enabled — it
  disqualifies App Store distribution.
- **4.6** When a kernel uses cooperative matrix or 64-bit atomics, the capability probe
  reports it unsupported on every Apple target and the existing degrade path is taken.
  Neither driver exposes them (§4).

## 5. Driver provisioning

cajeta has **no link-time GPU dependency on any platform** — every backend is `dlopen`'d
and degrades to CPU when absent. macOS follows that rule.

- **5.1** When cajeta runs on macOS, it discovers the drivers through the Khronos loader
  at runtime and ships neither driver nor loader itself.
- **5.2** When no driver is installed, the xpu layer falls back to CPU, and the diagnostic
  distinguishes *no driver installed* from *driver installed but no usable device*.
- **5.3** When targeting iOS or tvOS, MoltenVK's prebuilt **static** xcframework slice is
  vendored at a pinned version — there is no loader on those platforms. A static library
  carries no signature of its own, so it raises no library-validation question.
- **5.4** When a build carries our own driver patches, that build is from source — a
  development and validation path, not the shipping default.

**Deferred.** How macOS users *get* a driver — Homebrew formula dependencies, a `cvm`
install-time check, bundling — is not decided here. Runtime discovery works today
without it, and the question only becomes real when someone deploys. Revisit then.

## 6. Testing

No GPU-bearing Apple hardware is available, and GitHub-hosted macOS runners cannot
substitute: their paravirtual GPU reports Apple5 with no Metal 3 and
`simdgroup matrix mul = false` (§5).

- **6.1** When CI runs on a GitHub-hosted macOS runner, both drivers build and the
  packaging smoke test runs.
- **6.2** When the packaging smoke test runs without a usable GPU, it asserts
  `VK_ERROR_INITIALIZATION_FAILED` rather than `VK_ERROR_INCOMPATIBLE_DRIVER`, proving
  manifest paths, signing and rpaths resolve.
- **6.3** When a GPU-independent functional leg is needed, lavapipe provides one — it
  builds and runs on macOS arm64 and advertises Vulkan 1.4 (§5).
- **6.4** When MoltenVK functional tests run on a hosted runner, they execute against the
  paravirtual device, excluding anything needing Metal 3 or above.
- **6.5** When real-hardware validation is required, it runs on a self-hosted or rented
  Mac, matching the existing self-hosted GPU CI pattern.
- **6.6** When a driver is selected at runtime, a test asserts which one was chosen for a
  given platform and OS version.

## 7. Open questions

- **7.1** ~~Which macOS artifact format replaces `.tar.gz`?~~ **DECIDED: `.pkg`** when a
  deployment path is needed. Deferred with the rest of §5 — with no bundled drivers,
  nothing forces the change now.
- **7.2** ~~Bundle drivers, or discover them at runtime?~~ **DECIDED 2026-09-01: do not
  bundle on macOS** — cajeta has no link-time GPU dependency on any platform and `dlopen`s every
  backend, so bundling would be a one-platform departure. Document
  `brew install mesa molten-vk vulkan-loader`. iOS/tvOS must vendor MoltenVK's prebuilt
  **static** xcframework slice (no loader exists there); static libraries carry no
  signature, so the Team ID problem does not arise. Build from source only for builds
  carrying our own driver patches. §5 rewritten accordingly.
- **7.3** ~~Rented or self-hosted Mac?~~ **DECIDED 2026-09-01: AWS EC2 Mac.** Bare
  metal, M4 = Apple9, ~$29.52 for the 24 h dedicated-host minimum. Constraints that
  must hold: bare metal (paravirt GPUs report Apple5 and lack simdgroup matrix), and
  a **macOS 26 AMI** — KosmicKrisp will not run below it. Revisit buying a Mac mini M4
  (~$599, pays back against monthly rental in ~4 months) if the work is sustained.
- **7.4** Does `apple-targets-spec.md` get amended in place to drop its wrong Vulkan
  claim, or archived in favour of a rewrite?
