# Kernel byte-vector lowering — Spec

## 1. Definition

### 1.1 Purpose
`Vector<int8,N>` / `Vector<uint8,N>` are the working type of every quantized
kernel: nibbles are masked out of them, crumbs shifted into them, and scale
bytes picked from them by lane. The kernel lowering hands each of these
operations to LLVM as a `<N x i8>` instruction, and on amdgpu the backend
legalizes them one byte at a time — an `and <16 x i8>` becomes sixteen
`v_and_b16`, and `hv[j]` with a runtime `j` becomes a fifteen-deep
compare/select chain per byte. Measured in cajeta-llm unit 60 (plan
`cajeta-llm-plan.md` 60.1): the Q4_K wave mat-vec took 30 µs from cache,
the same as from DRAM — the kernel was ALU-bound on this legalization —
and a hand rewrite of eight kernels to word form halved their instruction
counts (3239 → 1709, 2572 → 1377) and took decode from 12.3 to 11.9 ms/tok.

This spec moves that rewrite into the compiler, so every byte-vector kernel
gets it, including the ones nobody rewrites by hand.

### 1.2 Scope
- Kernel lowering (`KernelLowering.cpp`, target-neutral): byte-vector
  bitwise operations and constant shifts emitted in word form; runtime-lane
  extraction from byte and half-word vectors emitted as a word extract and a
  shift.
- Host codegen: the slot an indexed vector read is returned through must be
  an entry-block alloca (cajeta-llm plan unit 37 — the same read, leaking
  stack per execution on the host).

### 1.3 Constraints
- 1.3.1 **Bit-identical.** Every rewritten form computes the same bytes as
  the `<N x i8>` instruction it replaces, on every input — including
  negative signed bytes. The proof is a CPU-oracle test against a host
  reference, not an argument.
- 1.3.2 **Target-neutral.** The IR shape is emitted once, in the shared
  lowering; the CPU, amdgpu, nvptx and SPIR-V backends all receive `<N/4 x
  i32>` ops. Word-width bitwise ops are native on all four.
- 1.3.3 **Source unchanged.** `(ql >> 4) & 15` stays the way a kernel is
  written. No new surface.

### 1.4 Non-goals
- Arithmetic shifts on signed byte vectors stay per-byte: a word-form
  sign-extension costs more instructions than the per-byte form it would
  replace. Only the `(v >> c) & k` shape, where the mask discards every
  sign-extended bit, is rewritten.
- Runtime-lane insertion (`v[i] = x`) is not rewritten.
- 16-bit vectors' bitwise ops are not rewritten (`v_pk_*_b16` already
  packs them two per instruction).

## 2. Word-form bitwise operations

- **2.1** When a kernel applies `&`, `|` or `^` to two `Vector<int8|uint8,N>`
  values with N a multiple of 4, the lowering emits the operation on `<N/4
  x i32>` and the result is the same bytes.
- **2.2** When a kernel shifts a byte vector left by a constant c in 1..7,
  the lowering emits `(w << c) & rep(0xFF << c)` on words.
- **2.3** When a kernel shifts an unsigned byte vector right by a constant c
  in 1..7, the lowering emits `(w >> c) & rep(0xFF >> c)` on words.
- **2.4** When a kernel masks an arithmetic right shift of a signed byte
  vector — `(v >> c) & k` with every bit of k below bit 8-c — the lowering
  emits the logical word-form shift and the word-form mask; the
  `ashr <N x i8>` is not emitted.
- **2.5** When a signed byte vector is shifted right and the result is used
  any other way, the lowering emits `ashr <N x i8>` as before.
- **2.6** A shift by zero emits no shift.

## 3. Runtime-lane extraction

- **3.1** When a kernel reads `v[i]` from a byte vector with a non-constant
  `i` and N a multiple of 4, the lowering extracts word `i >> 2` from the
  `<N/4 x i32>` view, shifts it right by `8 * (i & 3)` and truncates.
- **3.2** The same for 16-bit vectors with N a multiple of 2: word `i >> 1`,
  shift `16 * (i & 1)`.
- **3.3** A constant-lane read stays an `extractelement` with a constant
  index.

## 4. Host: the indexed-read slot

- **4.1** When host code reads `v[i]` (any index), the value slot it is
  returned through is allocated in the function's entry block; a read
  inside a loop does not grow the native stack per iteration.
- **4.2** The same for a matrix row read `m[r]`.
