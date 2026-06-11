//
// LoweringTarget — the device-backend variance surface (cajeta-amd.md §2).
//
// The kernel-body lowerer (DeviceLowerer in KernelLowering.cpp) walks the
// @Kernel AST and emits device LLVM IR that is ~90% target-neutral: buffer
// GEPs in addrspace(1), shared globals in addrspace(3), the full int/float
// operator set, control flow, casts. Only a handful of decisions actually
// differ between NVIDIA and AMD — and THIS interface is exactly that list.
//
// It was extracted empirically by threading a second backend (AMDGPU)
// through the originally NVIDIA-only lowerer: the methods that ended up here
// ARE the measured NVIDIA∩AMD variance, not a guess. Anything NOT on this
// vtable stayed shared.
//
// Coordinate reads return an i32 value. globalId has a shared default
// (workgroupId*workgroupDim + threadId) since that identity holds on both
// backends; only the three leaf reads + barrier + kernel decoration fork.
//

#pragma once

#include <string>
#include <vector>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/AtomicOrdering.h"   // llvm::AtomicOrdering (atomic/fence seams)

namespace llvm {
    class Value;
    class Type;
    class Module;
    class Function;
    class IRBuilderBase;
}

namespace cajeta {
namespace xpu {

    class LoweringTarget {
    public:
        virtual ~LoweringTarget() = default;

        // Lowercase backend name (diagnostics).
        virtual const char* name() const = 0;

        // --- the degrade seam, named (cajeta-gpu inc-4 brick #4) -------------
        //
        // The internal face of the "impl-layer / SPIR-V-degrade framework"
        // (CajetaGPU.md §1.5): a capability has a Native lowering where the
        // silicon has it and a Portable degrade everywhere else, selected by a
        // capability heuristic with an explicit override (default ≠ law). Core
        // is just the in-tree "vendor library" that has a fallback for
        // everything — same shape an external vendor library will use, which is
        // why this concept is named here rather than re-invented per feature.
        //
        // Two core features answer through this one enum:
        //   - the coop-matrix VERB tier (coopMatrixTier — Native MMA vs the
        //     portable flat-tile matmul), and
        //   - the ray-query verb tier (rayQueryTier — native OpRayQuery vs the
        //     SoftwareRayQuery walk), derived from the noun's recorded impl.
        // The explicit override is the CAJETA_GPU_<FEATURE>_IMPL family
        // (resolveImplTier, this header's .cpp side; CAJETA_GPU_AS_IMPL is the
        // runtime-side instance for the AS noun in cajeta_runtime.c).
        //
        // Native and Portable are DIFFERENT realizations, not the same code two
        // ways — so for an arithmetic feature like coop-matrix they need not be
        // bit-identical (the hardware MMA and the triple-loop tile accumulate in
        // a different order); both are validated against the reference, not
        // against each other.
        enum class ImplTier { Native, Portable };

        // Memory-fence scope (Stage 9): the visibility/ordering reach of a
        // scoped memory fence (Barrier.workgroupMemory / .deviceMemory) — a
        // memory barrier WITHOUT the thread rendezvous of workgroupBarrier.
        // Workgroup = LDS/shared + global visibility within the block; Device =
        // global-memory visibility across the whole device.
        enum class FenceScope { Workgroup, Device };

        // User-selectable memory ordering for a kernel atomic / fence (mirrors
        // the cajeta cajeta.gpu.core.MemoryOrder enum — ordinals MUST match).
        // `Default` = no explicit order given: each backend keeps its
        // established default (Monotonic on the portable seam, AcquireRelease on
        // Vulkan). LLVM bakes ordering at IR-build time, so the value is always
        // a compile-time constant.
        enum class MemoryOrder {
            Relaxed = 0, Acquire = 1, Release = 2, AcqRel = 3, SeqCst = 4,
            Default = -1
        };

        // Map a user MemoryOrder to an LLVM AtomicOrdering; `Default` falls back
        // to `fallback` (the backend's established default). The CAS failure
        // ordering is derived from the success ordering per LLVM's rule (no
        // stronger than success; never Release/AcqRel).
        static llvm::AtomicOrdering toAtomicOrdering(MemoryOrder o,
                                                     llvm::AtomicOrdering fallback);
        static llvm::AtomicOrdering casFailureOrdering(llvm::AtomicOrdering success);

        // Address space for entry-block allocas (the mutable scalar-slot model
        // — loop counters, reassigned locals). NVPTX: 0 (generic). AMDGPU: 5
        // (private). Getting this wrong on AMDGPU is the classic first bug
        // (cajeta-amd.md §2) — an AS-0 alloca there is invalid.
        virtual unsigned allocaAddressSpace() const = 0;

        // True when kernel params arrive as DESCRIPTORS bound in the kernel body
        // (Vulkan: a no-param `void main()`, args via handlefrombinding), false
        // when they are real function arguments (CPU/NVPTX/AMDGPU: fn->getArg).
        // Drives the bindless buffer-array base: a descriptor-bound backend binds
        // per-access (no prologue value), an fn-arg backend takes the marshalled
        // [count, h…] handle array as fn->getArg.
        virtual bool descriptorBoundParams() const { return false; }

        // Leaf coordinate reads (dim 0/1/2 = x/y/z). Build into `b`'s current
        // insert point; insert any intrinsic decls into `m`.
        virtual llvm::Value* threadId(llvm::IRBuilderBase& b, llvm::Module& m,
                                      unsigned dim) = 0;     // local / workitem id
        virtual llvm::Value* workgroupId(llvm::IRBuilderBase& b, llvm::Module& m,
                                         unsigned dim) = 0;  // block / CTA id
        virtual llvm::Value* workgroupDim(llvm::IRBuilderBase& b, llvm::Module& m,
                                          unsigned dim) = 0; // block dim (ntid)

        // Global thread index. Default: workgroupId*workgroupDim + threadId,
        // which is correct on both backends. Virtual so a backend with a
        // native global-id intrinsic can override.
        virtual llvm::Value* globalId(llvm::IRBuilderBase& b, llvm::Module& m,
                                      unsigned dim);

        // Total work-items in dim (= gridDim·blockDim) — the grid-stride loop's
        // stride (Item 6). NVPTX: nctaid·ntid. AMDGPU: the HSA dispatch packet's
        // grid_size field (already the total). SPIR-V: NumWorkgroups·WorkgroupSize.
        // CPU: gx·bx, threaded through the coord ABI. Returns i32.
        virtual llvm::Value* gridSize(llvm::IRBuilderBase& b, llvm::Module& m,
                                      unsigned dim) = 0;

