# windows-vulkan-cpu-forced-hang — defect (FIXED 2026-08-11; un-masked by the COFF JITLink fix)

**Resolution.** Not Vulkan, not the env override, not JITLink — the CPU
kernel pool's pthread primitives (`cajeta_xpu_dispatch.c` `g_caj_kpool`
mu/go/done) were plain zero-initialized statics, valid only where
`PTHREAD_*_INITIALIZER` happens to be all-zero (glibc). On winpthreads the
initializers are -1 sentinels and a zeroed mutex/condvar is an invalid
object; with the default `WORKER_SPIN=0` both workers and the joining caller
enter `pthread_cond_wait` on invalid primitives with unchecked return codes —
the first CPU-rung kernel launch on Windows hung forever. This test was
simply the first CPU-rung launch in Windows history: every earlier run died
at the COFF abort before reaching it, and all 40 post-COFF-fix green tests
were vulkan/nvptx.

**Attribution (1.4) answered by two bounded runner experiments**: the hang
reproduced in isolation on both the post-JITLink (run 31488253978) and
pre-JITLink (run 31492808199) binaries — pre-existing, the COFF fix merely
un-masked it. **Fix** (`fix/windows-cpu-pool-pthread-init`): designated
`PTHREAD_*_INITIALIZER`s on the pool struct — bit-identical to zero-init on
glibc, correct on winpthreads. Runtime sweep found no sibling offenders.
**Validated run 31536611067**: Windows passes `bundledVulkanCpuForcedToCpu`
in bounded time; Linux controls (the test + `XpuCpuPoolTeardown`) green.
Acceptance 2.1/2.3/2.4 met; 2.2 (full-suite Windows sweep) rides the next
full device-tests run.

## 1. Definition

**1.1 Symptom.** On Windows,
`XpuVulkanDispatchDeviceTests.bundledVulkanCpuForcedToCpu` starts and never
finishes: no output, no crash, no failure — 4.5 hours of silence until the
workflow's 6-hour ceiling killed the job. Everything before it was green.

**1.2 Found.** 2026-08-11, device-tests run 31452203287 on the `PHOENIX`
Windows runner (4090) — the first run carrying the COFF JITLink fix
(`windows-jit-coff-reloc`, fixed at `dcee8620`). 40 tests passed with zero
failures and zero relocation aborts before the hang.

**1.3 This test had never executed on Windows.** Every earlier Windows run
died at the COFF relocation abort before reaching it, so there is no Windows
baseline: the hang may be a pre-existing Windows defect that was always
waiting here, or a behavioral difference introduced by switching the JIT's
object layer from RuntimeDyld to JITLink. **Attribution is open.** Against
that, 40 JIT-heavy tests ran clean under JITLink first, and the same test
passes on Linux/ELF (part of proton's 69/69 `XpuVulkan*` hardware sweep, and
Linux kept RuntimeDyld→JITLink defaults untouched by the fix).

**1.4 The discriminating experiment.** Run exactly this test on Windows
against a pre-fix binary (RuntimeDyld): a single test compiles a small host
module set and may well link without tripping the relocation abort. If it
hangs there too → pre-existing, debug the test's wait; if it passes → the
JITLink switch changed something this path depends on (symbol resolution
order, initializer running, TLS) and the fix needs refining rather than the
test. Runnable via the workflow's `filter` input once workflow_dispatch is
available, or a temporary filter push on `ci/device-tests`.

**1.5 What it masks.** The same "floor, not a number" situation the COFF
abort caused, one layer deeper: the Windows suite order stops here, so
everything sequenced after has still never run on Windows. It also burns the
runner: every push/nightly run's Windows leg will sit at the hang until the
6-hour ceiling kills it — red, honestly, but expensively.

**1.6 Non-goals.** A committed `--gtest_filter` exclusion — the same
principle as `windows-jit-coff-reloc` 1.9: a permanently-skipped hanging test
is how a defect gets forgotten. If a clean sweep is needed before the fix,
exclude it through the workflow's `filter` input for that run only. Also out
of scope here: a general per-test timeout harness — worth considering
separately (a hang that costs 6 runner-hours to observe is a poor reporter),
but not as a way to bury this defect.

## 2. Acceptance

- **2.1** `bundledVulkanCpuForcedToCpu` on Windows completes (pass or fail)
  in bounded time; no silent multi-hour hang.
- **2.2** The Windows device-tests leg reaches the end of the suite, so the
  4090 pass rate is finally a measured number.
- **2.3** The attribution question (1.4) is answered and recorded here,
  whichever way it lands.
- **2.4** The test still passes on Linux (proton's `XpuVulkan*` sweep stays
  green).
