# nvptx-coop-bf16-fragment-abort — defect (fix in PR #3; found during cajeta-llama Unit 2)

*Filed 2026-08-10 as `spirv-coop-mixed-tier-abort`; renamed same day when the
repro relocated the defect — see 1.4.*

## 1. Definition

**1.1 Symptom.** JIT-compiling any program whose reachable kernel set contains
a cooperative-matrix kernel with **bf16 A/B operand tiles and an f32
accumulator tile** — the standard WMMA recipe — aborts the whole process
inside LLVM when the NVPTX backend is registered:

```
[ RUN      ] NumpyOpsTests.linspaceMatchesNumpy
cajeta_test: .../llvm/include/llvm/Support/Casting.h:572: decltype(auto)
llvm::cast(From*) [with To = FixedVectorType; From = Type]: Assertion
`isa<To>(Val) && "cast<Ty>() argument of incompatible type!"' failed.

cajeta: SIGABRT caught — likely heap corruption or assertion.
```

An assertion, then SIGABRT — the process dies where it stands, in whatever
test happened to trigger the compile. The kernel does not have to launch, or
even be launchable: kernel lowering runs for every registered backend at
compile time, **ptxas present or not**, so the abort fires on machines with no
NVIDIA hardware at all.

**1.2 Found.** 2026-08-10, on `proton` (Linux, AMD Radeon 8060S, Vulkan via
`radv`, no NVIDIA), running `NumpyOpsTests` against a build whose embedded
stdlib carried `Ewise.matmulBf16`'s first shape (bf16 operands, f32
accumulator). Reproduced twice, deterministically, at the first test to
compile the kernel.

**1.3 Root cause — the NVPTX lowering assumes every A/B fragment has the f16
shape.** PTX packs two bf16 values per `.b32` register, so LLVM's bf16 WMMA
fragments are `{i32 x 4}` (`IntrinsicsNVVM.td` `"m16n16k16:a:bf16"`), while
f16 fragments are `{<2 x half> x 8}`. Three sites in
`src/cajeta/xpu/nvidia/NvptxKernelLowering.cpp` assumed the latter:

- `coopMatrixMulAdd` cast fragment element 0 to `FixedVectorType` to probe
  the scalar — for bf16 that element is a scalar `i32`, and a failed
  `llvm::cast` is an assert + `abort()`, not an exception: nothing upstream
  can catch it. **This is the crash site.**
- `nvFragScalar` returned the raw `i32`, silently re-selecting the **f16**
  load intrinsic for a bf16 tile.
- `coopMatrixSplat` inserted an unpacked bfloat into the `i32` register slot.

The bf16 native path had simply never been exercised: no test compiled a bf16
cooperative-matrix kernel for NVPTX before `Ewise.matmulBf16` existed.

**1.4 The false lead, recorded so it is not retried.** The crash was first
attributed to the SPIR-V backend — the crashing machine has only a Vulkan
GPU, and on SPIR-V the same kernel is *mixed-tier* (bf16 → Portable, f32
accumulator → Native), which looked like a plausible straddle-abort. The
repro written to confirm that theory **passed**: SPIR-V skips mixed-tier
kernels gracefully, in both straddle directions, with the
`[xpu-kernel-skipped]` note. The NVIDIA backend lowers kernels on that
machine too (before discovering ptxas is absent), and on NVPTX the shape is
all-**Native** — bf16 operands are a real tensor-core config — so lowering
entered the broken fragment code. Pinned to NVPTX by re-running the repro
with `Backend::Nvptx` alone: identical assert.

**1.5 Why it outranks its trigger.** A stdlib or user kernel with this shape
kills every program that touches its class on any machine where the NVIDIA
backend registers, in tests or production, regardless of whether the bf16
path is ever exercised — and bf16-operands-with-f32-accumulator is the
*natural* shape to write (native silicon on both NVIDIA and AMD). Because it
is an abort, gtest exits 3 with *zero failed tests* recorded — the same
dishonest-reporting shape as `windows-jit-coff-reloc` 1.3.

**1.6 Reproduce.** JIT-compile, with the NVPTX backend registered:

```cajeta
@Kernel
public static void bad(KernelBuffer<float32> c, KernelBuffer<bfloat16> a,
                       KernelBuffer<bfloat16> b, uint32 depth) {
    CooperativeMatrix<float32,16,16,2> mc;   // f32 accumulator — Native
    mc.splat(0.0f);
    CooperativeMatrix<bfloat16,16,16,0> ma;  // bf16 operands — Native on NVPTX
    CooperativeMatrix<bfloat16,16,16,1> mb;
    ma.load(a, 0, 0, depth);
    mb.load(b, 0, 0, depth);
    mc.mma(ma, mb);                          // ← cast<FixedVectorType> abort
    mc.store(c, 0, 0, depth);
}
```

No NVIDIA device or ptxas needed — the abort is at lowering time.

**1.7 Non-goals.** An LLVM/SPIRV toolchain update: the pinned fork's bf16
WMMA intrinsics are present and correctly shaped; the assert fires inside
`Casting.h` only because that is where `llvm::cast` checks. Also out of
scope: a native bf16-*accumulator* config (`wmma.bf16.16x16x16.bf16` exists
in RDNA3 silicon but is unwired on AMD and absent on NVIDIA PTX) — the
all-bf16 GEMM stays mixed-tier on NVPTX and takes the graceful skip.

## 2. Acceptance

- **2.1** The 1.6 repro JIT-compiles without aborting: the kernel lowers
  through the bf16 WMMA intrinsics (`{i32 x 4}` fragments), and on a
  ptxas-less machine is then skipped at the ptxas step — gracefully.
- **2.2** A splat on a bf16 A/B tile builds the packed `.b32` register image
  (`<2 x bfloat>` → `i32`), not an unpacked insert.
- **2.3** The all-bf16 GEMM (bf16 accumulator — mixed-tier on NVPTX) still
  takes the `[xpu-kernel-skipped]` note, as observed on the WSL/NVIDIA
  runner 2026-08-10.
- **2.4** SPIR-V's graceful mixed-tier skip is pinned in both straddle
  directions plus an all-one-tier control, so the false lead stays disproven.

**Status: fix + all four acceptance tests in PR #3
(`fix/nvptx-coop-bf16-fragments`, `NvptxCoopBf16Tests` +
`XpuSpirvMixedTierTests`, 6/6 green locally). Real-silicon validation on the
4090 rides `device-tests` once the fix lands on `ci/device-tests`.**