        // Workgroup barrier (synchronize all threads in the block, with the
        // memory ordering the backend needs for LDS visibility).
        virtual void workgroupBarrier(llvm::IRBuilderBase& b,
                                      llvm::Module& m) = 0;

        // Scoped memory fence (Stage 9): order/make-visible memory accesses at
        // `scope`, with NO thread rendezvous (unlike workgroupBarrier). `order`
        // selects the ordering (MemoryOrder.Default = AcquireRelease, the safe
        // fence default; Relaxed is treated as AcqRel since a relaxed fence is a
        // no-op). Default emits a system-scope acq_rel `fence` (the CPU oracle);
        // GPU backends override with the native op (SPIR-V OpMemoryBarrier,
        // AMDGPU scoped fence, NVPTX membar).
        virtual void memoryFence(llvm::IRBuilderBase& b, llvm::Module& m,
                                 FenceScope scope,
                                 MemoryOrder order = MemoryOrder::Default);

        // Decorate a freshly-created kernel function: calling convention +
        // any kernel-marker metadata. NVPTX: ptx_kernel CC + nvvm.annotations.
        // AMDGPU: amdgpu_kernel CC, no metadata.
        virtual void decorateKernel(llvm::Function* fn, llvm::Module& m) = 0;

        // Called once at kernel finalization IFF the body used a cross-lane
        // subgroup op (Wave.shuffle/ballot/reduce). A hook for backends that can
        // request maximal reconvergence — the guarantee that source-converged
        // lanes stay converged, so the subgroup op sees the lanes the source
        // implies. Vulkan sets the "enable-maximal-reconvergence" fn-attr
        // (→ OpExecutionMode MaximallyReconvergesKHR, SPV_KHR_maximal_
        // reconvergence). Default no-op: NVPTX/AMDGPU/CPU model wave-op
        // convergence through their own ISA semantics + LLVM convergence, with
        // no equivalent module-level mode to set.
        virtual void onSubgroupOpsUsed(llvm::Function* /*fn*/,
                                       llvm::Module& /*m*/) {}

        // --- kernel signature / parameter model (the Vulkan fork) -----------
        //
        // NVPTX/AMDGPU take kernel arguments as a flat parameter list: buffers
        // as addrspace(1) pointers, scalars by value, matching the cuLaunch/
        // hipModuleLaunch kernelParams ABI. Vulkan/SPIR-V has NO raw-pointer
        // kernel ABI — its compute entry is `void main()` and arguments arrive
        // through descriptor-bound storage buffers (resource.handlefrombinding
        // + getpointer). That divergence is exactly these three hooks; their
        // defaults reproduce the NVPTX/AMDGPU pointer-arg model, so those
        // backends are behavior-preserving and only Vulkan overrides them.
        // (This is the "bigger fork than AMD" measured by the Vulkan bring-up:
        // unlike AMD, the kernel SIGNATURE and BUFFER ACCESS fork, not just the
        // coordinate leaf reads.)

        // One admitted kernel parameter, as seen by the signature/prologue hooks.
        struct KernelParam {
            std::string name;
            bool isBuffer;       // Buffer<T>/array (else a scalar primitive)
            llvm::Type* type;    // buffer element type, scalar type, or (sampler)
                                 // the {i32 filterMode, i32 addressMode} struct
            bool isSigned;       // scalar signedness / buffer-element signedness
            bool isTexture = false;  // Texture2D<T>/Texture3D<T> — a sampled-image
                                     // handle (Item 8). Carried as a backend handle
                                     // (ptr on CPU, image descriptor on Vulkan,
                                     // image rsrc on AMD); `type` is the texel
                                     // scalar T (float for float/UNORM/half formats,
                                     // i32 for the raw-integer formats; `isSigned`
                                     // tracks int32 vs uint32). The Vulkan image
                                     // binding and fetchTexture's result vector use it.
            bool isSampler = false;  // Sampler — filter/address descriptor (Item 8).
                                     // `type` is the {i32,i32} mode struct; bound
                                     // by value (CPU/SIMT) or as an OpTypeSampler
                                     // descriptor (Vulkan).
            bool isAccelStruct = false;  // AccelerationStructure — a descriptor-
                                     // bound BVH (cajeta-gpu Part C). Carried as a
                                     // backend handle; on Vulkan an
                                     // OpTypeAccelerationStructureKHR bound via
                                     // resource.handlefrombinding. `type` unused
                                     // (the AS handle is opaque). Ray-query only.
            bool isImage = false;    // Image2D — a writable 2-D storage image
                                     // (the writable twin of Texture2D). Carried
                                     // as a backend handle; on Vulkan a STORAGE_IMAGE
                                     // descriptor bound in GENERAL layout. `type` is
                                     // the texel scalar (f32); written by
                                     // `img.store(x, y, v)` (OpImageWrite).
            int textureDim = 2;      // texture KIND for isTexture params: 1 = Texture1D,
                                     // 2 = Texture2D, 3 = Texture3D, 4 = Texture2DArray,
                                     // 5 = TextureCube. Selects the image dimensionality
                                     // (Vulkan spirv.Image Dim + image-type), the
                                     // coord arity, and the 2-D vs 3-D sample/fetch
                                     // seam. Kept ahead of the trailing defaulted
                                     // fields so the 6/8-field positional KernelParam
                                     // inits keep their layout.
            bool isBufferArray = false;  // Buffer<T>[] — a bindless descriptor ARRAY of
                                     // buffers (`bufs[idx][i]`). isBuffer is ALSO true
                                     // (the element type / per-buffer access is identical
                                     // to a lone Buffer<T>); isBufferArray adds the outer
                                     // descriptor-array binding + the first subscript
                                     // selecting a descriptor. The runtime binds N
                                     // descriptors into one binding (descriptorCount = N
                                     // from the launch); on Vulkan an OpTypeRuntimeArray
                                     // indexed with NonUniformEXT. Appended last (after
                                     // textureDim) so existing positional inits are
                                     // unaffected.
        };

        // Create the kernel function for `name`. Default: a void-returning
        // function taking ptr addrspace(1) per buffer + the scalar type per
        // primitive, then decorateKernel(). Vulkan overrides to build `void()`
        // + its HLSL compute markers (no parameters).
        virtual llvm::Function* createKernel(
            llvm::Module& m, const std::string& name,
            const std::vector<KernelParam>& params);

