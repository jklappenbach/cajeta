# GPU Matmul Profiling — Spec

## 1. Definition

### 1.1 Purpose
Extend the `samples/profile` cross-language benchmark suite with **GPU compute
benchmarks** that fairly and completely represent Cajeta's `@Kernel` GPU capability.
The first workload is **dense `float64` matrix multiply** run on real GPU backends,
benchmarked against optimized GPU peers across languages — the template for a growing
family of GPU computational / ML / DS benchmarks.

### 1.2 Problem
Cajeta's `@Kernel matmul` already lowers to four backends (CPU, NVPTX, AMDGPU, Vulkan)
and runs correctly on-device, but the profile suite benches **only the CPU kernel-pool
path**. The suite therefore *understates* Cajeta: it shows a single-core/CPU matmul
losing to numpy, while the measured reality on this box is:

| N | Cajeta GPU (HIP, gfx1151) | Cajeta CPU pool | crossover |
|---|---|---|---|
| 200 | 0.099 ms | 0.057 ms | overhead-bound (CPU wins) |
| 1024 | 9.94 ms | 25.1 ms | **GPU 2.5×** (compute-bound) |

Both `check=true`. A naïve single-accumulator kernel already hits ~216 GFLOP/s fp64 at
n=1024 — with headroom from multi-accumulator ILP + FMA. None of this is on the site.

### 1.3 Scope
- A GPU matmul benchmark wired into the suite, run on **HIP and Vulkan** (both
  first-class, separately reported), selected at runtime from one multi-backend build.
- A **problem-size sweep** (n ∈ {256, 512, 1024, 2048}) that exposes the
  overhead→compute crossover; the report headlines the size where the GPU clearly wins.
- A **multi-accumulator unroll** of the k reduction that breaks the latency chain so
  **FMA helps on the GPU path** (the codegen-perf-levers U2 NO-GO showed global FMA
  regresses the single-accumulator kernel; ILP is the prerequisite).
- **GPU competitors across languages** (e.g. rocBLAS via C++/HIP, CuPy or PyTorch via
  Python, a raw HIP kernel), vendored + version-pinned, emitting the same result schema,
  so the GPU bars are GPU-vs-GPU and honest.
- Report integration: a distinct GPU area with GFLOP/s throughput, backend-labeled rows,
  and a fidelity gate so Cajeta never silently drops from a tab.

### 1.4 Constraints
- **Hardware**: this box is AMD **gfx1151** (Radeon 8060S) with HIP (ROCm) + RADV
  (native Vulkan) + llvmpipe (software fallback). No NVIDIA — CUDA/cuBLAS measured rows
  are out of scope on this machine (kept compile-only).
- Reuse the existing `Benchmark`/`Harness`/`Result`/`report.py` machinery and the
  `cajeta.json` flavor mechanism; do not fork the pipeline.
- **Portability**: no hardcoded device id; gate cleanly (skip, not crash) when a backend
  or competitor library is unavailable, so the suite still runs on a CPU-only host.
