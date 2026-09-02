# Apple Vulkan support — research findings

Evidence base for Apple platform support via Vulkan-on-Metal (KosmicKrisp + MoltenVK).
Companion to a spec/plan not yet written.

- **Author:** session julian-4d, 2026-09-01
- **Status:** RESEARCH COMPLETE — all 5 tracks reported. One empirical question remains (§4.2)
  and it needs Apple hardware.
- **Decisions taken by Julian:** ship **both** drivers; **fix** both drivers' missing
  cooperative matrix and 64-bit atomics; fork, develop against cajeta, then upstream PRs.
- Related: `apple-targets-spec.md` (untracked draft, iOS/tvOS only, contradicts this),
  `research-platform-roadmap-spec.md:81,372` (references a nonexistent
  `apple-metal-backend-spec.md` as P0).

---

## 1. The starting position is worse than "no Apple support"

**Every macOS binary cajeta has shipped has the entire Vulkan path compiled to stubs** —
both the compiler-side `VulkanDriver.cpp` and the runtime `cajeta_xpu_vulkan.c`. The
`libMoltenVK.dylib` fallback at `cajeta_xpu_vulkan.c:249` is unreachable dead code.

Cause: detection is purely `__has_include(<vulkan/vulkan.h>)` against **default system
include paths**, and Vulkan headers are provisioned **nowhere** — not in `setup.sh`'s brew
list, not in apt, not in `release.yml`'s macOS leg. Only MSYS2 and the lavapipe test leg
install them. Vulkan appears in `src/CMakeLists.txt` exactly once, in a comment.

There is no configure-time signal — no `CAJETA_REQUIRE_VULKAN` analogous to the existing
`CAJETA_REQUIRE_ZMQ`. It stubs silently.