        // Materialize parameter `idx` into `b`'s current (entry) block and
        // return its runtime value: a scalar value (the caller stores it into a
        // mutable slot) or a buffer base/handle (kept in bufferBases). Default:
        // fn->getArg(idx). Vulkan binds descriptor resources here instead.
        virtual llvm::Value* materializeParam(
            llvm::IRBuilderBase& b, llvm::Module& m, llvm::Function* fn,
            unsigned idx, const KernelParam& p);

        // Pointer to buffer element `index` of `base` (element type `elemTy`,
        // `index` already widened to i64). Default: an addrspace-preserving GEP
        // (correct for NVPTX/AMDGPU global+shared and for Vulkan shared-mem
        // globals). Vulkan routes descriptor-buffer handles through
        // resource.getpointer instead, self-dispatching on the base's type.
        virtual llvm::Value* bufferElementPtr(
            llvm::IRBuilderBase& b, llvm::Module& m, llvm::Value* base,
            llvm::Type* elemTy, llvm::Value* index);

        // Bindless descriptor array (Buffer<T>[]): select descriptor `descIndex`
        // of the buffer-array param bound at `binding`, returning a per-buffer
        // base the caller then feeds to bufferElementPtr for the inner `[i]`. The
        // first subscript of `bufs[idx][i]`. `arrayBase` is the materialized arg
        // for pointer backends (the [count, h…] handle array; null on Vulkan
        // where the descriptor is bound here via resource.handlefrombinding).
        // Default: unsupported (XPU-N01) — Vulkan + CPU override. `descIndex` is
        // i32.
        virtual llvm::Value* bufferArrayElement(
            llvm::IRBuilderBase& b, llvm::Module& m, llvm::Function* fn,
            unsigned binding, llvm::Value* arrayBase, llvm::Type* elemTy,
            llvm::Value* descIndex);

        // Sample the 2-D texture `texHandle` at normalized coords (u, v) in
        // [0, 1] through `samplerHandle`, returning the filtered texel (f32) —
        // the lowering of `tex.sample(sampler, u, v)` (Item 8). `texHandle` and
        // `samplerHandle` are exactly the values materializeParam produced for
        // the texture and sampler kernel params, so their representation is the
        // backend's own (CPU: a host texobj ptr + the {i32,i32} sampler struct;
        // Vulkan: image + sampler descriptors; AMD: image rsrc + sampler rsrc).
        // Default: unsupported (XPU-N01) — only backends with image sampling
        // override. Sampling at explicit LOD 0 (compute-valid; Vulkan requires
        // explicit LOD outside a fragment shader).
        // `lod` is the explicit mip level (float): the plain `tex.sample` passes
        // a constant 0.0; `tex.sampleLod(s, u, v, lod)` passes the user value.
        virtual llvm::Value* sampleTexture(
            llvm::IRBuilderBase& b, llvm::Module& m,
            llvm::Value* texHandle, llvm::Value* samplerHandle,
            llvm::Value* u, llvm::Value* v, llvm::Value* lod);

        // Fetch (texelFetch) the 2-D texture `texHandle` at the EXACT integer
        // coordinate (x, y), mip level 0, unfiltered and WITHOUT a sampler,
        // returning the texel as a <4 x `texelTy`> — the lowering of
        // `tex.fetch(x, y)` on a Texture2D<T> (`texelTy` is T's scalar LLVM type:
        // float for the float/UNORM/half formats, i32 for the raw-integer
        // formats R32I/R32UI/RGBA32I/RGBA32UI). The unfiltered twin of
        // sampleTexture: same texture descriptor (`texHandle` from
        // materializeParam; a *sampled* image, not a storage image — that
        // distinguishes this from loadImage), no sampler, no addressing mode.
        // `x`/`y` are i32 texel indices (NOT normalized).
        // Default: unsupported (XPU-N01) — only backends with an unfiltered
        // image read override. Vulkan emits OpImageFetch (+ Lod 0) via
        // llvm.spv.resource.load.level (the sampled-image Sampled=1 branch of
        // the read/fetch selection); AMD via __ockl_image_load_2D (the float4
        // result bitcast to <4 x i32> for integer formats — the HW image_load is
        // raw for a non-normalized integer image); CPU via the exact texel read.
        // (NVPTX emit-deferred.) sampleTexture takes no texelTy — sampling is
        // float-only (integer textures are rejected at the call site).
        // `lod` is the explicit mip level (i32): `tex.fetch` passes a constant 0;
        // `tex.fetchLod(x, y, lod)` passes the user value.
        virtual llvm::Value* fetchTexture(
            llvm::IRBuilderBase& b, llvm::Module& m,
            llvm::Value* texHandle, llvm::Value* x, llvm::Value* y,
            llvm::Type* texelTy, llvm::Value* lod);

        // Texture3D — the 3-D (volumetric) twins of sampleTexture/fetchTexture.
        // Same handle/contract, but a 3-component coordinate (u, v, w) / (x, y, z)
        // and a 3-D image. `sampleTexture3D` is the trilinear filtered read
        // (float-only); `fetchTexture3D` the unfiltered exact-voxel read returning
        // <4 x texelTy>. Default: unsupported (XPU-N01) — only backends with 3-D
        // image support override (CPU runtime, Vulkan 3-D OpImageSample/Fetch, AMD
        // __ockl_image_{sample,load}_3D). NVPTX 3-D emit-deferred.
        virtual llvm::Value* sampleTexture3D(
            llvm::IRBuilderBase& b, llvm::Module& m,
            llvm::Value* texHandle, llvm::Value* samplerHandle,
            llvm::Value* u, llvm::Value* v, llvm::Value* w);

        virtual llvm::Value* fetchTexture3D(
            llvm::IRBuilderBase& b, llvm::Module& m,
            llvm::Value* texHandle, llvm::Value* x, llvm::Value* y, llvm::Value* z,
            llvm::Type* texelTy);

        // Texture1D — the 1-D (linear) twins of sampleTexture/fetchTexture. Same
        // handle/contract, but a single-component coordinate (u) / (x) and a 1-D
        // image. `sampleTexture1D` is the linear filtered read (float-only);
        // `fetchTexture1D` the unfiltered exact-texel read returning <4 x texelTy>.
        // No `lod` operand — mipmaps are 2-D only. Default: unsupported (XPU-N01) —
        // only backends with 1-D image support override (CPU runtime reuse of the
        // 2-D path with height=1, Vulkan 1-D OpImageSample/Fetch, AMD
        // __ockl_image_{sample,load}_1D — scalar coords). NVPTX 1-D emit-deferred.
        virtual llvm::Value* sampleTexture1D(
            llvm::IRBuilderBase& b, llvm::Module& m,
            llvm::Value* texHandle, llvm::Value* samplerHandle, llvm::Value* u);

