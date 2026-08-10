# spirv-coop-mixed-tier-abort — defect (open; found during cajeta-llama Unit 2)

## 1. Definition

**1.1 Symptom.** JIT-compiling any program whose reachable kernel set contains
a cooperative-matrix kernel whose tiles straddle implementation tiers on the
Vulkan backend aborts the whole process inside LLVM:

```
[ RUN      ] NumpyOpsTests.linspaceMatchesNumpy
cajeta_test: .../llvm/include/llvm/Support/Casting.h:572: decltype(auto)
llvm::cast(From*) [with To = FixedVectorType; From = Type]: Assertion
`isa<To>(Val) && "cast<Ty>() argument of incompatible type!"' failed.

cajeta: SIGABRT caught — likely heap corruption or assertion.
```

An assertion, then SIGABRT — the process dies where it stands, in whatever
test happened to trigger the compile.

**1.2 Found.** 2026-08-10, on `proton` (Linux, AMD Radeon 8060S, `radv`),
running `NumpyOpsTests` against a build whose embedded stdlib carried a new
`Ewise` kernel with **bfloat16 A/B operand tiles and a float32 accumulator
tile**. Reproduced twice, deterministically, at the first test to compile the
kernel.

**1.3 Mechanism.** `SpirvKernelLowering::coopMatrixTier`
(`src/cajeta/xpu/vulkan/SpirvKernelLowering.cpp:807-819`) assigns bf16 tiles
`ImplTier::Portable` (no Vulkan driver exposes a bf16 cooperative-matrix
config) while f16/f32/int tiles are `Native`. The two tiers use different
LLVM representations — Portable tiles are `FixedVectorType`s, Native tiles are
`target("spirv.CooperativeMatrixKHR", ...)` opaque types. A single `mma` whose
operand tiles resolved Portable and whose accumulator resolved Native hands a
`TargetExtType` to code expecting a `FixedVectorType`, and `llvm::cast`
asserts.

**1.4 The AMD backend already handles this case gracefully.** The identical
tier mix on the AMD backend produces a diagnostic, not an abort:

```
[xpu-kernel-skipped] matmulF32: CooperativeMatrix.mma: a native accumulator
cannot consume software-tier operands — give all three tiles the same dtype tier
```

The Vulkan lowering has no equivalent mixed-tier guard. The comment at
`SpirvKernelLowering.cpp:810-811` shows the constraint is *known* ("its
accumulator must be bf16 too, so the whole GEMM stays one tier") — it is
documented but not enforced.

**1.5 Why it outranks its trigger.** The kernel that trips it does not have to
run, or even be launched — it only has to be *compiled*, and kernel lowering
compiles every reachable `@Kernel` for every registered backend. A stdlib or
user kernel with this shape therefore kills every program that touches its
class on any machine with a Vulkan device, in tests or production, regardless
of whether the bf16 path is ever exercised. And because it is an abort, gtest
exits 3 with *zero failed tests* recorded — the same dishonest-reporting shape
as `windows-jit-coff-reloc` 1.3.

**1.6 The mix is easy to write and reasonable-looking.** f16 operands with an
f32 accumulator is the *standard* WMMA recipe, native on Vulkan, AMD, and
NVIDIA — so writing bf16 operands with an f32 accumulator by analogy is the
natural next line. On AMD it is even native silicon
(`llvm.amdgcn.wmma.f32.16x16x16.bf16`). Only Vulkan's config gap makes it
mixed-tier, and only there does it abort.

**1.7 Reproduce.** Add to any `@Kernel`-bearing class:

```cajeta
@Kernel
public static void bad(KernelBuffer<float32> c, KernelBuffer<bfloat16> a,
                       KernelBuffer<bfloat16> b, uint32 rows, uint32 cols, uint32 depth) {
    CooperativeMatrix<float32,16,16,2> mc;   // Native tier on Vulkan
    mc.splat(0.0f);
    CooperativeMatrix<bfloat16,16,16,0> ma;  // Portable tier on Vulkan
    CooperativeMatrix<bfloat16,16,16,1> mb;
    ma.load(a, 0, 0, depth);
    mb.load(b, 0, 0, cols);
    mc.mma(ma, mb);
    mc.store(c, 0, 0, cols);
}
```

and JIT-compile the class on a machine with a Vulkan device. The 2026-08-10
occurrence was exactly this shape in `Ewise.matmulBf16` (since corrected to
all-bf16 tiles in the stdlib).

**1.8 Non-goals.** Making mixed-tier bf16/f32 *work* on Vulkan. Without
`SPV_INTEL_bfloat16_arithmetic` there is no native bf16 tile to promote, and
demoting the accumulator behind the author's back would silently change
numerics. The defect is the *abort*: the same situation must produce the
AMD-style skip diagnostic (or a compile-time error naming the kernel, the
tiles, and their tiers), leaving the process alive and the report honest.

## 2. Acceptance

- **2.1** When a kernel's cooperative-matrix tiles resolve to different tiers
  on the Vulkan backend, compilation produces a diagnostic naming the kernel
  and the offending tiles' dtypes and tiers — the process does not abort.
- **2.2** When such a kernel is skipped, every other kernel in the class still
  compiles and launches on Vulkan, and host execution proceeds.
- **2.3** The 1.7 repro, JIT-compiled on a Vulkan machine, runs to a normal
  exit with the diagnostic emitted.
- **2.4** All-one-tier kernels are unaffected: the f16(A/B)+f32(acc) native
  GEMM and the all-bf16 portable GEMM both still lower and pass their
  device tests on Vulkan.
