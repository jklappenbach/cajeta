# windows-jit-coff-reloc — defect (open; found by the first device-tests CI run)

## 1. Definition

**1.1 Symptom.** On Windows, the in-process JIT aborts while linking a host
COFF object:

```
[ RUN      ] XpuVulkanDispatchDeviceTests.arbitraryBlockSizeOnDevice
LLVM ERROR: IMAGE_REL_AMD64_ADDR32NB relocation requires an ordered section layout

cajeta: SIGABRT caught — likely heap corruption or assertion.
```

`LLVM ERROR` followed by `SIGABRT` — a hard abort inside LLVM, not a failing
assertion. The test process dies where it stands.

**1.2 Found.** 2026-08-09, by the first run of
`.github/workflows/device-tests.yml` on the `PHOENIX` self-hosted runner
(Windows, NVIDIA 4090). Run 31339873957. Not previously known: no occurrence
of `IMAGE_REL_AMD64_ADDR32NB` or "ordered section layout" anywhere under
`src/`, `specs/`, or `docs/` before this.

**1.3 Why it outranks a single failing test.** The abort truncates the whole
binary. In the run that found it, 15 device tests had already executed and
passed on the 4090; the rest of `XpuVulkanDispatchDeviceTests` (36 tests in
that suite) and everything sequenced after it never ran. **The true NVIDIA
pass rate is unknown while this stands** — the defect is masking whatever else
does or does not work on Windows.

It also reports dishonestly. gtest exits **3** (abort), not 1 (assertion
failure), so the run recorded *zero failed tests* while crashing. Any gate
keying on "no failures" reads this as clean.

**1.4 Scope — host object linking, not device codegen.**
`IMAGE_REL_AMD64_ADDR32NB` is a COFF/PE image-relative 32-bit relocation (x64
exception-handling and RVA references). It has no ELF counterpart, so this
cannot fire on Linux, and it is raised by LLVM's JIT **object linking layer**
processing the *host* module. The Vulkan/SPIR-V backend is only what causes
that particular host module to be emitted; it is not the thing that fails.

**1.5 Confirmed platform-specific by differential evidence,** not inference.
The identical test passes on `proton` (Linux/ELF, AMD Radeon 8060S via
`radv`): a full `XpuVulkan*` sweep of 69 tests was green there on 2026-08-09,
`arbitraryBlockSizeOnDevice` included, and verified to have *executed* on
hardware (`[ OK ]` at ~68 s, not `SKIPPED`). The only variable that changed is
the object format.

**1.6 Suspected mechanism.** `src/cajeta/jit/CajetaJitHost.cpp:643` constructs
the JIT with a bare `llvm::orc::LLJITBuilder` and never calls
`setObjectLinkingLayerCreator`, so the object linking layer is LLVM's platform
default. On COFF/x86-64 the RuntimeDyld-vs-JITLink selection is the obvious
lever: the diagnostic comes from a linker that needs sections in a defined
order to compute an image-relative displacement, a guarantee one path makes
and the other does not.

**1.7 Not Vulkan-specific, and probably latent elsewhere.** Both
`saxpyRoutesToVulkanOnDevice` (passes) and `arbitraryBlockSizeOnDevice`
(aborts) compile the same `saxpyHostSource(...)`; the crashing one differs only
by `block=128` versus the default
(`test/xpu/XpuVulkanDispatchDeviceTests.cpp:107-118`). A small change in the
emitted host module is enough to cross the threshold, which implies any JIT
workload on Windows can hit this — it is not a property of the Vulkan path.

**1.8 Reproduce.** On a Windows host with a Vulkan-capable GPU:

```
build/test/cajeta_test --gtest_filter=XpuVulkanDispatchDeviceTests.arbitraryBlockSizeOnDevice
```

Or in CI: `device-tests.yml` with `legs=windows`.

**1.9 Non-goals.** Suppressing the test. A permanent `--gtest_filter`
exclusion in the workflow is rejected: a permanently-skipped crashing test is
how a defect gets forgotten, and this one is actively hiding the remaining
Windows device results. If an exclusion is needed to obtain one clean sweep,
pass it through the workflow's `filter` **input** so it lives in a single run
rather than in the committed file.

## 2. Acceptance

- **2.1** When `XpuVulkanDispatchDeviceTests.arbitraryBlockSizeOnDevice` runs
  on Windows against a Vulkan-capable GPU, it completes without aborting.
- **2.2** When the full `*DeviceTests*` filter runs on the `PHOENIX` runner,
  the process reaches the end of the suite — no `LLVM ERROR` / `SIGABRT`
  truncation — so the NVIDIA pass rate becomes a measured number rather than a
  floor.
- **2.3** When a JIT abort of this class occurs in future, CI fails visibly
  rather than reporting zero failures: the gate must treat a non-zero,
  non-1 exit code (abort) as failure and not merely count failed assertions.
- **2.4** The Linux/ELF path stays green — the `XpuVulkan*` sweep on `proton`
  continues to pass 69/69 on hardware, so a COFF-side fix has not regressed
  the ELF side.
