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
        └── XpuTour.cajeta   ← @Kernel SAXPY + vecAdd, dispatched at runtime
```

## Run it

The compiler must be built first (`cd <repo> && ./build.sh`). Then:

```sh
./run-xpu.sh                 # default: --xpu-backend=cpu — runs anywhere, no GPU
./run-xpu.sh amdgpu,cpu      # use the AMD GPU (HIP), fall to CPU if absent
./run-xpu.sh vulkan,cpu      # use a Vulkan device (SPIR-V), fall to CPU if absent
./run-xpu.sh nvptx,cpu       # use the NVIDIA GPU (CUDA), fall to CPU if absent
```

Expected output (identical on every backend — the kernels are data-parallel):

```
=== Cajeta XPU tour ===
-- SAXPY: y = 2*x + y, x[i]=i, y[i]=1 --
  y[0]=1 y[1]=3 y[10]=21 y[255]=511  (expect 1, 3, 21, 511)
-- vecAdd: c = a + b, a[i]=i, b[i]=2*i --
  c[1]=3 c[10]=30 c[255]=765  (expect 3, 30, 765)
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
and routes the whole orchestration — `Buffer.allocate` / `upload` /
`kernel.launch` / `Stream.sync` / `download` — to it. If nothing is available it
prints a precise *"no available backend among {…}; rebuild with `cpu`…"*
diagnostic instead of crashing (explicit-only bundling is a build-time contract).

## The kernels

Both kernels in `XpuTour.cajeta` are **data-parallel and barrier-free / wave-free**,
so they produce identical results on every backend:

- `saxpy(y, x, a, n)` — `y[i] = a*x[i] + y[i]`, the canonical accelerator
  "hello world".
- `vecAdd(c, a, b, n)` — `c[i] = a[i] + b[i]`, element-wise.

`block = 64` is used because Vulkan bakes its workgroup size into the SPIR-V at 64;
CUDA / HIP / CPU accept it too. Workgroup barriers and true wave reductions on the
CPU are later increments, so demos that need them aren't included here yet.

See `cajeta-cpu.md`, `cajeta-xpu.md`, and `cajeta-docs/CajetaXPU.md` for the
backend/dispatcher design.
