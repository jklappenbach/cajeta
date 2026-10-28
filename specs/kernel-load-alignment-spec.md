# Kernel load alignment

Status: **approved** 2026-10-27 (Julian). Origin: cajeta-llm plan 15.2.19.8 and 15.2.19.10.
Decided with Julian 2026-10-27: the proof and a stated form both, spelled
`align:`, checked in debug and ubTraps builds, the llm masks removed after.

## 1. Definition

### 1.1 Problem

A `KernelBuffer<T>.vload<N>(i)` or `vstore` reaches each backend with only
element alignment, which for `int8` is one byte. On Vulkan the SPIR-V lowering
emits N scalar loads. Mesa merges them into wide loads but cannot prove the
address aligned, so ACO loads `addr & ~3` as a b128 plus a b64 and rebuilds the
vector with `v_alignbyte`/`v_perm`. In the llm's Q8_0 Cols mat-vec that was 99
alignbyte and 72 perm per loop, about a quarter of the loop's instructions.

Masking the index by hand (`o & -16`) lets Mesa see the alignment. With the
scale fold, that took Vulkan 8B Q2_K 4-seq decode from 146 to 226 tok/s against
llama.cpp's 188 (llm 9e8bb13). The llm kernels now carry about 60 masks
(`QuantKernel.a16`). They read like arithmetic but are promises, and nothing
checks them.

### 1.2 What exists

The NVPTX lowering proves alignment from the index expression
(`NvptxWordFormTests`): products and shifts by constants, sums of aligned
terms, and loop-carried values whose start and step are both aligned. It widens
byte loads to word or 16-byte vector loads only where the proof holds, and
refuses at launch a buffer whose base breaks it. Vulkan and AMDGPU have no
proof.

### 1.3 Scope

How a kernel's vector load and store alignment is known and lowered on every
device backend:

- the alignment the compiler proves from the index (§2);
- an alignment the author states where the proof cannot reach (§3);
- the run-time check of a stated alignment (§4);
- the buffer base the proof and the statement both depend on (§5).

### 1.4 Non-goals

- Host (CPU) vector loads. The cpu XPU backend takes the stated form and the
  check (§3, §4) but needs no wide-load lowering.
- Changing any buffer's layout, including the Q8_K pack.
- Making unaligned loads faster. An unaligned load stays correct and pays the
  realignment.
- Scalar subscripts (`buf[i]`).

## 2. Proven alignment

The NVPTX proof becomes shared and drives every device backend's load and
store lowering.

- **2.1** When a vload or vstore index is provably a multiple of A bytes, it is
  emitted at alignment A on NVPTX, AMDGPU and Vulkan. On Vulkan, ACO issues
  the access without realignment (no `v_alignbyte`/`v_perm` for it).
- **2.2** When nothing proves the index aligned, the access stays correct at
  element alignment.
- **2.3** When the index is a `select` or an `if` merge of aligned values, it
  is proven aligned to the smaller of the two.
- **2.4** When the index is masked by hand (`i & -16`), it is proven aligned
  to the mask, so existing masks stay correct.
- **2.5** When a proof credits more alignment than the arithmetic has, a test
  catches it. Each backend has a "does not fire" case for an unprovable index,
  as NVPTX has today.
- **2.6** When the proven alignment exceeds 16 bytes, it is used as 16.

## 3. Stated alignment

- **3.1** When an author writes `buf.vload<N>(i, align: A)` or
  `buf.vstore(i, v, align: A)`, the access is emitted at alignment A even
  where the proof cannot reach, for example an offset passed in as a
  parameter.
- **3.2** When A is not a constant power of two from 1 to 16, compilation
  fails naming the call.
- **3.3** When A is smaller than the alignment the proof already has, the
  larger one is used.
- **3.4** When A is larger than the element size times N, compilation fails
  naming the call, since no access of that width needs it.
- **3.5** When `align:` appears outside a kernel or device function, the
  existing kernel-only error applies.

## 4. Checking a stated alignment

- **4.1** When a build has debug checks or ubTraps on and a stated alignment
  is violated at run time, the launch raises, naming the kernel, the call site
  and the index. The kernel does not trap: the violation is written to the
  kernel's fault record, the access proceeds at the index rounded down, and
  the runtime reads the record after the launch (Julian, 2026-10-28; a device
  trap would poison the CUDA context and lose a Vulkan device).
- **4.2** When a release build without ubTraps violates a stated alignment,
  nothing is checked, and the behaviour is the backend's misaligned access.
  On Vulkan it is not: the statement reaches Mesa as an index mask (the
  fork's SPIR-V backend writes no `Aligned` operand, Q5), so a violated
  statement reads or writes the elements at the index rounded down to a
  multiple of A. (Julian, 2026-10-28.)
- **4.3** When the check is on, a proven alignment (§2) adds no check. Only a
  statement the proof cannot confirm is checked.

## 5. Buffer base alignment

- **5.1** When a buffer or view handed to a launch has a base offset that
  breaks the alignment a kernel relies on, proven or stated, the launch is
  refused with a message naming the buffer, on every device backend, as NVPTX
  does today.
- **5.2** When a view (`wordView`, `halfView`, a slice) is made at an offset,
  its base alignment is the offset's, and 5.1 applies to it.

## 6. Adoption in cajeta-llm

- **6.1** When 2.1 holds on Vulkan, the llm's `a16` and `& -4` masks are
  removed, and a Vulkan ISA census of the mat-vec kernels shows the same
  aligned loads without them. Any mask the proof cannot replace becomes an
  `align:`.
- **6.2** When the masks are removed, Vulkan decode on the 8B quants measures
  within noise of the masked build.

## 7. Open questions

- **Q5** Whether the Vulkan lowering should emit the alignment as SPIR-V
  `Aligned` memory operands on wide loads instead of per-lane loads plus a
  mask. Either meets 2.1; the plan picks by measurement.
- **Q6** Whether 5.1's refusal needs a per-launch cost check on Vulkan, where
  buffers come from the runtime's own allocator.