        virtual llvm::Value* fetchTexture1D(
            llvm::IRBuilderBase& b, llvm::Module& m,
            llvm::Value* texHandle, llvm::Value* x, llvm::Type* texelTy);

        // Texture2DArray — the layered twins of sampleTexture/fetchTexture. Like a
        // 2-D texture but with an extra `layer` selecting one of N planes: the
        // sample/fetch is 2-D within that layer (NO cross-layer filtering — unlike
        // Texture3D's trilinear). `layer` is an INTEGER array index (i32), not a
        // normalized coord; backends that want a float layer coordinate convert it.
        // `sampleTexture2DArray` is the bilinear filtered read (float-only) at the
        // nearest layer; `fetchTexture2DArray` the unfiltered exact-texel read →
        // <4 x texelTy>. Default: unsupported (XPU-N01). On the image side a layered
        // image is Dim=2D, Arrayed=1 (Vulkan VIEW_TYPE_2D_ARRAY, AMD
        // __ockl_image_{sample,load}_2Da, CPU reuse of the 2-D path per layer).
        virtual llvm::Value* sampleTexture2DArray(
            llvm::IRBuilderBase& b, llvm::Module& m,
            llvm::Value* texHandle, llvm::Value* samplerHandle,
            llvm::Value* u, llvm::Value* v, llvm::Value* layer);

        virtual llvm::Value* fetchTexture2DArray(
            llvm::IRBuilderBase& b, llvm::Module& m,
            llvm::Value* texHandle, llvm::Value* x, llvm::Value* y,
            llvm::Value* layer, llvm::Type* texelTy);

        // TextureCube — sampled by a DIRECTION vector (x, y, z), not a planar
        // coordinate: the hardware picks the face the vector points at and projects
        // onto it. The filtered read (float-only); a cube has NO fetch (no single
        // integer texel for a direction). Default: unsupported (XPU-N01). On the
        // image side a cube is Dim=Cube (Vulkan VIEW_TYPE_CUBE + CUBE_COMPATIBLE,
        // AMD __ockl_image_sample_CM, CPU direction→face projection + bilinear).
        virtual llvm::Value* sampleTextureCube(
            llvm::IRBuilderBase& b, llvm::Module& m,
            llvm::Value* texHandle, llvm::Value* samplerHandle,
            llvm::Value* x, llvm::Value* y, llvm::Value* z);

        // Store `value` into the 2-D storage image `imgHandle` at INTEGER texel
        // coordinate (x, y) — the lowering of `img.store(x, y, value)` (writable
        // images, the gfx bridge / twin of sampleTexture). `imgHandle` is exactly
        // the value materializeParam produced for the Image2D kernel param (Vulkan:
        // a STORAGE_IMAGE descriptor bound in GENERAL layout). `x`/`y` are i32
        // texel indices (NOT normalized); `value` is the f32 texel.
        // Default: unsupported (XPU-N01) — only backends with storage-image write
        // override. Vulkan emits a single OpImageWrite via
        // llvm.spv.resource.store.2d (a <4 x f32> texel, value in .x; the image's
        // R32 format keeps lane 0).
        virtual void storeImage(
            llvm::IRBuilderBase& b, llvm::Module& m,
            llvm::Value* imgHandle, llvm::Value* x, llvm::Value* y,
            llvm::Value* value);

        // Read the texel of the 2-D storage image `imgHandle` at INTEGER texel
        // coordinate (x, y), returning it as an f32 — the lowering of
        // `img.load(x, y)` (the read twin of storeImage; together they make
        // Image2D read-modify-write and image->image passes possible). Same
        // STORAGE_IMAGE descriptor as storeImage (no sampler; GENERAL layout).
        // Default: unsupported (XPU-N01) — only backends with storage-image read
        // override. Vulkan emits a single OpImageRead via
        // llvm.spv.resource.load.2d (a scalar result is read as a <4 x f32> texel
        // and component 0 extracted; the R32f image keeps lane 0).
        virtual llvm::Value* loadImage(
            llvm::IRBuilderBase& b, llvm::Module& m,
            llvm::Value* imgHandle, llvm::Value* x, llvm::Value* y);

        // --- transcendental math (B2 increment 2) ---------------------------
        // sin/cos/tan/asin/acos/atan (unary), pow/atan2 (binary), rsqrt. The
        // DEFAULT emits the matching `llvm.*` intrinsic — correct on CPU (libm)
        // and Vulkan (the SPIR-V backend maps it to OpExtInst GLSL.std.450). The
        // AMD backend OVERRIDES it to emit `__ocml_<name>_f32` device-library
        // calls (AMDGPU mis-lowers `llvm.sin` without ocml range reduction); the
        // AMD backend then links ocml.bc. `name` is the cajeta Math name; `args`
        // is 1 element (unary/rsqrt) or 2 (pow/atan2), already in an FP type.
        virtual llvm::Value* transcendental(
            llvm::IRBuilderBase& b, llvm::Module& m, const std::string& name,
            llvm::ArrayRef<llvm::Value*> args);

        // --- integer dot product (SPV_KHR_integer_dot_product, DP4a) ---------
        // `Vector<int8,4>` / `Vector<uint8,4>` dot -> int32, with int32
        // accumulation. `a`/`c` are <4 x i8> vectors; `acc` is the i32
        // accumulator (i32 0 for a plain dot, the third method arg for the
        // `a.dot(b, acc)` fused form). DEFAULT: a portable widening reduce
        // (vecops::idotWiden — sext/zext each lane to i32, mul, sum, + acc),
        // correct on CPU/AMD/NVIDIA. Vulkan OVERRIDES to pack the four lanes
        // into an i32 and emit llvm.spv.dot4add.{i8,u8}packed (the DP4a op:
        // OpSDot/OpUDot PackedVectorFormat4x8Bit + OpIAdd). `isSigned` picks
        // signed vs unsigned.
        virtual llvm::Value* integerDot4x8(
            llvm::IRBuilderBase& b, llvm::Module& m, llvm::Value* a,
            llvm::Value* c, llvm::Value* acc, bool isSigned);

