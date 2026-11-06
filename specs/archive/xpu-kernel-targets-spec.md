# xpu-kernel-targets — a kernel names the backends it is built for

## 1. Definition

### 1.1 Purpose
A `@Kernel` is lowered for every backend a build targets. A kernel written
for one class of device (a wave mat-vec, a cooperative-matrix GEMM, an
8-row CPU interleave kernel) is still lowered for every other backend, and
on the CPU it runs as emulated blocks when a router reaches it. This spec
lets a kernel declare the backends it is built for. Other backends do not
lower it, and a launch there fails by name instead of running emulated.

### 1.2 The problem, measured (cajeta-llm, 2026-11-05)
- 1.2.1 A CPU build of cajeta-llm's `SchedThroughput` takes about 532 s on
  proton. 170 s of it is per-kernel vectorisation, done serially. 17
  Q4_K/Q6_K GPU wave mat-vec kernels account for 123 s, and the CPU decode
  never routes a model projection to them. One of them,
  `q6kQ8WavePfMatVecKernel`, takes 31.7 s for 3,803 instructions.
- 1.2.2 Holding the 89 cooperative-matrix kernels off the CPU cut the same
  build from about 9:00 to 8:09. Holding 15 of the wave mat-vec kernels as
  well cut it to 6:01, a third off.
- 1.2.3 cabra's CI test build went from 11 minutes (llm 0.1.5) to over its
  30-minute limit (llm 0.1.6).
- 1.2.4 At runtime, three CPU paths for the Qwen2 72B ran GPU kernels as
  emulated blocks: the Mw8 GEMM, the GQA-8 prefill attend and the GQA-8
  decode attend. Load stalled for over 10 minutes, and prefill ran at 0.82
  of llama.cpp. Each was found by measurement, not by any diagnostic.
- 1.2.5 The only lever today is `@Unlowered(backend = "cpu", hold = true)`.
  It means "this backend cannot lower this, tracked by plan item X", not
  "built for other backends by design". One annotation per kernel carries
  one hold for every backend it lists. So a kernel already tracked on
  Vulkan cannot also be held off the CPU (16 such kernels in llm).

### 1.3 Constraints
- 1.3.1 The default is unchanged: a kernel with no declaration is built for
  every backend.
- 1.3.2 `@Unlowered` keeps its meaning: a lowering failure, tracked by a
  plan item, with stale detection.
- 1.3.3 A kernel's absence on a backend is visible to code: `KernelCap`
  (and its stdlib equivalent) answers "not here" without a launch.

### 1.4 Non-goals
- 1.4.1 Lowering on first use (JIT) on the CPU backend. Not chosen
  2026-11-05.
- 1.4.2 A per-backend hold inside `@Unlowered`. Not chosen 2026-11-05.
- 1.4.3 Deciding routes. The library's router keeps choosing kernels; this
  spec only removes the ones that are not built.
- 1.4.4 Not chosen 2026-11-05: a separate `@Targets` annotation,
  `@GpuKernel`/`@CpuKernel` shorthands, vendor classes (`"amd"`,
  `"nvidia"`), a compile-time rejection of an off-target launch, and no
  migration note.

## 2. Declaring targets

- 2.1 When a kernel declares its targets with `@Kernel(targets = {...})`, it
  is lowered and registered only for those backends. A target is `"cpu"`,
  `"gpu"`, or an exact backend name (`"vulkan"`, `"amdgpu"`, `"nvptx"`).
- 2.2 When a kernel declares no targets, it is built for every backend, as
  today.
- 2.3 When a kernel lists a backend the compiler does not know, the build
  fails naming the kernel and the backend.
- 2.4 When a kernel is declared for the GPU class, it is built for vulkan,
  amdgpu and nvptx, and for any GPU backend added later.
- 2.5 When a kernel is declared for the CPU only, it is built only for the
  cpu backend. This replaces today's
  `@Unlowered(backend = {"vulkan", "nvptx", "amdgpu"}, hold = true)` on
  cpu-only kernels.
- 2.6 When a kernel carries both a target declaration and `@Unlowered`,
  the two compose: `@Unlowered` may only name backends in the target set.
  Naming one outside it is an error, because that kernel is never
  attempted there.
- 2.7 When a templated kernel declares targets, every instantiation inherits
  them.

## 3. A kernel off its targets

- 3.1 When code asks whether a kernel is usable on a backend outside its
  targets, the answer is no, with the reason "not a target".
- 3.2 When a kernel is launched on a backend outside its targets, the launch
  fails at run time with an error naming the kernel and the backend. It does
  not run emulated. The backend is chosen at run time, so the check is made
  there.
- 3.3 When the kernel census runs on a backend, a kernel outside its targets
  is listed as OUT-OF-TARGET, not UNCOVERED. A test that value-checks it
  there is not expected.
- 3.4 When a build targets only backends outside a kernel's targets, the
  kernel contributes no code, no manifest entry and no compile time to that
  build.

## 4. Vectorisation cost (separate defect)

- 4.1 When a CPU kernel's body contains a prefetch loop, vectorising it costs
  in proportion to its size, as for the same kernel without the loop.
  Today `q4kQ8WavePfMatVecKernel` takes 19.4 s against 0.26 s for its plain
  twin.
- 4.2 When a kernel's vectorisation exceeds a budget, the trace names it and
  its time (`CAJETA_XPU_CPU_VECTORIZE_TRACE` already prints both).

## 5. Migration

- 5.1 When an `@Unlowered(..., hold = true)` holds every GPU backend, or
  holds the cpu backend, the compiler emits a note naming the kernel and
  suggesting `@Kernel(targets = ...)`. Nothing fails, and a library adopts
  the form when its toolchain pin allows.
