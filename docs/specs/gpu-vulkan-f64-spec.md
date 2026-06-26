# GPU Vulkan f64 — Investigation Spec

## 1. Definition

### 1.1 Purpose
Investigate and resolve the two compiler-side blockers that keep Cajeta's f64 `@Kernel`
matmul from running correctly on the **Vulkan/SPIR-V** backend (gfx1151 / RADV), so the
`gpu-matmul-profiling` suite can add a Vulkan row alongside HIP. Decide Vulkan's fate
from evidence: fix it, or document it as unsupported with a precise reason.

### 1.2 Problem (established 2026-06-26)
The HIP path works (correct, fast). Vulkan does not, for two independent reasons:
- **8-wide won't compile.** The matmul's `cv` accumulator is a loop-carried `<8 x f64>`
  phi. Shader SPIR-V has no >4-component vector type, and the fork's SPIR-V backend can't
  split a wide vector carried through a loop. Bumping `MaxVectorSize` only moves the
  failure (G_UNMERGE → G_SHUFFLE_VECTOR → `allShaderVectors` type lists …); the backend
  assumes ≤4 components throughout, and forced 8-wide output would be spec-illegal for
  Vulkan (needs `Vector16`+`Kernel` caps). See [[reference_spirv_8wide_f64_legalize_fail]].
- **4-wide compiles + runs but is wrong.** A 4-wide kernel builds, and **RADV executes it
  on-device** (n=200, 0.22 ms) — so the driver/hardware are capable — but returns
  `check=false`. A separate Vulkan f64 correctness bug.

### 1.3 Scope
- Root-cause the 4-wide `check=false` (the nearer-term, likely-tractable bug).
- Characterize the 8-wide wide-vector-through-loops limitation and decide if a real
  SPIR-V fix (split wide vectors across phis/control flow) is worth it or out of scope.
- Evaluate the reverted `fewerElements` legalizer change (it fixed non-loop-carried
  >4-wide vectors, the `#170534` class) as a standalone **upstream LLVM PR**.
- Feed the outcome back to `gpu-matmul-profiling` (add a Vulkan row, or record it as
  unsupported with the reason).

### 1.4 Constraints
- gfx1151 + RADV (Mesa) + the fork LLVM SPIR-V backend (`cajeta-llvm`, branch
  `cajeta-spirv`). Fork-LLVM rebuilds are heavy; minimize cycles. HIP is the reference
  oracle for correctness (same math, known-good).

### 1.5 Non-goals
- Blocking `gpu-matmul-profiling` on this — HIP ships independently.
- A general SPIR-V wide-vector-through-control-flow implementation unless 1.3 finds it
  cheap and worthwhile.

---

## 2. The 4-wide correctness bug

### 2.1 Requirements
- Identify why the 4-wide f64 matmul returns `check=false` on RADV while HIP returns
  `check=true` from the same source.
- Candidate causes to rule in/out: `shaderFloat64` feature support/precision; the
  launch grid/threadgroup mapping on the SPIR-V backend (fixed local size 64); buffer
  binding / addrspace; an f64 SPIR-V codegen defect (vload/vstore/broadcast).

### 2.2 Use cases
- **2.2.1** As the investigator, when I run the 4-wide kernel on Vulkan and HIP with the
  same inputs, then I can localize the divergence (which output elements differ) and name
  the cause.
- **2.2.2** As the investigator, when the cause is identified, then either a fix makes
  Vulkan `check=true`, or it is recorded as a precise unsupported-reason.

## 3. The 8-wide wide-vector limitation

### 3.1 Requirements
- Document precisely why >4-component vectors can't be carried through a loop in shader
  SPIR-V, and what a correct fix would require (splitting wide values through phis).
- Decide GO/NO-GO on attempting that fix, with a cost/benefit (the 4-wide path already
  gives a working width once 2.x is fixed).

### 3.2 Use cases
- **3.2.1** As the investigator, when I evaluate the wide-vector fix, then I produce a
  GO/NO-GO with rationale, not an open-ended dig.

## 4. Upstream LLVM PR (#170534 class)

### 4.1 Requirements
- Reconstruct the reverted `fewerElements` fallback for G_BUILD_VECTOR / G_CONCAT_VECTORS
  / G_UNMERGE_VALUES (it made non-loop-carried >4-wide vectors legalize) and verify it
  against issue #170534's exact repro (size-6 G_BUILD_VECTOR).
- If valid, prepare an upstream PR with the required `Assisted-by: Claude (Anthropic)`
  disclosure ([[reference_llvm_ai_disclosure_required]]) and a terse one-line code comment
  with the issue reference (rationale in the PR, not the source —
  [[feedback_single_line_comments]]).

### 4.2 Use cases
- **4.2.1** As a contributor, when the fix passes #170534's repro + the SPIR-V test suite,
  then it's submitted upstream with the disclosure and a clean single-line comment.