        // --- float atomics (SPV_EXT_shader_atomic_float_add / _min_max) ------
        // `Buffer<float32>.atomic{Add,Min,Max}(i, v)`: an atomic read-modify-
        // write on the element pointer, returning the OLD value. DEFAULT: a
        // relaxed (monotonic), system-scope `atomicrmw` — selects the native
        // global FP atomic on AMDGPU/NVPTX and a lock/cmpxchg on CPU. Vulkan
        // OVERRIDES to AcquireRelease + Device scope and emits OpAtomicFAddEXT /
        // FMinEXT / FMaxEXT (spirv-val rejects SequentiallyConsistent and
        // relaxed-with-storage-class semantics on OpAtomicF*EXT, and CrossDevice
        // scope, so the Vulkan path uses Device scope + AcquireRelease).
        // `order` selects the memory ordering (MemoryOrder.Default keeps the
        // backend's established default — see toAtomicOrdering).
        enum class AtomicFloatOp { Add, Min, Max };
        virtual llvm::Value* atomicFloatRMW(
            llvm::IRBuilderBase& b, llvm::Module& m, AtomicFloatOp op,
            llvm::Value* ptr, llvm::Value* value,
            MemoryOrder order = MemoryOrder::Default);

        // --- integer atomics (core SPIR-V — no extension) --------------------
        // `Buffer<int32|uint32>.atomic{Add,Sub,Min,Max,And,Or,Xor,Exchange}(i, v)`:
        // an atomic read-modify-write on the element pointer, returning the OLD
        // value. Unlike the float atomics these are CORE (no SPV_EXT), so the
        // generic `atomicrmw` maps to OpAtomicIAdd/ISub/SMin/UMin/SMax/UMax/And/Or/
        // Xor/Exchange directly (the backend picks the opcode from the BinOp;
        // `isSigned` selects S vs U min/max). DEFAULT: relaxed (monotonic),
        // system-scope — native global atomic on AMDGPU/NVPTX, lock-prefixed on
        // CPU. Vulkan OVERRIDES to Device scope + AcquireRelease (same memory-model
        // constraint as the float path). The universal concurrency primitive:
        // counters, histograms, lock-free allocation (pairs with Wave.prefixSum).
        enum class AtomicIntOp { Add, Sub, Min, Max, And, Or, Xor, Exchange };
        virtual llvm::Value* atomicIntRMW(
            llvm::IRBuilderBase& b, llvm::Module& m, AtomicIntOp op,
            llvm::Value* ptr, llvm::Value* value, bool isSigned,
            MemoryOrder order = MemoryOrder::Default);

        // `Buffer<int32|uint32>.atomicCompareExchange(i, expected, desired)`: the
        // universal lock-free primitive — atomically set element `i` to `desired`
        // iff it currently equals `expected`, returning the OLD value (compare the
        // result to `expected` to learn if the swap happened). Lowers to a
        // `cmpxchg` → OpAtomicCompareExchange; returns the loaded value (element 0
        // of the {value, success} pair). DEFAULT monotonic; Vulkan Device scope +
        // AcquireRelease/Acquire (success/failure orderings).
        virtual llvm::Value* atomicCompareExchange(
            llvm::IRBuilderBase& b, llvm::Module& m, llvm::Value* ptr,
            llvm::Value* expected, llvm::Value* desired,
            MemoryOrder order = MemoryOrder::Default);

        // --- shader clock (SPV_KHR_shader_clock) -----------------------------
        // `Thread.clock()` — read a free-running hardware counter for in-kernel
        // timing/profiling, returning the 64-bit tick. DEFAULT: llvm.readcycle-
        // counter (CPU rdtsc). Vulkan emits OpReadClockKHR at Subgroup scope via
        // the fork's llvm.spv.read.clock intrinsic; AMD uses s_memrealtime;
        // NVPTX uses clock64. Ticks are for *relative* measurement (diff two
        // reads), not wall-clock seconds.
        virtual llvm::Value* readClock(llvm::IRBuilderBase& b, llvm::Module& m);

        // --- ray query (SPV_KHR_ray_query) — the Vulkan-only fork ------------
        //
        // A RayQuery kernel-body local is a function-local opaque object; its
        // ops lower to the llvm.spv.ray.query.* intrinsics (the texture-style
        // intrinsic path, since the __spirv_* builtin path is shader-gated to
        // OpenCL and ray query is [EnvVulkan]-only). Only the Vulkan backend
        // overrides these; the defaults reject a RayQuery in a kernel lowered
        // for a backend without ray-query support (XPU-N02).

        // The noun seam's compile-time face (cajeta-gpu inc-4 brick #2): the impl
        // this backend builds an AccelerationStructure as. The noun's impl
        // determines the verb's lowering (CajetaGPU.md §1.4) — so this one method
        // is the single source the RayQuery verb path derives from, instead of the
        // backend being re-inferred. Ordinals MUST match CajetaAsImpl in
        // runtime/native/cajeta_noun_impl.h (comment-synced, like CAJETA_KP_*).
        // Today impl == backend: Vulkan builds the native BLAS, CPU the portable
        // software BVH. The capability-heuristic brick lets one backend pick either.
        // This is the ray-query noun's ABI-pinned encoding of the degrade choice;
        // it feeds rayQueryTier() below (the unified ImplTier face).
        enum class NounImpl { SoftwareBvh = 0, VulkanNative = 1 };
        virtual NounImpl accelImpl() const { return NounImpl::VulkanNative; }

        // The ray-query verb tier in the unified ImplTier vocabulary: SoftwareBvh
        // ⇒ Portable (the SoftwareRayQuery walk), VulkanNative ⇒ Native
        // (OpRayQuery). Derived from the noun's recorded impl so the verb follows
        // the noun (one source). The coop-matrix counterpart is coopMatrixTier;
        // both features answer "which tier?" through ImplTier.
        ImplTier rayQueryTier() const {
            return accelImpl() == NounImpl::SoftwareBvh ? ImplTier::Portable
                                                        : ImplTier::Native;
        }

        // True when this backend has no native inline ray query and uses the
        // portable Software tier instead (cajeta-gpu ray-query-to-core): a
        // RayQuery lowers to the cajeta.gpu.core.SoftwareRayQuery @Device walk over
        // a software BVH `Buffer<float32>`, not to the native rayQuery* seams. Thin
        // alias over rayQueryTier() — the call site (KernelLowering) reads this to
        // pick the verb lowering, and the noun (AccelerationStructure) is
        // materialized as the BVH buffer base.
        bool softwareRayQuery() const { return rayQueryTier() == ImplTier::Portable; }

