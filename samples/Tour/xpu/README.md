# Cajeta XPU tour

Portable `@Kernel` programs that run through the CajetaXPU **runtime backend
dispatcher**. The *same* source compiles for NVIDIA (NVPTX → CUDA), AMD (AMDGPU
→ HIP), Vulkan (SPIR-V), or the **CPU** — the backend is a build-time
`--xpu-backend=` choice, and when several are bundled the runtime picks the best
one available at launch (`CUDA → HIP → Vulkan → CPU`), falling to the CPU when no
accelerator is present.

This is the companion to the language tour one folder up (`samples/Tour/`, the
stdlib / language-feature walkthrough). It lives in its own subfolder because XPU
programs need the `--xpu-backend` flag and a device-or-CPU-fallback to run.

```
samples/Tour/
├── src/tour/          ← the stdlib / language tour (build-bin.sh, build-uber.sh)
└── xpu/               ← you are here
    ├── README.md
    ├── run-xpu.sh     ← compile + (optionally) run the XPU tour
    └── src/tourxpu/
        └── XpuTour.cajeta   ← @Kernel SAXPY + vecAdd + waveReduce, dispatched at runtime
```

## Run it

The compiler must be built first (`cd <repo> && ./build.sh`). Then:

```sh
./run-xpu.sh                 # default: --xpu-backend=cpu — runs anywhere, no GPU
./run-xpu.sh amdgpu,cpu      # use the AMD GPU (HIP), fall to CPU if absent
./run-xpu.sh vulkan,cpu      # use a Vulkan device (SPIR-V), fall to CPU if absent
./run-xpu.sh nvptx,cpu       # use the NVIDIA GPU (CUDA), fall to CPU if absent
```

Expected output — the data-parallel results are identical on every backend; the
**wave width is hardware-specific and queried at runtime**, so the same source
reports 16 on an AVX-512 CPU, 8 on AVX2, and 32/64 on a GPU:

```
=== Cajeta XPU tour ===
-- SAXPY: y = 2*x + y, x[i]=i, y[i]=1 --
  y[0]=1 y[1]=3 y[10]=21 y[255]=511  (expect 1, 3, 21, 511)
-- vecAdd: c = a + b, a[i]=i, b[i]=2*i --
  c[1]=3 c[10]=30 c[255]=765  (expect 3, 30, 765)
-- transform: out = M*p + t, M=rot90, p[i]=(i,1), t=(100,200) --
  (i=10) -> (99, 210)  (i=255) -> (99, 455)  (expect (99,210) and (99,455))
-- mask/select: branchless per-lane conditionals --
  ReLU  (v>0).select(v,0):  sum[i=0]=3 sum[i=10]=11  (expect 3, 11)
  prune (w>1).select(w,0):  sum[i=0]=0 sum[i=10]=10  (expect 0, 10)
-- transforms: quaternions + determinant/inverse --
  quaternion (rotate+compose+conjugate+length) sum = 4   (expect 4)
  matrix (det + g*g^-1 diag + solve x) sum = 15   (expect 15)
-- precision & dtypes: @FastMath / vectorized math / fp16+bf16 --
  @FastMath  2*i+1:   [10]=21 [100]=201  (expect 21, 201)
  vectorized sqrt sum: [0]=14 [10]=24  (expect 14, 24)
  fp16+bf16 (h2.w+b2.y): [0]=48 [10]=58  (expect 48, 58)
-- coopGemm: 16x16x16 tile matmul on the matrix cores --
  C[0][0]=16 C[5][5]=16  (expect 16, 16)
  staged (via LDS): C[0][0]=16 C[5][5]=16  (expect 16, 16)
-- waveReduce: sum across each wave, in[i]=1 --
  wave width (queried, not hardcoded) = 16        # 64 on an AMD GPU, 32 on NVIDIA
  every lane of a wave agrees: sums[0]=16 sums[1]=16
=== xpu tour complete ===
```

### Knobs

| Variable | Effect |
|----------|--------|
| positional arg / `XPU_BACKEND=<list>` | backends to bundle (comma-separated). Default `cpu`. |
| `RUN=0` | compile + link only, don't execute (`RUN=0 ./run-xpu.sh amdgpu,cpu`). |
| `CAJETA_XPU_BACKEND=<one>` | force the runtime's choice at execution time. Bundle `amdgpu,cpu` then force `cpu` to prove the fall-to-CPU path on a box that *has* the GPU. |
| `DEBUG=1` | keep symbols/debug info in the linked binary. |
| `CAJETA_BIN`, `CLANG_BIN` | override the compiler / linker paths. |

