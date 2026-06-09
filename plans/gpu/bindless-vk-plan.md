# Bindless / multi-buffer descriptor sets (VK) — plan

Stage B4's last item: let a `@Kernel` take an **array of buffers** bound as a single
Vulkan **descriptor array** and indexed in-kernel (`bufs[idx][i]`), instead of one
descriptor binding per buffer arg. The carrier for variable-count resource sets — a
scene of N buffers indexed at runtime — and the piece that makes Vulkan "fully formed."

## Key finding (2026-06-09): no fork-LLVM work needed

The `cajeta-spirv` fork already has the HLSL descriptor-indexing path:
- `OpTypeRuntimeArray` for runtime-sized descriptor arrays (`SPIRVGlobalRegistry.cpp:977`).
- `int_spv_resource_nonuniformindex` intrinsic + `NonUniformEXT` decoration
  (`SPIRVInstructionSelector.cpp:5352`, `selectResourceNonUniformIndex`).
- `llvm.spv.resource.handlefrombinding(set, binding, range, index, …)` — `range` =
  descriptor-array size, `index` = which descriptor.
- Triple is `spirv64-unknown-vulkan1.3-compute` → descriptor indexing is core in VK 1.2+.

So the work is entirely cajeta-side: kernel-arg model, launch marshalling, SPIR-V
codegen (use the array operands), and the Vulkan runtime descriptor-array binding.

## Surface

```
@Kernel
public static void gather(Buffer<int32>[] bufs, uint32 count,
                          Buffer<int32> out, uint32 n) {
    uint32 i = Thread.globalIdX();
    if (i < n) {
        int32 s = 0;
        for (uint32 b = 0; b < count; b = b + 1) { s = s + bufs[b][i]; }
        out[i] = s;
    }
}
// host:
Buffer<int32>[] bufs = heap Buffer<int32>[3];
bufs[0] = heap Buffer<int32>(n); bufs[0].upload(...); ...
gather.launch(s, grid, block)(bufs, 3, out, n);
```

`bufs[b]` selects descriptor `b` (the buffer handle); `[i]` indexes into it. The count
is passed explicitly as a scalar (the kernel/runtime needs the bound count).

## Increments

- **Inc 1 — kernel-arg model + admission.** `KernelParam::isBufferArray`
  (`LoweringTarget.h`); admit `Buffer<T>[]` in `isKernelArgAdmissible`
  (`KernelArgTrait.cpp`); classify it in `collectParams` (`KernelLowering.cpp`), reading
  the element type `T` off the array element like `Buffer<T>`. No behaviour change yet
  (no kernel uses it).
- **Inc 2 — launch marshalling.** In `CallExpression.cpp`, a `Buffer<T>[]` arg lowers to
  a runtime-built handle vector: `[i64 count, i64 h0 … i64 h(count-1)]` (read each
  element's `deviceHandle`); argv slot points at it. Launch-borrow each element.
- **Inc 3 — SPIR-V codegen.** `SpirvKernelLowering` `materializeParam`: a buffer-array
  param binds a **runtime descriptor array** (`handlefrombinding` with the array range).
  Two-level subscript: `bufs[b]` → `nonuniformindex(b)` + `handlefrombinding(index=b)`
  → a buffer handle; `[i]` → the existing `getpointer`. Emit test (OpTypeRuntimeArray +
  descriptor-array binding + spirv-val).
- **Inc 4 — Vulkan runtime.** In `cajeta_xpu_vk_launch`: for a buffer-array param read
  `count` from the argv slot, set the binding's `descriptorCount = count` (was hardcoded
  1 at `cajeta_runtime.c:7871,7975`), size the pool for it, and write `count` buffer
  descriptors into the one binding.
- **Inc 5 — CPU + tests + docs.** CPU: an array of buffers = an array of pointers
  (`bufs[b]` = the b-th handle, `[i]` = element) — portable test. `Buffer[N]` gather
  device test on Vulkan/RADV + a CPU test. Plan/matrix doc row.

## Scope / decisions

- **v1 backends: Vulkan (the real bindless) + CPU (portable test).** HIP/CUDA get the
  same via pointer-of-pointers — a follow-on (they don't need descriptor indexing).
- **Count is explicit** (a `uint32` arg), not inferred — the kernel iterates `0..count`.
  Descriptor array is runtime-sized (`OpTypeRuntimeArray`), so the SPIR-V is count-agnostic
  and the runtime binds `count` descriptors at launch.
- **Non-uniform index** decoration emitted (the index may be per-thread); correct + the
  spec-required decoration on VK 1.3.
- v1: arrays of **buffers** only (not textures/samplers — a follow-on); 1-D arrays.