- `float64`, `n % 8 == 0` (the kernel's vload<8> tiling); A = identity so `C == B` gives
  an exact, cheap cross-check at every size.

### 1.5 Non-goals
- NVIDIA/CUDA on-device measurement (no hardware); ML training or full models (later
  family members — this establishes the GPU-bench template only).
- Replacing the CPU matmul bench — it stays (it is the honest small-N / CPU story).
- A production GEMM (cache-blocking, shared-memory tiling, mixed precision) — the kernel
  optimization here is scoped to multi-accumulator ILP + FMA, not a full BLAS. Tiling
  (the bulk of rocBLAS/PyTorch's edge) is deferred to a stacked follow-up plan
  `gpu-matmul-tiling`; this plan's §5 narrows the gap and measures what remains.

---

## 2. GPU benchmark execution model

How a GPU `@Kernel` bench is built, selected, and recorded in the suite.

### 2.1 Requirements
- One build, many backends: build the GPU bench exe with
  `--xpu-backend=amdgpu,vulkan,cpu`; the active backend is chosen at **run time** via
  `CAJETA_XPU_BACKEND` (hip | vulkan | cpu). No per-backend rebuild.
- Each result row records **which backend produced it** (see §6 schema), so HIP and
  Vulkan rows are distinguishable and groupable.
- A GPU bench self-reports device availability and **skips** (status=skipped, with a
  reason) rather than failing when the selected backend has no device.

### 2.2 Use cases
- **2.2.1** As the bench driver, when I build the GPU flavor once and run the exe with
  `CAJETA_XPU_BACKEND=hip` then `=vulkan`, then I get one labeled result set per backend
  from the same binary.
- **2.2.2** As the harness, when the GPU matmul runs, then it records the active backend
  name on each row so the report can separate HIP and Vulkan.
- **2.2.3** As a CPU-only host with no GPU, when I run the GPU bench, then it emits
  `status=skipped` rows (reason "no <backend> device") and the suite completes.
- **2.2.4** As the bench author, when I validate a GPU run, then `checkResult()` confirms
  `C == B` (A=identity) at each size on each backend, so a miscompiled kernel is caught.

---

## 3. Backend coverage (HIP ships; Vulkan deferred)

> **Status (2026-06-26):** HIP is the shipping backend. Vulkan is **deferred** to the
> `gpu-vulkan-f64` investigation plan after two findings: (1) the 8-wide tile won't
> compile to shader SPIR-V (no >4-component vector type; the `cv` accumulator is a
> loop-carried `<8 x f64>` phi the backend can't split — [[reference_spirv_8wide_f64_legalize_fail]]),
> and (2) a 4-wide kernel *compiles and runs on RADV on-device* but returns the wrong
> result (`check=false`) — a separate Vulkan f64 correctness bug. The driver/hardware are
> capable (the shader executes); the gaps are compiler-side.

### 3.1 Requirements
- The kernel lowers to and runs on **AMDGPU (HIP)** on gfx1151, producing first-class
  reported rows (the shipping backend).
- **Vulkan (SPIR-V)** coverage is gated on the `gpu-vulkan-f64` plan; until it lands,
  the suite reports HIP only and does not build the Vulkan target.
- Correctness (`check=true`) is required on every benched size on the shipping backend;
  a backend that fails correctness is reported as `invalid`, not silently dropped.

### 3.2 Use cases
- **3.2.1** As a viewer, when I open the GPU matmul page, then I see separate Cajeta-HIP
  and Cajeta-Vulkan bars, each labeled with its backend.
- **3.2.2** As the author, when Vulkan compute lacks an fp64 feature or a kernel
  construct (XPU-N01), then the Vulkan row is marked skipped/invalid with the reason,
  and HIP still reports — partial coverage degrades gracefully.

---

## 4. Problem-size sweep & crossover

### 4.1 Requirements
- The bench runs a sweep n ∈ {256, 512, 1024, 2048} (each `% 8 == 0`), one labeled row
  per (backend, size), via the harness variant mechanism.
- The report headlines the **crossover** — the smallest size at which the GPU clearly
  beats the Cajeta CPU pool — and shows the full sweep so the overhead-bound small-N
  regime is visible (honest: the GPU is not a free win at every size).

### 4.2 Use cases
- **4.2.1** As a viewer, when I read the sweep, then I see GPU lose at n=256 (overhead-
  bound) and win at n≥1024 (compute-bound), with the crossover called out.
- **4.2.2** As the author, when n=2048 exceeds device memory or time budget, then that
  size skips with a reason rather than hanging the suite.

---

## 5. Kernel optimization — multi-accumulator unroll + FMA

**Goal (scoped honestly):** *narrow* the gap to the GPU competitors (PyTorch ROCm is
~2.1× ahead at n=1024), not match them. The bulk of rocBLAS/PyTorch's edge is
shared-memory + register **tiling**, which is a **non-goal here** (§1.5) and is recorded
as a stacked follow-up plan `gpu-matmul-tiling` that chases PyTorch as its own work. This
unit delivers the ILP+FMA win and an honest measured gap; closing it is the follow-up.

### 5.1 Requirements
- Split the single `cv` k-reduction into **N independent accumulators** (unroll the k
  loop by N, N ∈ {2,4} tuned by measurement) so the latency chain is broken and the loop
  becomes throughput-bound — the precondition for FMA to help.
- Confirm FMA is emitted on the GPU path for the unrolled kernel and measure the
  speedup vs the single-accumulator baseline (A/B at the compute-bound sizes).
- Record the **remaining gap to PyTorch/rocBLAS** after this unit (the headroom the
  `gpu-matmul-tiling` follow-up targets) — do not claim parity.
- FP results stay within tolerance / the exact `C == B` check still holds (A=identity
  makes the multiply exact regardless of accumulation order).

### 5.2 Use cases
- **5.2.1** As the author, when I unroll the k loop into multiple accumulators on the GPU
  kernel, then the compute-bound sizes (n≥1024) speed up measurably over the single-`cv`
  baseline, and the result stays correct.
- **5.2.2** As the author, when FMA is enabled on the unrolled GPU kernel, then it is a
  win (unlike the global-FMA NO-GO on the single-accumulator kernel), demonstrating that
  ILP is the unlock.
- **5.2.3** As the author, when unroll factor N is varied, then the best N is selected by
  measurement and recorded, not assumed.

---

## 6. Result schema — backend labeling

### 6.1 Requirements
- Add a way to record the **GPU backend** on each result row so HIP, Vulkan, CPU, and
  each competitor library are distinguishable in the report. Preferred: a dedicated
  `backend` column (schema bump 1→2) populated for all rows (CPU-only benches = "cpu"),
  or, if a schema bump is undesirable, reuse `variant`/`library` consistently.
- Competitor rows record their library + version (`library`, `library_version`) — e.g.
  rocBLAS x.y, CuPy x.y — exactly as the existing CPU competitors do.

### 6.2 Use cases
- **6.2.1** As report.py, when I read a GPU result, then the backend is an explicit field
  I can group and color by, not parsed out of free-text flags.
- **6.2.2** As a maintainer, when I add a new backend or GPU competitor later, then it
  slots into the same `backend`/`library` fields with no schema change.

---

## 7. GPU competitors

### 7.0 Confirmed environment (2026-06-26)
The Python-GPU peer is **validated working** on this box: PyTorch ROCm
`2.12.0a0+rocm7.13.0a20260411` in the pinned venv `ml/venv-rocm-gfx1151` runs fp64
`A @ B` on gfx1151 — n=1024 = **4.75 ms / 452.6 GFLOP/s, check=True**. (The shared
`ml/venv-rocm7.x` is an Oct-2025 build that segfaults on any GPU op — ROCm #5853 — and is
NOT used; see [[reference_python_gpu_torch_gfx1151_venv]].) For reference, Cajeta's naïve
single-accumulator HIP kernel is 9.94 ms (~216 GFLOP/s) — PyTorch is ~2.1× ahead, the
optimization headroom §5 targets. **Python-on-GPU is the headline comparison** (the
developer's priority), with rocBLAS as the second GPU peer.

### 7.1 Requirements
- **Python GPU (headline)**: PyTorch ROCm `A @ B` on gfx1151, pinned to the
  `ml/venv-rocm-gfx1151` venv (torch + rocm versions recorded in `library`/
  `library_version`), fp64, same sizes, same `C == B` check, per-iteration anti-DCE.
- **rocBLAS (second GPU peer)**: C++/HIP `rocblas_dgemm`, vendored + version-pinned,
  the optimized-GPU reference bar. Optionally a raw HIP kernel and a Vulkan-compute
  competitor as additional points.
- All GPU competitors run through a `competitors/gpu.sh` runner and emit the same CSV
  schema (with the `backend` column from §6) as the existing CPU competitors.
- **CUDA/cuBLAS is out of scope** on this box (no NVIDIA) — kept compile-only.
- Competitors that need an absent library/device **skip cleanly** (no failed suite); a
  CPU-only host gets skip rows, not crashes.

### 7.2 Use cases
- **7.2.1** As a viewer, when I read the GPU matmul page, then Cajeta-HIP is compared
  against rocBLAS and a Python GPU baseline at the same sizes — GPU-vs-GPU, fair.
- **7.2.2** As the driver, when rocBLAS or CuPy is not installed, then those competitor
  rows are skipped with a reason and the Cajeta rows still publish.
- **7.2.3** As a reviewer, when I audit fairness, then each competitor does the same N³
  fp64 work with its own per-iteration anti-DCE and the same checksum cross-check.

---

## 8. Report & fidelity

### 8.1 Requirements
- Register the GPU area (proposed name `gpu`) in `report.py`: `AREA_THR_UNIT["gpu"] =
  "GFLOP/s"`, with `math_flops` extended (or reused) for matmul at the swept sizes.
- The site presents GPU rows grouped/colored by backend and language, headlines the
  crossover size, and keeps the Time / Throughput / Memory tabs consistent.
- The **fidelity gate** asserts Cajeta (HIP and Vulkan where available) appears on every
  tab for the GPU area — no silent omission (the failure mode the GFLOP/s bug had).

### 8.2 Use cases
- **8.2.1** As report.py, when the GPU area is generated, then GFLOP/s is derived for
  Cajeta GPU rows (no throughput column needed) exactly like the CPU math area.
- **8.2.2** As the fidelity gate, when a GPU run is published, then it fails the build if
  a Cajeta GPU row is missing from any tab it should appear on.
- **8.2.3** As a maintainer, when I add the next GPU bench (e.g. a conv or GEMV for the
  ml/ds family), then the area/unit registration and backend labeling already generalize.