## How it works

`run-xpu.sh` mirrors `../build-bin.sh`, plus `--xpu-backend`:

1. `cajeta --emit=obj --xpu-backend=<list>` — compiles each module to a native
   `.o`, and for every selected backend embeds the device kernels, the
   per-kernel registration constructors, and the **bundled-backend manifest**.
2. `clang` links the `.o` files (user code + the embedded runtime) into a binary.
   None of `libcuda` / `libamdhip64` / `libvulkan` is a link-time dependency —
   the runtime `dlopen`s them on demand, so the binary links and runs on a box
   without any of them (and the dispatcher falls to the CPU if `cpu` was bundled).
3. Run it (unless `RUN=0`).

At first device touch the runtime picks the active backend among the **bundled**
set (the manifest) ∩ the **available** set (it probes each), caches the choice,
and routes the whole orchestration — the `Buffer<T>` constructor (allocate) and
destructor (free), `upload` / `kernel.launch` / `Stream.sync` / `download` — to
it. `Buffer<T>` is RAII: `heap Buffer<T>(n)` allocates device memory and the drop
chain frees it at scope exit, so the demos never call `allocate()`/`free()` (a
launch-borrowed buffer that would drop before `Stream.sync()` is a compile error,
XPU-K02). If nothing is available it
prints a precise *"no available backend among {…}; rebuild with `cpu`…"*
diagnostic instead of crashing (explicit-only bundling is a build-time contract).

## The kernels

