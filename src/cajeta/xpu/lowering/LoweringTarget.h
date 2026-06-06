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

        // Address space for entry-block allocas (the mutable scalar-slot model
        // — loop counters, reassigned locals). NVPTX: 0 (generic). AMDGPU: 5
        // (private). Getting this wrong on AMDGPU is the classic first bug
        // (cajeta-amd.md §2) — an AS-0 alloca there is invalid.
        virtual unsigned allocaAddressSpace() const = 0;

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

        // Decorate a freshly-created kernel function: calling convention +
        // any kernel-marker metadata. NVPTX: ptx_kernel CC + nvvm.annotations.
        // AMDGPU: amdgpu_kernel CC, no metadata.
        virtual void decorateKernel(llvm::Function* fn, llvm::Module& m) = 0;

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
            bool isTexture = false;  // Texture2D — a sampled-image handle (Item 8).
                                     // Carried as a backend handle (ptr on CPU,
                                     // image descriptor on Vulkan, image rsrc on
                                     // AMD); `type` is the texel scalar (f32).
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
        virtual llvm::Value* sampleTexture(
            llvm::IRBuilderBase& b, llvm::Module& m,
            llvm::Value* texHandle, llvm::Value* samplerHandle,
            llvm::Value* u, llvm::Value* v);

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

        // --- ray query (SPV_KHR_ray_query) — the Vulkan-only fork ------------
        //
        // A RayQuery kernel-body local is a function-local opaque object; its
        // ops lower to the llvm.spv.ray.query.* intrinsics (the texture-style
        // intrinsic path, since the __spirv_* builtin path is shader-gated to
        // OpenCL and ray query is [EnvVulkan]-only). Only the Vulkan backend
        // overrides these; the defaults reject a RayQuery in a kernel lowered
        // for a backend without ray-query support (XPU-N02).

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

        // Cooperative matrix (cajeta-gpu Part C, CM4). Device-only subgroup
        // matrix-core tiles. Vulkan lowers these to the SPV_KHR_cooperative_matrix
        // ops via the llvm.spv.cooperative.matrix.* intrinsics; every other
        // backend's default throws (XPU-N03). All matrices are at Subgroup scope.

        // Which lowering a `CooperativeMatrix<T,Rows,Cols,Use>` gets on this
        // backend for the given element/shape:
        //   Native   — a hardware matrix-core path (Vulkan SPV_KHR_cooperative_
        //              matrix for the dtype configs the driver advertises; AMD
        //              amdgcn WMMA; NVIDIA wmma).
        //   Software  — a portable flat-tile matmul (the DeviceLowerer emits a
        //              `[Rows*Cols x T]` tile + a strided gather/scatter + an
        //              f32/i32 triple-loop multiply-add). Correct on every
        //              backend; not matrix-core accelerated.
        // The choice is static per (backend, dtype, shape) — known at compile
        // time, so no runtime branch is emitted. The default is Software, so a
        // backend with no native MMA seam still RUNS cooperative matrix (it does
        // not throw); a backend overrides this to claim Native where it can.
        enum class CoopMatrixTier { Native, Software };
        virtual CoopMatrixTier coopMatrixTier(llvm::Type* /*elem*/,
                                              uint32_t /*rows*/, uint32_t /*cols*/,
                                              uint32_t /*use*/) {
            return CoopMatrixTier::Software;
        }

        // The LLVM type to alloca for a `CooperativeMatrix<T,Rows,Cols,Use>`
        // local: target("spirv.CooperativeMatrixKHR", elem, 3, rows, cols, use).
        // Only called for the Native tier; the Software tier uses a flat tile.
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

        // Wave-wide reduction (sum) over i32 across active lanes; returns i32.
        // The "comprehensiveness-inversion" probe found this maps 1:1 like
        // shuffle/ballot — a single hardware intrinsic on all three backends
        // (the guessed shuffle/DPP-sequence fork did not materialize). NVPTX's
        // redux.sync is gated on sm_80+; below that a butterfly-shuffle fallback
        // would be needed (out of scope).
        virtual llvm::Value* waveReduceSum(llvm::IRBuilderBase& b,
                                           llvm::Module& m,
                                           llvm::Value* value) = 0;

        // The calling work-item's lane index within its wave: i32 in
        // [0, waveWidth). The other half of "interrogate your environment"
        // (with waveWidth) for width-agnostic kernels. NVPTX laneid sreg; AMDGPU
        // mbcnt; Vulkan SubgroupLocalInvocationId; CPU tid.x % width.
        virtual llvm::Value* waveLaneId(llvm::IRBuilderBase& b,
                                        llvm::Module& m) = 0;

        // A dynamic (runtime-sized) `shared T[n]` lowers to an external unsized
        // [0 x T] addrspace(3) global — the native extern-shared model on NVPTX
        // and AMDGPU, where the launch sizes it. Vulkan can't: an external
        // workgroup variable needs the Vulkan-forbidden Linkage capability, and a
        // workgroup array needs a concrete length. Backends that return true get a
        // concrete INTERNAL [1 x T] array instead (a post-emit pass turns the
        // length into a spec constant the launch's sharedBytes sets).
        virtual bool dynamicSharedNeedsConcreteSize() const { return false; }
    };

} // namespace xpu
} // namespace cajeta