        // The LLVM type to alloca for a `RayQuery` local. Vulkan:
        // target("spirv.RayQueryKHR"). Default: unsupported.
        virtual llvm::Type* rayQueryType(llvm::Module& m);

        // rq.initialize(as, rayFlags, cullMask, origin<3xf32>, tMin,
        //               direction<3xf32>, tMax). `rqPtr` is the RayQuery alloca;
        // `asHandle` the materialized AccelerationStructure descriptor.
        // Void op (→ OpRayQueryInitializeKHR).
        virtual void rayQueryInitialize(
            llvm::IRBuilderBase& b, llvm::Module& m,
            llvm::Value* rqPtr, llvm::Value* asHandle,
            llvm::Value* rayFlags, llvm::Value* cullMask,
            llvm::Value* origin, llvm::Value* tMin,
            llvm::Value* direction, llvm::Value* tMax);

        // rq.proceed() → i1 (→ OpRayQueryProceedKHR).
        virtual llvm::Value* rayQueryProceed(
            llvm::IRBuilderBase& b, llvm::Module& m, llvm::Value* rqPtr);

        // rq.committedType()/candidateType() → i32. `intersection` is the i32
        // intersection selector (1 = committed, 0 = candidate).
        // (→ OpRayQueryGetIntersectionTypeKHR.)
        virtual llvm::Value* rayQueryIntersectionType(
            llvm::IRBuilderBase& b, llvm::Module& m, llvm::Value* rqPtr,
            llvm::Value* intersection);

        // rq.candidatePrimitiveIndex() → i32 — which indexed primitive the
        // candidate intersection hit (RTNN exact-distance refinement).
        // (→ OpRayQueryGetIntersectionPrimitiveIndexKHR.)
        virtual llvm::Value* rayQueryIntersectionPrimitiveIndex(
            llvm::IRBuilderBase& b, llvm::Module& m, llvm::Value* rqPtr,
            llvm::Value* intersection);

        // Nearest-hit getters + commit (inc 3b). `intersection` selects candidate
        // (0) or committed (1). The getters → OpRayQueryGetIntersection{T,
        // Barycentrics,FrontFace}KHR; confirm/generate → OpRayQuery{Confirm,
        // Generate}IntersectionKHR (void). Default throws (software-only via
        // SoftwareRayQuery; only SpirvTarget overrides, fork-gated).

        // rq distance `t` → f32.
        virtual llvm::Value* rayQueryIntersectionT(
            llvm::IRBuilderBase& b, llvm::Module& m, llvm::Value* rqPtr,
            llvm::Value* intersection);
        // rq barycentrics → <2 x float> (u, v).
        virtual llvm::Value* rayQueryIntersectionBarycentrics(
            llvm::IRBuilderBase& b, llvm::Module& m, llvm::Value* rqPtr,
            llvm::Value* intersection);
        // rq front-face → i1.
        virtual llvm::Value* rayQueryIntersectionFrontFace(
            llvm::IRBuilderBase& b, llvm::Module& m, llvm::Value* rqPtr,
            llvm::Value* intersection);
        // Commit the current triangle candidate (void).
        virtual void rayQueryConfirmIntersection(
            llvm::IRBuilderBase& b, llvm::Module& m, llvm::Value* rqPtr);
        // Commit an AABB candidate at distance `tHit` (void).
        virtual void rayQueryGenerateIntersection(
            llvm::IRBuilderBase& b, llvm::Module& m, llvm::Value* rqPtr,
            llvm::Value* tHit);

        // Cooperative matrix (cajeta-gpu Part C, CM4). Device-only subgroup
        // matrix-core tiles. Vulkan lowers these to the SPV_KHR_cooperative_matrix
        // ops via the llvm.spv.cooperative.matrix.* intrinsics; every other
        // backend's default throws (XPU-N03). All matrices are at Subgroup scope.

        // Which lowering a `CooperativeMatrix<T,Rows,Cols,Use>` gets on this
        // backend for the given element/shape — answered in the unified ImplTier
        // vocabulary (the coop-matrix instance of the degrade seam):
        //   Native    — a hardware matrix-core path (Vulkan SPV_KHR_cooperative_
        //               matrix for the dtype configs the driver advertises; AMD
        //               amdgcn WMMA; NVIDIA wmma).
        //   Portable  — a portable flat-tile matmul (the DeviceLowerer emits a
        //               `[Rows*Cols x T]` tile + a strided gather/scatter + an
        //               f32/i32 triple-loop multiply-add). Correct on every
        //               backend; not matrix-core accelerated.
        // This is the per-backend BASE decision, static per (backend, dtype,
        // shape) — known at compile time, so no runtime branch is emitted. The
        // explicit CAJETA_GPU_COOPMATRIX_IMPL override is layered on top of it by
        // resolveImplTier at the call site (KernelLowering). The default is
        // Portable, so a backend with no native MMA seam still RUNS cooperative
        // matrix (it does not throw); a backend overrides this to claim Native
        // where it can.
        virtual ImplTier coopMatrixTier(llvm::Type* /*elem*/,
                                        uint32_t /*rows*/, uint32_t /*cols*/,
                                        uint32_t /*use*/) {
            return ImplTier::Portable;
        }

        // The LLVM type to alloca for a `CooperativeMatrix<T,Rows,Cols,Use>`
        // local: target("spirv.CooperativeMatrixKHR", elem, 3, rows, cols, use).
        // Only called for the Native tier; the Portable tier uses a flat tile.
        virtual llvm::Type* coopMatrixType(llvm::Module& m, llvm::Type* elem,
                                           uint32_t rows, uint32_t cols,
                                           uint32_t use);

        // m.load(src, layout, stride) → the loaded tile value (result type
        // `matrixType`). `ptr` is the Buffer<T> element-0 pointer; `layout`/
        // `stride` are i32. `rows`/`cols`/`use` describe the tile shape — the
        // Vulkan seam ignores them (the opaque matrixType already carries them);
        // a per-lane backend (AMD WMMA) needs them to gather the right fragment
        // (→ OpCooperativeMatrixLoadKHR).
        virtual llvm::Value* coopMatrixLoad(
            llvm::IRBuilderBase& b, llvm::Module& m, llvm::Value* ptr,
            llvm::Value* layout, llvm::Value* stride, llvm::Type* matrixType,
            uint32_t rows, uint32_t cols, uint32_t use);