**Two compiles need the header**, and the second is easy to miss:
1. `cajeta_lib`'s C++ compile of `VulkanDriver.cpp` — CMake include dirs (`src/CMakeLists.txt:1090-1095`)
2. **The runtime-bitcode compile** (`src/CMakeLists.txt:663-668`) — uses `${CAJETA_CLANG}`
   (the *fork's* clang) with only `-I${CAJETA_XXHASH_INCLUDE_DIR}`. On Apple it adds
   `-isysroot $(xcrun --show-sdk-path)`. A Vulkan SDK at `~/VulkanSDK/...` would not be
   found. `CAJETA_RT_FLAGS` (`:622-638`) is where a `-I` must go.

### But macOS is not a green field
- `aarch64-apple-darwin` is a **shipping release target** (`release.yml:102`, `macos-14`).
- `x86_64-apple-darwin` deliberately dropped (`release.yml:209-211`) — Intel is already a non-goal.
- The LLVM fork publishes a **`macos-arm64` toolchain with SPIRV in `LLVM_TARGETS_TO_BUILD`**,
  so SPIR-V codegen on macOS already works.
- `.pkg` via `productbuild` exists; **signing/notarization explicitly not implemented**
  (`cmake/CPackOptions.cmake:92-93`).

### Four Vulkan spec violations, pre-existing and vendor-independent
1. **No `VK_KHR_portability_enumeration`.** Zero hits repo-wide; instance creation is bare.
   Under the Khronos loader this makes `vkEnumeratePhysicalDevices` return **zero devices**,
   and `cajeta_xpu_vulkan.c:315` bails on `if (count == 0) return 0`. Breaks MoltenVK.
2. **No `VK_KHR_portability_subset` in `devExts`** — required by spec when advertised
   (VUID-VkDeviceCreateInfo-pProperties-04451).
3. **No device `apiVersion` check anywhere.** All 1.3 gating is `#if defined(VK_VERSION_1_3)`
   — a *header* check, not a device check.
4. **`vulkanMemoryModel` never enabled**, though the compiler emits
   `OpMemoryModel Logical VulkanKHR` (`SpirvBackend.cpp:53-55`). `VK_KHR_cooperative_matrix`
   likewise never enabled at device creation.

### Runtime portability is better than assumed
Only 17 `__linux__` and 6 `__APPLE__` sites in non-vendored code. Fibers are `ucontext`
with explicit Apple handling; sync is pure pthreads with no platform `#if`; bitcode
embedding uses `xxd -i`, not `objcopy`, so it is not ELF-specific. **Absent entirely:**
io_uring, futex, timerfd, eventfd, `pthread_barrier`, `<linux/*>`, `/sys/` literals.

Real blockers, prioritized:
- **P0 fd leak** — `cajeta_rt_concurrent_exec.c:2035`/`:2056` return `-1` without calling
  `close(fd)`. User-reachable intrinsic.
- **P0 `Command.cwd` hard-fails** — `__linux__` gate too narrow; macOS has
  `posix_spawn_file_actions_addchdir_np` (10.15+).
- **P1 `madvise(MADV_DONTNEED)` does not zero-fill on Darwin**, but the arena depends on it
  (`cajeta_rt_core.c:1834-1835`). Needs `MADV_FREE_REUSABLE` or explicit memset.
- **P1** every net await blocks its carrier thread (no kqueue engine exists).

---

## 2. KosmicKrisp — what it is

Mesa Vulkan-on-Metal driver. **LunarG-built, Google-funded**, MIT, in Mesa main at
`src/kosmickrisp` since commit `7c268a1e9185` (2025-10-20), first shipped in **Mesa 26.0.0
(2026-02-11)**. 352 commits and actively developed; ~40-58 commits/month.

**Hard floor: macOS 26 + Apple Silicon + Metal 4.** `mtl_device_create()` takes the first
device supporting `MTLGPUFamilyMetal4`; no match → nil → **zero physical devices**. No
Intel Macs, structurally.

**Conformance is narrower than the marketing.** Exactly one registry entry:

> **#958** — "Apple M3 Pro (KosmicKrisp)", CTS 1.4.3.2, **macOS 15.4.1**, Software Freedom
> Conservancy, 2025-10-29, level **Vulkan 1.3**.

That submission **predates the June 2026 move to Metal 4 / macOS 26**, so it does not cover
the code path you would ship. The driver advertises 1.4; that is **unsubmitted**. LunarG's
last explicit quality label is **beta** (Feb 2026); their SDK doc calls it *"a technical
preview, or an Alpha release only."*

**Shader path — matters for a SPIR-V-emitting compiler:**
```
SPIR-V → spirv_to_nir() [NIR_SPIRV_VULKAN] → msl_preprocess_nir → kk_lower_nir
       → nir_to_msl() → MSL SOURCE TEXT → MTL4LibraryDescriptor.source → MTL4Compiler
```
**Not SPIRV-Cross — Mesa's NIR.** That is the main architectural difference from MoltenVK.
Ordinary Vulkan SPIR-V needs no special handling. `MESA_KK_DEBUG=msl` dumps every generated
shader — useful for diagnosing our own codegen.

**Loader-only.** The dylib exports exactly three symbols: `vk_icdGetInstanceProcAddr`,
`vk_icdGetPhysicalDeviceProcAddr`, `vk_icdNegotiateLoaderICDInterfaceVersion`. **No
`vkCreateInstance`.** You must ship `libvulkan.1.dylib`; direct linking is impossible.

**iOS does not exist.** Mesa docs verbatim: *"No iOS support is present as of now."* No
`ios` in `meson.options` platforms; zero iOS conditionals in `src/kosmickrisp`; the only
iOS MR (!39186) is build plumbing, opened 2026-01-07 and **still unmerged**. LunarG
estimate: "anticipated in 2026." Of the Vulkanised 2026 roadmap, tessellation and 1.4
exposure landed; **iOS and 1.4 conformance did not.**

**Meson:** `-Dvulkan-drivers=kosmickrisp` — `auto` yields nothing on macOS by design.
Build deps LLVM 20.1.8+/libclc/spirv-llvm-translator are **build-time only**; SPIRV-Tools
is a genuine runtime dep (statically absorbed with `--prefer-static`).

---

## 3. MoltenVK — the iOS path

Apache-2.0, © Brenwill Workshop. **Not conformant** — zero entries in the Khronos registry;
CTS tracking shows 545,919 pass / 14,117 fail (2.5%) at v1.4.2. Do **not** read
`conformanceVersion` from it as a conformance claim — `MVKDevice.mm:830` reports `{1,4,4,0}`
with the comment `// Latest version of CTS used to test`.

macOS 12+ / iOS 15+ / tvOS 15+, Apple Silicon **and Intel**, plus visionOS, Simulator and
(buildable separately) Catalyst. Tagged releases 2-4×/year; **v1.4.2 (2026-07-24)**.

**iOS artifact shape:** deliberately **no naked dylib** — Apple rejects those (ITMS-90171).
You get `static/MoltenVK.xcframework/ios-arm64/libMoltenVK.a` (7.13 MB) or a dynamic
`.framework`. For cajeta's static-library Tier-1 target, the **static slice is the right
artifact**, matching what Godot and PPSSPP do.

**Runtime MSL compilation on iOS is permitted.** `makeLibrary(source:options:)` is
documented iOS 8.0+ and not deprecated; Apple shipped a *new* runtime source-compile API
(`MTL4Compiler`) on iOS 26. Guideline 2.5.2 never mentions JIT. Decisive: **WebKit's WebGPU
compiles MSL at runtime on iOS with arbitrary web content**, and WebKit is the mandated
engine. Zero App Store rejections for shader compilation in MoltenVK's tracker history.
**Never ship `MVK_USE_METAL_PRIVATE_API=1`** — its README says that disqualifies App Store
distribution; the live risk is Guideline 2.5.1 (private API), not 2.5.2.

**No JIT entitlement needed** — Metal compilation produces GPU bytecode via Apple's
out-of-process `MTLCompilerService.xpc`, not writable-executable CPU pages. But note: if
any part of the cajeta toolchain JITs CPU code, *that host* needs
`com.apple.security.cs.allow-jit`.

---

## 4. The two capability gaps — RESOLVED

### 4.1 Cooperative matrix — classification (c): unwritten lowering

**Apple Silicon has the hardware.** `simd_matrix_fmadd16/32` are real ISA opcodes from
Apple7 (M1) onward; Apple10 (M5 / A19 Pro) adds a dedicated Neural Accelerator per GPU core
(Apple Newsroom 2025-10-15, "over 4x the peak GPU compute performance compared to M4").

**MSL exposes it, narrowly.** MSL 4.1 §2.4/§6.8: `simdgroup_matrix<T,Cols,Rows>` with
`Cols = Rows = 8` only, `T` ∈ {half, bfloat, float}. Five functions total. **One template
parameter `T` for all four operands**, no integer types, no saturation, and *"the mapping of
matrix elements to threads in the SIMD-group is unspecified"* — so per-lane element access
has no correct lowering.

**This lands directly on cajeta's type model.** `amdgpu-coopmatrix-tier-straddle-spec.md:46-47`:
accumulators are `f32`/`i32`, A/B operands are `f16`/`bf16`/`i8`.

| cajeta kernel | via `simdgroup_matrix` |
|---|---|
| `Ewise.matmulF32` (f32→f32) | **yes** — `simdgroup_float8x8` |
| `Ewise.matmulBf16` (bf16→bf16) | **yes** — `simdgroup_bfloat8x8` |
| `matmulF16` (f16→f32) | **no** — mixed types unrepresentable |
| `matmulBf16Wide` (bf16→f32) | **no** — mixed types unrepresentable |
| `matmulI8` (i8→i32) | **no** — no integer matrices |

**Empirical confirmation this is not theoretical:** four real coop-matrix consumers tested on
an M4 against a MoltenVK build that *did* advertise the extension — vkpeak,
`vk_cooperative_matrix_perf`, clpeak, llama.cpp. **Zero of four worked** (spec-constant
dimensions, 8×8-only, unsupported element access).

**KosmicKrisp is the better target than MoltenVK**, for two structural reasons:
1. Mesa already ships `nir_lower_cooperative_matrix_flexible_dimensions()` (1,370 lines,
   `nir.h:7553`), which decomposes 16×16×16 into 8×8×8 granules. SPIRV-Cross has no
   equivalent and throws — which is exactly why llama.cpp fails on MoltenVK.
2. Scale is bounded: panvk did the equivalent in **MR !42723, +767 lines, merged 2026-08-05**
   (*"Took me some time to pass all the CTS but CI is happy"*). KosmicKrisp's version is
   ~700–1,200 lines with **no ISA or register-allocator work**, because it emits MSL text.

Note KosmicKrisp must *not* copy panvk's design — panvk/radv/anv scalarize because they own
the register allocator. KosmicKrisp must keep the matrix opaque to MSL source, i.e. the
SPIRV-Cross approach. `spirv_msl.cpp` is the template; `panvk_nir_lower_cooperative_matrix.c`
is not.

**MoltenVK's coop-matrix work is already someone else's and stalled.** SPIRV-Cross merged MSL
codegen (PR #2596, 2026-03-13); MoltenVK vendors a *later* SPIRV-Cross revision; driver PR
#2753 is **67 lines**, open since 2026-06-09, WIP-blocked. **Leave it alone.**

**Apple is steering away from `simdgroup_matrix`.** MSL 4.1 §6.8 opens: *"Instead of using
simdgroup matrix multiplication, consider using Tensors (section 2.22) and Metal Performance
Primitives (section 7)."* Metal 4's `tensor_ops::matmul2d` takes **arbitrary M/N/K**, mixed
precision, and int8/int4/int2/fp8/fp4 — Table 7.3 lists `char×char→int` and `half×half→float`,
i.e. **exactly cajeta's type model**. Apple's M5 announcement says the Neural Accelerators are
programmed *"using Tensor APIs in Metal 4."*

⇒ A `simdgroup_matrix` implementation covers 2 of cajeta's 5 GEMM kernels and **probably will
not** collect the M5 uplift (unverified — Apple states nothing either way). The full-coverage
target is `tensor_ops::matmul2d` / `cooperative_tensor`, on which **neither Mesa nor
SPIRV-Cross has written a line.**

### 4.2 64-bit atomics — classification CONTESTED, one experiment settles it

**Apple's own documentation contradicts itself, and this changes the recommendation.**

- **MSL 4.1 spec (2026-06-04) §6.16.4.6** documents exactly two 64-bit atomics —
  `atomic_min_explicit`, `atomic_max_explicit` — on `atomic_ulong`, **device address space
  only, `memory_order_relaxed` only, returning `void`**. No add/and/or/xor/exchange/CAS, no
  threadgroup scope. Since `OpAtomicUMin`/`OpAtomicUMax` must return the previous value, this
  would make `VK_KHR_shader_atomic_int64` unimplementable.
- **Metal Feature Set Tables (2026-05-21), footnote 7**, says the opposite: Apple8 supports
  ulong min/max on macOS only, and ***"The full set of 64-bit atomic operations is supported
  on all platforms starting with Apple9."***

"The full set" is unresolved. It may mean "the full set the language exposes" (min/max) or
literally the full atomic op set. **This is an empirical question, not a documentation one.**

**The experiment that settles it:** one Metal 4 compute kernel calling
`atomic_fetch_add_explicit` on an `atomic_ulong`, on an M3 or M4 (Apple9). Nothing else
matters until this is run. Requires Apple hardware — see §5.

**Strategic weight if it works.** Diffing `docs/features.txt` per-extension rows, KosmicKrisp
is "all DONE" for Vulkan 1.0, 1.1 and 1.3. **`VK_KHR_shader_atomic_int64` is the *single*
remaining gap in its Vulkan 1.2 row** (Vulkan 1.4's only gap is
`VK_EXT_pipeline_protected_access`). It is unowned, and deferred solely because MoltenVK lacks
it — reasoning that collapses the moment someone shows Metal can do it.

MoltenVK's blocker is billhollings' 2022 analysis on issue #1692 (*"Faking this out by bolting
on a separate fetch operation would defeat the purpose"*), which was correct against Metal 3.0
and has never been re-tested against the Apple9 feature-table claim.

⇒ **int64 atomics is now the STRONGER of the two features**, contingent on one measurement.

---

## 4A. Upstreaming — process and entry point

**There is a written invitation.** Mesa issue **#14251 "kk: wish list"**, opened by Aitor
Camacho: *"This issue tracks things KK will aim to implement but are of no priority now. Feel
free to add wanted extensions/features as a comment for them to be added to the list."* And
LunarG's standing position on deprioritized items: *"We can revisit this if an application
requires it."*

**Post before writing code.** A concrete "here is a working Metal kernel on M3" is exactly the
evidence both projects said would move priorities. It is also how to find out whether
cooperative matrix is welcome — that feature is **completely unmentioned**: absent from the
extension table, the wish list, and the MoltenVK-parity list (#14209). Greenfield, but with
**zero stated maintainer intent**.

**Scope note:** KosmicKrisp's roadmap is explicitly *MoltenVK parity* (#14209, 248/252 boxes
done), so both features are outside stated scope by construction.

**Reviewers** (across 305 `kk:` commits): **Aitor Camacho** 138 (`aitor@lunarg.com`, CODEOWNER
`/src/kosmickrisp`), **Arcady Goldmints-Orlov** 56 (`arcady@lunarg.com`, CODEOWNER
`/src/kosmickrisp/compiler` — **where any coop-matrix NIR→MSL lowering lands**), squidbus 14,
Alyssa Rosenzweig 2.

**The only macOS CI for this driver anywhere** is squidbus'
`github.com/squidbus/mesa-kosmickrisp` (automated universal macOS builds via GitHub Actions).
Manual CTS results are posted to Mesa issue #15877. That is where conformance evidence goes.

**Mandatory workaround registry.** `docs/drivers/kosmickrisp/workarounds.rst`, currently at
`KK_WORKAROUND_18`, with a strict template (macOS version, Apple ticket number + status,
triggering CTS test, dated log): *"All workarounds must be documented here and no code comment
info should be provided other than the name `KK_WORKAROUND_#`."* int64-atomics work will very
likely need an entry.

**`MESA_KK_EXPERIMENTAL`** is live practice for partial support (currently gating
`VK_EXT_custom_border_color`, `VK_EXT_border_color_swizzle`, `VK_EXT_image_view_min_lod`).

**Commit trailer: `Assisted-by:`, not `Co-authored-by:`.**

**Recommended sequence:**
1. Run the Metal 64-bit-atomic experiment on M3/M4. Nothing proceeds until this is settled.
2. If it works, comment on Mesa #14251 and MoltenVK #1692 with the result.
3. Land in KosmicKrisp first — gate is `.KHR_shader_atomic_int64 = pdev->info.gpu_apple_family >= 9`
   plus NIR→MSL work in `compiler/`, `docs/features.txt`, `docs/relnotes/new_features.txt`, a
   `KK_WORKAROUND_#` if needed, CTS results in the MR description. **Completing the Vulkan 1.2
   row is the argument.** Then use that to make the MoltenVK case.
4. **Leave MoltenVK cooperative matrix alone** — it is zainsharief's, blocked on
   HansKristian-Work's *"far too naive to be shippable."* If we want that space, the unexplored
   angle is `tensor_ops::matmul2d`.