`XpuTour.cajeta` has three data-parallel kernels (identical results everywhere)
and one wave-cooperative kernel (correct everywhere, at the hardware's wave width):

- `saxpy(y, x, a, n)` — `y[i] = a*x[i] + y[i]`, the canonical accelerator
  "hello world". Uses **`heap Buffer<T>(n)`**.
- `vecAdd(c, a, b, n)` — `c[i] = a[i] + b[i]`, element-wise. Uses
  **`stack Buffer<T>(n)`**.
- `transform(outx, outy, px, py, m, tx, ty, n)` — `out[i] = M·p[i] + t`, a 2-D
  affine transform that showcases the **intrinsic linear-algebra value types on
  the device**: a `Matrix<float32,2,2>` passed **by value** as a kernel parameter
  (marshalled like a POD — the host packs its 4 floats, the device reads them
  back), `Matrix * Vector` (matrix-vector multiply, *not* element-wise), and
  `Vector + Vector`. The matrix lowers to a `<4 x float>` and the vector to a
  `<2 x float>` on every backend, and the matVec is honest extract/insert/fma IR,
  so the result is **bit-exact on CPU, Vulkan, and AMD** (verified on RADV +
  gfx1151). `Matrix<T,R,C>` also supports `m[r][c]`, `*`=matmul, `+ - /`
  element-wise, comparisons (`== != < <= > >=`), and `transpose`/`identity`/
  `row`/`col`/`hadamard` in a kernel.
- `reluReg` / `pruneReg` — **branchless per-lane conditionals (mask → select)**.
  A comparison on a `Vector`/`Matrix` yields a per-lane `<N x i1>` mask;
  `mask.select(a, b)` blends per lane (`mask[i] ? a[i] : b[i]`) with no branch —
  a vector ReLU `(v > 0).select(v, 0)` and a matrix weight-prune
  `(w > 1).select(w, 0)`. `.all()`/`.any()` reduce a mask to a `boolean`. The
  per-element decision stays in flat `<N x T>` ops, so it never diverges the warp.
  Full walkthrough + the alternatives it replaces: `cajeta-docs/MaskSelect.md`.
- `quatk` / `linalgk` — **3-D transform algebra on the device**. `quatk` runs a
  `Quaternion<float32>` per thread: rotate a vector (`q * v`), compose rotations
  (`q1 * q2` = Hamilton product), inverse-rotate (`conjugate()`), and `length()`.
  `linalgk` runs a `Matrix<float32,2,2>` per thread: `determinant()`, `inverse()`,
  and an inverse-based solve `g⁻¹ · rhs`. Both are bit-exact on CPU, Vulkan, and
  AMD. Walkthroughs: `cajeta-docs/Quaternions.md`,
  `cajeta-docs/MatrixDeterminantInverse.md`.
- `coopGemm` — **the matrix cores + the tiering fallback**. One 16×16×16 tile
  matmul `C = A·B` through the `CooperativeMatrix<T,R,C,Use>` verbs (load A and B
  tiles, zero the accumulator, one `mma`, store C) — A,B are `float16`, the
  accumulator/result `float32` (the mixed-precision ML config). With A,B all-ones
  each C element is the inner dim K=16. The **same** `@Kernel` takes the fastest
  path each backend offers: `float16`/`float32` lowers to the **native** matrix
  cores on Vulkan (RADV cooperative-matrix) **and on AMD** (RDNA3
  `v_wmma_f32_16x16x16_f16`, device-verified on gfx1151 — and `bfloat16` is
  native WMMA there too, `v_wmma_f32_16x16x16_bf16`), and to a **portable
  software tile-matmul** on the CPU (and any backend with no matrix config for
  the dtype — e.g. `bfloat16` on Vulkan). The software path is
  bit-identical; when it is taken the compile step prints a sticky
  `note: [mma-tiering]` (a severity below *warning* — it tells you the tier
  without dissuading use, and the path auto-promotes to the cores where the
  hardware exposes the config, e.g. bf16 WMMA on AMD). Walkthrough:
  `cajeta.xpu.core.CooperativeMatrix` + `cajeta-docs/LintRules.md` § Notes.
- `coopGemmStaged` — **the LDS-staged variant**. The same 16×16 tile matmul, but
  the A and B tiles are first staged into workgroup-shared memory (LDS) by the
  whole workgroup via `CoopStage.panel`, published with a `Barrier.workgroup`, and
  read back through the `CooperativeMatrix.load(Shared<T>)` overload — the building
  block of a bandwidth-efficient GEMM (a staged panel is reused across waves /
  K-steps, turning N redundant global reads into one global read + N cheap LDS
  reads). Portable: native LDS + matrix cores on the GPUs, a per-block stack buffer
  + the software tile-matmul on the CPU (barrier via loop fission). Making this
  work on Vulkan/RADV took three fixes: correcting `Workgroup.dimX()` codegen (it
  emitted the `WorkgroupSize` BuiltIn as a variable — invalid Vulkan SPIR-V), and
  two SPIR-V backend fixes in the cajeta-llvm fork (array-global typing +
  access-chaining an aggregate cooperative-matrix pointer to element 0; see
  `cajeta-llvm/UPSTREAM-PRS.md`). Larger tiled forms (K-loop, M/N output tiling
  across waves) are device-verified in
  `test/xpu/XpuCooperativeMatrixAmdDeviceTests.cpp`.
- `fastMath` / `vecMath` / `floatTypes` — **the device math surface**. `fastMath`
  is a `@FastMath` kernel (relaxed IEEE FP: FMA fusion, approximate
  transcendentals). `vecMath` applies `Math.sqrt` **elementwise** over a
  `Vector<float32,4>`. `floatTypes` does `float16` and `bfloat16` vector
  arithmetic — the 16-bit ML dtypes, both portable (bf16 is a storage format
  computed in f32, so it runs on RADV too). Surface: `cajeta.lang.Math`.
- `waveReduce(sums, in, n)` — `sums[i] =` the sum of `in` across `i`'s **wave**
  (the warp/wavefront/subgroup on a GPU; the SIMD vector on the CPU — Inc 5C).
  Written **width-agnostically**: it queries the environment (`Wave.reduceSum`,
  and `Wave.width()` / `Wave.laneId()` / `Wave.isFirstLane()` are available) and
  never hardcodes a wave size, so the same source is correct whether the wave is
  16 (AVX-512), 8 (AVX2), 32 (NVIDIA), or 64 (AMD). With all-ones input each
  lane's wave-sum *is* the wave width, so the demo prints the width it discovered.

The two demos deliberately use the two `Buffer<T>` forms. `Buffer<T>` is RAII —
the constructor allocates device memory, `~Buffer()` frees it via the drop chain
at scope exit, and `#buf` transfers ownership. The handle is a fixed 24-byte
struct `{ptr, deviceHandle, elementCount}`; the element count sizes the *device*
allocation, which lives off-stack — so `stack Buffer<T>(n)` reserves only the
handle on the frame (like a `std::vector` / Rust `Vec` header on the stack over
off-stack data) and is the cheaper, preferred form for the common same-scope
case. `heap` is for handles that must outlive the frame (returned/stored/moved).
A launch-borrowed buffer that would drop before `Stream.sync()` is a compile
error (XPU-K02) for either form.

`block = 64` is used because Vulkan bakes its workgroup size into the SPIR-V at 64;
CUDA / HIP / CPU accept it too. Workgroup barriers on the CPU are a later
increment, so demos that need them aren't included here yet.

See `cajeta-cpu.md`, `cajeta-xpu.md`, and `cajeta-docs/CajetaXPU.md` for the
backend/dispatcher design.