        // m.store(dst, layout, stride): store `matrixVal` to `ptr`. Void op.
        // `rows`/`cols`/`use` as in coopMatrixLoad (→ OpCooperativeMatrixStoreKHR).
        virtual void coopMatrixStore(
            llvm::IRBuilderBase& b, llvm::Module& m, llvm::Value* ptr,
            llvm::Value* matrixVal, llvm::Value* layout, llvm::Value* stride,
            uint32_t rows, uint32_t cols, uint32_t use);

        // c.mma(a, b) → a*b+c (result type `matrixType`, the accumulator type)
        // (→ OpCooperativeMatrixMulAddKHR).
        virtual llvm::Value* coopMatrixMulAdd(
            llvm::IRBuilderBase& b, llvm::Module& m, llvm::Value* a,
            llvm::Value* bMat, llvm::Value* c, llvm::Type* matrixType);

        // m.splat(value) → a tile with every element = `value` (the zero/initial
        // accumulator), result type `matrixType` (→ OpCompositeConstruct).
        virtual llvm::Value* coopMatrixSplat(
            llvm::IRBuilderBase& b, llvm::Module& m, llvm::Value* value,
            llvm::Type* matrixType);

        // Called once on the enclosing kernel function the first time a NATIVE
        // cooperative-matrix tile is allocated in its body. A backend whose
        // matrix-core path has ABI requirements on the kernel (AMD RDNA3 WMMA is
        // wave32-only) sets them here; the default is a no-op (Vulkan needs none).
        virtual void prepareNativeCoopMatrix(llvm::Function* /*fn*/) {}

        // The LLVM type of a Buffer<T> when passed BY VALUE as a @Device helper
        // argument — i.e. the type of the buffer base held in bufferBases. A
        // kernel buffer param arrives via the backend's mechanism (a pointer arg
        // on NVPTX/AMDGPU/CPU, a descriptor handle on Vulkan), but a helper takes
        // that already-materialized base as a plain function argument, so its
        // param type must match. Default: ptr addrspace(1) (NVPTX/AMDGPU global).
        // CPU overrides to a flat addrspace(0) pointer; Vulkan to the
        // storage-buffer handle (spirv.VulkanBuffer) it keeps in bufferBases.
        virtual llvm::Type* bufferParamType(llvm::Module& m, llvm::Type* elemTy);

        // The LLVM type of a Texture2D when passed as a kernel parameter in the
        // flat pointer-arg model (Item 8 Stage C/D). AMDGPU: ptr addrspace(4) to
        // the HIP texture object (image obj at +0, sampler obj at +48), consumed
        // by sampleTexture via __ockl_image_sample_2D. NVPTX (emit-only): i64
        // (cudaTextureObject_t handle by value). Default: i64. CPU/Vulkan override
        // createKernel/materializeParam and never consult this.
        virtual llvm::Type* textureParamType(llvm::Module& m);

        // --- wave / subgroup ops (the @Wave variance-shaped feature) ---------
        //
        // All three backends have hardware wave ops, but they diverge in
        // intrinsic, lane-width, and ballot shape — so these are pure-virtual
        // seam points, the headline test of whether the abstraction holds for a
        // genuinely divergent capability (cajeta-amd.md §2).

        // Lanes per wave: i32. NVPTX warpsize sreg (32); AMDGPU wavefrontsize
        // intrinsic; Vulkan spv.wave.get_lane_count.
        virtual llvm::Value* waveWidth(llvm::IRBuilderBase& b,
                                       llvm::Module& m) = 0;

        // Read i32 `value` from lane `srcLane` (i32), broadcast across the wave
        // (shuffle-by-index / readlane). Returns i32.
        virtual llvm::Value* waveShuffle(llvm::IRBuilderBase& b, llvm::Module& m,
                                         llvm::Value* value,
                                         llvm::Value* srcLane) = 0;

        // Ballot: an i64 bitmask whose bit i is set iff lane i's `pred` (i1) is
        // true. Backends whose native ballot is narrower (i32) zero-extend.
        virtual llvm::Value* waveBallot(llvm::IRBuilderBase& b, llvm::Module& m,
                                        llvm::Value* pred) = 0;

        // Like waveShuffle, but `srcLane` may be DIVERGENT (per-lane different).
        // waveShuffle is uniform-index on some backends (AMDGPU readlane needs an
        // SGPR source), so cross-lane patterns with a computed per-lane source
        // (rotate, scan) use this instead. Default = waveShuffle (correct where
        // the native shuffle already takes a divergent index — NVPTX shfl, Vulkan
        // OpGroupNonUniformShuffle, the CPU gather); AMDGPU overrides to
        // ds_bpermute.
        virtual llvm::Value* waveShuffleDivergent(llvm::IRBuilderBase& b,
                                                  llvm::Module& m,
                                                  llvm::Value* value,
                                                  llvm::Value* srcLane) {
            return waveShuffle(b, m, value, srcLane);
        }

        // Wave-wide reduction (sum) over i32 across active lanes; returns i32.
        // The "comprehensiveness-inversion" probe found this maps 1:1 like
        // shuffle/ballot — a single hardware intrinsic on all three backends
        // (the guessed shuffle/DPP-sequence fork did not materialize). NVPTX's
        // redux.sync is gated on sm_80+; below that a butterfly-shuffle fallback
        // would be needed (out of scope).
        virtual llvm::Value* waveReduceSum(llvm::IRBuilderBase& b,
                                           llvm::Module& m,
                                           llvm::Value* value) = 0;

        // The wave-reduction family beyond sum: a single i32 (unsigned) value
        // reduced across the active lanes; every lane receives the same result.
        // Max/Min are UNSIGNED (the uint32 surface). Native on every backend —
        // Vulkan OpGroupNonUniform{UMax,UMin,BitwiseAnd,BitwiseOr,BitwiseXor}
        // (the GroupNonUniformArithmetic family, already Shader-reachable — NOT
        // the OpenCL-only SPV_KHR_uniform_group_instructions); AMDGPU
        // wave.reduce.{umax,umin,and,or,xor}; NVPTX redux.sync.{...} (sm_80+);
        // CPU a VFABI reduce variant. Product is intentionally absent (no AMD/
        // NVPTX hardware reduce — a shuffle-tree follow-on).
        enum class WaveReduceOp { Max, Min, And, Or, Xor };
        virtual llvm::Value* waveReduce(llvm::IRBuilderBase& b, llvm::Module& m,
                                        WaveReduceOp op, llvm::Value* value) = 0;

        // EXCLUSIVE prefix scan across the lanes: lane i receives the sum (or
        // product) of lanes 0..i-1; lane 0 gets the identity (0 / 1). uint32.
        // NOT pure-virtual: the default (out-of-line in KernelLowering.cpp) is a
        // width-agnostic Hillis-Steele scan built on waveShuffleDivergent /
        // waveLaneId / waveWidth — correct on any backend whose shuffle takes a
        // divergent index (NVPTX shfl; AMDGPU once it overrides
        // waveShuffleDivergent → ds_bpermute). Vulkan OVERRIDES to the single
        // native OpGroupNonUniform{IAdd,IMul} with the ExclusiveScan group op
        // (the fork spv_wave_prefix_{sum,product} intrinsics); CPU overrides to a
        // VFABI scan variant.
        enum class WaveScanOp { Sum, Product };
        virtual llvm::Value* waveScan(llvm::IRBuilderBase& b, llvm::Module& m,
                                      WaveScanOp op, llvm::Value* value);

        // The calling work-item's lane index within its wave: i32 in
        // [0, waveWidth). The other half of "interrogate your environment"
        // (with waveWidth) for width-agnostic kernels. NVPTX laneid sreg; AMDGPU
        // mbcnt; Vulkan SubgroupLocalInvocationId; CPU tid.x % width.
        virtual llvm::Value* waveLaneId(llvm::IRBuilderBase& b,
                                        llvm::Module& m) = 0;

        // Wave rotate: read i32 `value` from the lane `delta` positions ahead,
        // modulo the wave width — i.e. from lane `(laneId + delta) mod width`.
        // NOT pure-virtual: the default (defined out-of-line in KernelLowering.cpp
        // — the header only forward-declares IRBuilderBase) is a width-agnostic
        // shuffle built on the existing waveShuffle/waveLaneId/waveWidth seams,
        // so every backend gets it for free (the isFirstLane pattern). Vulkan
        // OVERRIDES to the single native OpGroupNonUniformRotateKHR
        // (SPV_KHR_subgroup_rotate), reached from the Shader flavor via the
        // fork's llvm.spv.subgroup.rotate intrinsic. Cross-lane, so callers flag
        // the kernel for maximal reconvergence (like shuffle/ballot/reduce).
        virtual llvm::Value* waveRotate(llvm::IRBuilderBase& b, llvm::Module& m,
                                        llvm::Value* value, llvm::Value* delta);

        // --- quad (2x2) ops (SPV_KHR_quad_control + core GroupNonUniformQuad) --
        //
        // A quad is four invocations with consecutive lane ids (laneId & ~3 ..
        // +3) — the 2x2 derivative/tile group. Like the wave seams these are
        // cross-lane and flag the kernel for maximal reconvergence; UNLIKE them
        // they are NOT pure-virtual. The defaults (out-of-line in
        // KernelLowering.cpp) are width-agnostic forms built on the existing
        // waveShuffleDivergent / waveBallot / waveLaneId seams — so NVPTX, AMDGPU
        // and CPU get quad ops for FREE, validating the same lane layout the
        // Vulkan native ops use. Vulkan OVERRIDES each to the single native op
        // (OpGroupNonUniformQuad{Broadcast,Swap} core; OpGroupNonUniformQuad
        // {All,Any}KHR from SPV_KHR_quad_control) via the fork llvm.spv.quad.*
        // intrinsics.

        // Read i32 `value` from quad lane `index` (0-3); every lane in the quad
        // receives that lane's value. Vulkan: OpGroupNonUniformQuadBroadcast.
        virtual llvm::Value* quadBroadcast(llvm::IRBuilderBase& b,
                                           llvm::Module& m, llvm::Value* value,
                                           llvm::Value* index);

        // Exchange i32 `value` across the 2x2 quad: direction 0 = horizontal
        // (lanes 0<->1, 2<->3), 1 = vertical (0<->2, 1<->3), 2 = diagonal
        // (0<->3, 1<->2). The partner lane is laneId ^ (direction+1). Vulkan:
        // OpGroupNonUniformQuadSwap.
        virtual llvm::Value* quadSwap(llvm::IRBuilderBase& b, llvm::Module& m,
                                      llvm::Value* value, unsigned direction);

        // Quad-wide vote of a per-lane predicate (i1 -> i1): all = true iff
        // `pred` holds for every lane of the quad; any = true iff it holds for
        // some lane. Vulkan: OpGroupNonUniformQuad{All,Any}KHR (no Scope operand
        // — implicitly quad-scoped). The portable default reads a wave ballot and
        // tests this lane's quad nibble (assumes full quads).
        virtual llvm::Value* quadAll(llvm::IRBuilderBase& b, llvm::Module& m,
                                     llvm::Value* pred);
        virtual llvm::Value* quadAny(llvm::IRBuilderBase& b, llvm::Module& m,
                                     llvm::Value* pred);

        // A dynamic (runtime-sized) `shared T[n]` lowers to an external unsized
        // [0 x T] addrspace(3) global — the native extern-shared model on NVPTX
        // and AMDGPU, where the launch sizes it. Vulkan can't: an external
        // workgroup variable needs the Vulkan-forbidden Linkage capability, and a
        // workgroup array needs a concrete length. Backends that return true get a
        // concrete INTERNAL [1 x T] array instead (a post-emit pass turns the
        // length into a spec constant the launch's sharedBytes sets).
        virtual bool dynamicSharedNeedsConcreteSize() const { return false; }
    };

    // The explicit-override layer of the degrade seam (CajetaGPU.md §1.5): apply
    // the CAJETA_GPU_<FEATURE>_IMPL env override to a per-backend BASE tier.
    // `feature` is the uppercase feature tag (e.g. "COOPMATRIX"), spliced into
    // the env name. The override wins when set: "software" → Portable, "native" →
    // Native; unset or any other value keeps `base`. The env is read here and
    // ONLY here (the compile-time-feature instance of the convention; the
    // runtime-noun instance is caj_resolve_as_impl / CAJETA_GPU_AS_IMPL in
    // cajeta_runtime.c). Defined out-of-line in KernelLowering.cpp beside the
    // other LoweringTarget defaults.
    LoweringTarget::ImplTier resolveImplTier(const char* feature,
                                             LoweringTarget::ImplTier base);

} // namespace xpu
} // namespace cajeta
