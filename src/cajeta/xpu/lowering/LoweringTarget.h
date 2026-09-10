// LoweringTarget — the device-backend variance surface: the measured list of
// decisions that differ between backends. Everything else lowers target-neutral.

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

    class XpuKernelAttr;   // @Occupancy override view (applyOccupancy hook)

    // Block-padded LDS tile (BlockPadded<T,Block,Pad>): the physical slot is
    // `logical + (logical/period)*pad`. Additive padding does NOT distribute over
    // base+offset, so `ptr` is the bare base and `baseOffset` the logical offset.
    struct LdsBlockPad {
        uint32_t period = 0;                // block size in elements (0 = none)
        uint32_t pad = 0;                   // padding elements per block
        llvm::Value* baseOffset = nullptr;  // logical offset (ptr is the bare base)
    };

    class LoweringTarget {
    public:
        virtual ~LoweringTarget() = default;

        // Lowercase backend name (diagnostics).
        virtual const char* name() const = 0;

        // --- the degrade seam -------------------------------------------------
        // A capability has a Native lowering where the silicon has it and a
        // Portable degrade elsewhere, per backend + a CAJETA_GPU_<F>_IMPL override.
        enum class ImplTier { Native, Portable };

        // Reach of a scoped memory fence: Workgroup = LDS/shared + global within
        // the block; Device = global memory across the whole device.
        enum class FenceScope { Workgroup, Device };

        // User-selectable ordering for a kernel atomic or fence — ordinals MUST
        // match cajeta.xpu.MemoryOrder; `Default` keeps the backend's own default.
        enum class MemoryOrder {
            Relaxed = 0, Acquire = 1, Release = 2, AcqRel = 3, SeqCst = 4,
            Default = -1
        };

        // Map a user MemoryOrder to an LLVM AtomicOrdering, `Default` falling back
        // to `fallback`. The CAS failure ordering is derived per LLVM's rule.
        static llvm::AtomicOrdering toAtomicOrdering(MemoryOrder o,
                                                     llvm::AtomicOrdering fallback);
        static llvm::AtomicOrdering casFailureOrdering(llvm::AtomicOrdering success);

        // User spec-constant slots map to SpecId kFirstUserSpecId + slot; SpecId
        // 0-3 are the runtime's reserved bank (workgroup dims, dynamic-shared).
        static constexpr unsigned kFirstUserSpecId = 4;
        static constexpr unsigned kMaxUserSpecConstants = 16;

        // Address space for entry-block allocas (the mutable scalar-slot model).
        // NVPTX: 0 (generic). AMDGPU: 5 (private) — an AS-0 alloca is invalid there.
        virtual unsigned allocaAddressSpace() const = 0;

        // True when kernel params arrive as DESCRIPTORS bound in the body (Vulkan:
        // a no-param `void main()`), false when they are real function arguments.
        virtual bool descriptorBoundParams() const { return false; }

        // Does this backend honour `!nontemporal` with a real cache policy? AMDGPU
        // and NVPTX do; CPU/SPIR-V leave it off, so the manifest cannot overclaim.
        virtual bool supportsNontemporal() const { return false; }

        // Leaf coordinate reads (dim 0/1/2 = x/y/z). Build into `b`'s current
        // insert point; insert any intrinsic decls into `m`.
        virtual llvm::Value* threadId(llvm::IRBuilderBase& b, llvm::Module& m,
                                      unsigned dim) = 0;     // local / workitem id
        virtual llvm::Value* workgroupId(llvm::IRBuilderBase& b, llvm::Module& m,
                                         unsigned dim) = 0;  // block / CTA id
        virtual llvm::Value* workgroupDim(llvm::IRBuilderBase& b, llvm::Module& m,
                                          unsigned dim) = 0; // block dim (ntid)

        // Global thread index. Default workgroupId*workgroupDim + threadId, correct
        // on both backends; virtual for a native global-id intrinsic.
        virtual llvm::Value* globalId(llvm::IRBuilderBase& b, llvm::Module& m,
                                      unsigned dim);

        // Total work-items in dim (gridDim·blockDim) — the grid-stride loop's
        // stride. Returns i32.
        virtual llvm::Value* gridSize(llvm::IRBuilderBase& b, llvm::Module& m,
                                      unsigned dim) = 0;

        // Workgroup barrier (synchronize all threads in the block, with the
        // memory ordering the backend needs for LDS visibility).
        virtual void workgroupBarrier(llvm::IRBuilderBase& b,
                                      llvm::Module& m) = 0;

        // Scoped memory fence: order and make visible the accesses at `scope`, with
        // NO thread rendezvous. Default: a system-scope acq_rel `fence`.
        virtual void memoryFence(llvm::IRBuilderBase& b, llvm::Module& m,
                                 FenceScope scope,
                                 MemoryOrder order = MemoryOrder::Default);

        // Async global->shared copy of `count` elements, striped across the
        // workgroup. The DEFAULT is a SYNCHRONOUS strided copy — correct, not fast.
        virtual void asyncCopy(llvm::IRBuilderBase& b, llvm::Module& m,
                               llvm::Value* dstBase, llvm::Type* dstElem,
                               llvm::Value* dstOffset, llvm::Value* srcBase,
                               llvm::Type* srcElem, llvm::Value* srcOffset,
                               llvm::Value* count);

        // Close the current async-copy group (the unit `asyncWait` counts).
        virtual void asyncCommit(llvm::IRBuilderBase& b, llvm::Module& m);

        // Block until at most `groupsInFlight` committed groups remain outstanding.
        // Default no-op (synchronous fallback); AMDGPU: s_waitcnt vmcnt.
        virtual void asyncWait(llvm::IRBuilderBase& b, llvm::Module& m,
                               llvm::Value* groupsInFlight);

        // Instruction-scheduling hints: steer how the backend interleaves matrix-
        // core / LDS / global instructions. ImmArg operands; the DEFAULT is a NO-OP.
        virtual void schedBarrier(llvm::IRBuilderBase& b, llvm::Module& m,
                                  uint32_t mask);
        virtual void schedGroupBarrier(llvm::IRBuilderBase& b, llvm::Module& m,
                                       uint32_t mask, uint32_t size,
                                       uint32_t syncId);
        virtual void schedPriority(llvm::IRBuilderBase& b, llvm::Module& m,
                                   uint32_t level);
        virtual void schedPipelineOpt(llvm::IRBuilderBase& b, llvm::Module& m,
                                      uint32_t strategy);

        // Conflict-free LDS swizzle of flat index `idx` (i64) in a `Swizzled<T,S>`
        // tile: `row*S + (col ^ (row & (S-1)))`, an involution. DEFAULT: identity.
        virtual llvm::Value* swizzleAddr(llvm::IRBuilderBase& b, llvm::Value* idx,
                                         uint32_t stride);

        // Block padding: map flat index `idx` to its physical slot
        // `idx + (idx/period)*pad`; `period`==0 is the identity. DEFAULT: identity.
        virtual llvm::Value* blockPadAddr(llvm::IRBuilderBase& b, llvm::Value* idx,
                                          uint32_t period, uint32_t pad);

        // Device printf: `fmt` is an i8* constant format string, `args` the already
        // lowered scalars (no C varargs in the language). The default REJECTS.
        virtual void devicePrintf(llvm::IRBuilderBase& b, llvm::Module& m,
                                  llvm::Value* fmt,
                                  llvm::ArrayRef<llvm::Value*> args);

        // Specialization constant `Spec.geti(slot, default)` → i32. The DEFAULT
        // bakes an i32 literal; Vulkan emits an override-able OpSpecConstant.
        virtual llvm::Value* specConstantI32(llvm::IRBuilderBase& b,
                                             llvm::Module& m, unsigned slot,
                                             int32_t defaultValue);

        // f32 companion of specConstantI32 (`Spec.getf(slot, default)`), same model:
        // Vulkan a real float OpSpecConstant, the others bake the literal.
        virtual llvm::Value* specConstantF32(llvm::IRBuilderBase& b,
                                             llvm::Module& m, unsigned slot,
                                             float defaultValue);

        // Decorate a freshly-created kernel function: calling convention plus any
        // marker metadata. NVPTX: ptx_kernel + nvvm.annotations. AMDGPU: the CC.
        virtual void decorateKernel(llvm::Function* fn, llvm::Module& m) = 0;

        // Called once at finalization IFF the body used a cross-lane subgroup op: a
        // hook for requesting maximal reconvergence (Vulkan). Default no-op.
        virtual void onSubgroupOpsUsed(llvm::Function* /*fn*/,
                                       llvm::Module& /*m*/) {}

        // Apply an @Occupancy override to a freshly-lowered kernel. Runs before the
        // auto budgeting, so an explicit override wins.
        virtual void applyOccupancy(llvm::Function* /*fn*/,
                                    const XpuKernelAttr& /*attr*/) {}

        // --- kernel signature / parameter model (the Vulkan fork) -------------
        // NVPTX/AMDGPU take kernel arguments as a flat parameter list; Vulkan has no
        // raw-pointer kernel ABI, so it alone overrides the three hooks below.

        // One admitted kernel parameter, as seen by the signature/prologue hooks.
        struct KernelParam {
            std::string name;
            bool isBuffer;       // Buffer<T>/array (else a scalar primitive)
            llvm::Type* type;    // buffer element type, scalar type, or (sampler)
                                 // the {i32 filterMode, i32 addressMode} struct
            bool isSigned;       // scalar signedness / buffer-element signedness
            bool isTexture = false;  // Texture2D<T>/Texture3D<T> — a sampled-image
                                     // handle carried per backend; `type` is the
                                     // texel scalar (i32 for raw-integer formats).
            bool isSampler = false;  // Sampler — filter/address descriptor (Item 8).
                                     // `type` is the {i32,i32} mode struct.
            bool isAccelStruct = false;  // AccelerationStructure — a descriptor-
                                     // bound BVH, carried as a backend handle;
                                     // `type` unused. Ray-query only.
            bool isImage = false;    // Image2D — a writable 2-D storage image
                                     // carried as a backend handle; `type` is the
                                     // texel scalar (f32), written by img.store().
            int textureDim = 2;      // texture KIND for isTexture params: 1 = Texture1D,
                                     // 2 = Texture2D, 3 = Texture3D, 4 = 2DArray,
                                     // 5 = TextureCube. Selects dimensionality/arity.
            bool isBufferArray = false;  // Buffer<T>[] — a bindless descriptor ARRAY of
                                     // buffers (`bufs[idx][i]`). isBuffer is ALSO
                                     // true; this adds the outer array binding.
            bool isPushConstant = false;  // @PushConstant (cajeta-gfx §4.b-rest) — a
                                     // by-value scalar riding a graphics stage's
                                     // PushConstant block. Vulkan-only.
        };

        // Create the kernel function for `name`. Default: void-returning, one ptr
        // addrspace(1) per buffer + the scalar type per primitive, + decorateKernel.
        virtual llvm::Function* createKernel(
            llvm::Module& m, const std::string& name,
            const std::vector<KernelParam>& params);

        // Materialize parameter `idx` into `b`'s entry block: a scalar value, or a
        // buffer base/handle for bufferBases. Default fn->getArg; Vulkan binds here.
        virtual llvm::Value* materializeParam(
            llvm::IRBuilderBase& b, llvm::Module& m, llvm::Function* fn,
            unsigned idx, const KernelParam& p);

        // --- graphics shader output -------------------------------------------
        // A graphics entry is `void main()` writing an output interface variable, so
        // when this is true `return <expr>` becomes storeShaderOutput + `ret void`.
        virtual bool shaderOutputReturn() const { return false; }

        // Store a graphics shader's evaluated `return` value into its stage output
        // variable. Only called when shaderOutputReturn() is true.
        virtual void storeShaderOutput(llvm::IRBuilderBase& /*b*/,
                                       llvm::Module& /*m*/, llvm::Function* /*fn*/,
                                       llvm::Value* /*value*/) {}

        // Pointer to buffer element `index` of `base` (`index` already i64). Default
        // is a GEP; Vulkan routes descriptor handles through resource.getpointer.
        virtual llvm::Value* bufferElementPtr(
            llvm::IRBuilderBase& b, llvm::Module& m, llvm::Value* base,
            llvm::Type* elemTy, llvm::Value* index);

        // Bindless descriptor array (Buffer<T>[]): select descriptor `descIndex` of
        // the array bound at `binding`, returning a base for bufferElementPtr.
        virtual llvm::Value* bufferArrayElement(
            llvm::IRBuilderBase& b, llvm::Module& m, llvm::Function* fn,
            unsigned binding, llvm::Value* arrayBase, llvm::Type* elemTy,
            llvm::Value* descIndex);

        // Kernel-aware vectorized load/store over `lanes` contiguous elements
        // (`index` i64). Default: bufferElementPtr + a packed vector op.
        virtual llvm::Value* vectorLoad(
            llvm::IRBuilderBase& b, llvm::Module& m, llvm::Value* base,
            llvm::Type* elemTy, unsigned lanes, llvm::Value* index);
        virtual void vectorStore(
            llvm::IRBuilderBase& b, llvm::Module& m, llvm::Value* base,
            llvm::Type* elemTy, unsigned lanes, llvm::Value* index,
            llvm::Value* value);

        // Sample 2-D texture `texHandle` at normalized (u, v) through
        // `samplerHandle` at explicit mip `lod`. Default: unsupported (XPU-N01).
        virtual llvm::Value* sampleTexture(
            llvm::IRBuilderBase& b, llvm::Module& m,
            llvm::Value* texHandle, llvm::Value* samplerHandle,
            llvm::Value* u, llvm::Value* v, llvm::Value* lod);

        // texelFetch: the unfiltered, sampler-less twin of sampleTexture — exact
        // texel (x, y) at mip `lod` as <4 x texelTy>. Default: unsupported.
        virtual llvm::Value* fetchTexture(
            llvm::IRBuilderBase& b, llvm::Module& m,
            llvm::Value* texHandle, llvm::Value* x, llvm::Value* y,
            llvm::Type* texelTy, llvm::Value* lod);

        // Texture3D — the volumetric twins of sampleTexture/fetchTexture: a
        // 3-component coordinate and a 3-D image. Default: unsupported.
        virtual llvm::Value* sampleTexture3D(
            llvm::IRBuilderBase& b, llvm::Module& m,
            llvm::Value* texHandle, llvm::Value* samplerHandle,
            llvm::Value* u, llvm::Value* v, llvm::Value* w);

        virtual llvm::Value* fetchTexture3D(
            llvm::IRBuilderBase& b, llvm::Module& m,
            llvm::Value* texHandle, llvm::Value* x, llvm::Value* y, llvm::Value* z,
            llvm::Type* texelTy);

        // Texture1D — the linear twins: a single-component coordinate and a 1-D
        // image. No `lod` operand (mipmaps are 2-D only). Default: unsupported.
        virtual llvm::Value* sampleTexture1D(
            llvm::IRBuilderBase& b, llvm::Module& m,
            llvm::Value* texHandle, llvm::Value* samplerHandle, llvm::Value* u);

        virtual llvm::Value* fetchTexture1D(
            llvm::IRBuilderBase& b, llvm::Module& m,
            llvm::Value* texHandle, llvm::Value* x, llvm::Type* texelTy);

        // Texture2DArray — 2-D twins with an extra INTEGER `layer` (i32) selecting
        // one plane; NO cross-layer filtering. Default: unsupported.
        virtual llvm::Value* sampleTexture2DArray(
            llvm::IRBuilderBase& b, llvm::Module& m,
            llvm::Value* texHandle, llvm::Value* samplerHandle,
            llvm::Value* u, llvm::Value* v, llvm::Value* layer);

        virtual llvm::Value* fetchTexture2DArray(
            llvm::IRBuilderBase& b, llvm::Module& m,
            llvm::Value* texHandle, llvm::Value* x, llvm::Value* y,
            llvm::Value* layer, llvm::Type* texelTy);

        // TextureCube — sampled by a DIRECTION vector (x, y, z), the hardware
        // picking the face it points at. No fetch. Default: unsupported.
        virtual llvm::Value* sampleTextureCube(
            llvm::IRBuilderBase& b, llvm::Module& m,
            llvm::Value* texHandle, llvm::Value* samplerHandle,
            llvm::Value* x, llvm::Value* y, llvm::Value* z);

        // Store f32 `value` into 2-D storage image `imgHandle` at integer texel
        // (x, y) — `img.store(x, y, value)`. Default: unsupported (XPU-N01).
        virtual void storeImage(
            llvm::IRBuilderBase& b, llvm::Module& m,
            llvm::Value* imgHandle, llvm::Value* x, llvm::Value* y,
            llvm::Value* value);

        // Read the texel of storage image `imgHandle` at integer (x, y) as an f32 —
        // the read twin of storeImage. Default: unsupported (XPU-N01).
        virtual llvm::Value* loadImage(
            llvm::IRBuilderBase& b, llvm::Module& m,
            llvm::Value* imgHandle, llvm::Value* x, llvm::Value* y);

        // --- transcendental math ----------------------------------------------
        // The DEFAULT emits the matching `llvm.*` intrinsic; AMD OVERRIDES to
        // `__ocml_<name>_f32` (AMDGPU mis-lowers llvm.sin without range reduction).
        virtual llvm::Value* transcendental(
            llvm::IRBuilderBase& b, llvm::Module& m, const std::string& name,
            llvm::ArrayRef<llvm::Value*> args);

        // --- integer dot product (DP4a) ---------------------------------------
        // <4 x i8> dot -> i32 with i32 accumulation (`acc` is 0 for a plain dot).
        // DEFAULT: a portable widening reduce; Vulkan emits llvm.spv.dot4add.
        virtual llvm::Value* integerDot4x8(
            llvm::IRBuilderBase& b, llvm::Module& m, llvm::Value* a,
            llvm::Value* c, llvm::Value* acc, bool aSigned, bool cSigned);

        // WIDE `dotAccum` for an ISA with a wide fused int8 dot (x86 vpdpbusd takes
        // 16 or 64 lanes at once, which the per-lane slicing destroys). nullptr = none.
        virtual llvm::Value* integerDotWide(
            llvm::IRBuilderBase& /*b*/, llvm::Module& /*m*/,
            llvm::Value* /*w*/, llvm::Value* /*a*/, llvm::Value* /*acc*/,
            bool /*wUnsigned*/) { return nullptr; }

        // `Vector<int8,N>.lut4(table)` -> out[i] = table[indices[i] & 15]. DEFAULT:
        // spill + per-lane gather; AMDGPU emits v_perm_b32 as a byte-permute LUT.
        virtual llvm::Value* byteLut16(
            llvm::IRBuilderBase& b, llvm::Module& m, llvm::Value* indices,
            llvm::Value* table);

        // --- float atomics -----------------------------------------------------
        // `Buffer<float32>.atomic{Add,Min,Max}(i, v)` — an atomic RMW returning the
        // OLD value. DEFAULT monotonic/system; Vulkan Device + AcquireRelease.
        enum class AtomicFloatOp { Add, Min, Max };
        virtual llvm::Value* atomicFloatRMW(
            llvm::IRBuilderBase& b, llvm::Module& m, AtomicFloatOp op,
            llvm::Value* ptr, llvm::Value* value,
            MemoryOrder order = MemoryOrder::Default);

        // --- integer atomics (core SPIR-V) ------------------------------------
        // `Buffer<int32|uint32>.atomic{Add,...,Exchange}(i, v)` — RMW returning the
        // OLD value; `isSigned` picks S vs U min/max. DEFAULT monotonic/system.
        enum class AtomicIntOp { Add, Sub, Min, Max, And, Or, Xor, Exchange };
        virtual llvm::Value* atomicIntRMW(
            llvm::IRBuilderBase& b, llvm::Module& m, AtomicIntOp op,
            llvm::Value* ptr, llvm::Value* value, bool isSigned,
            MemoryOrder order = MemoryOrder::Default);

        // `atomicCompareExchange(i, expected, desired)`: set element `i` to
        // `desired` iff it equals `expected`, returning the OLD value.
        virtual llvm::Value* atomicCompareExchange(
            llvm::IRBuilderBase& b, llvm::Module& m, llvm::Value* ptr,
            llvm::Value* expected, llvm::Value* desired,
            MemoryOrder order = MemoryOrder::Default);

        // --- shader clock ------------------------------------------------------
        // `Thread.clock()` — a free-running 64-bit hardware counter for in-kernel
        // timing. Ticks are for RELATIVE measurement, not wall-clock seconds.
        virtual llvm::Value* readClock(llvm::IRBuilderBase& b, llvm::Module& m);

        // --- ray query (the Vulkan-only fork) ---------------------------------
        // A RayQuery local is a function-local opaque object whose ops lower to the
        // llvm.spv.ray.query.* intrinsics; every default rejects it (XPU-N02).

        // The impl this backend builds an AccelerationStructure as: the noun's impl
        // determines the verb's lowering, so this is the single source rayQueryTier
        // derives from. Ordinals MUST match CajetaAsImpl in cajeta_noun_impl.h.
        enum class NounImpl { SoftwareBvh = 0, VulkanNative = 1, Optix = 2 };
        virtual NounImpl accelImpl() const { return NounImpl::VulkanNative; }

        // The ray-query verb tier in the unified ImplTier vocabulary, derived from
        // the noun's recorded impl so the verb follows the noun (one source).
        ImplTier rayQueryTier() const {
            return accelImpl() == NounImpl::SoftwareBvh ? ImplTier::Portable
                                                        : ImplTier::Native;
        }

        // True when this backend has no native inline ray query and takes the
        // portable tier: a RayQuery lowers to the cajeta.xpu.SoftwareRayQuery walk
        // over a BVH `Buffer<float32>`. Thin alias over rayQueryTier().
        bool softwareRayQuery() const { return rayQueryTier() == ImplTier::Portable; }

        // The LLVM type to alloca for a `RayQuery` local. Vulkan:
        // target("spirv.RayQueryKHR"). Default: unsupported.
        virtual llvm::Type* rayQueryType(llvm::Module& m);

        // rq.initialize(as, rayFlags, cullMask, origin<3xf32>, tMin,
        // direction<3xf32>, tMax): `rqPtr` is the RayQuery alloca, `asHandle` the
        // materialized AccelerationStructure descriptor. Void op.
        virtual void rayQueryInitialize(
            llvm::IRBuilderBase& b, llvm::Module& m,
            llvm::Value* rqPtr, llvm::Value* asHandle,
            llvm::Value* rayFlags, llvm::Value* cullMask,
            llvm::Value* origin, llvm::Value* tMin,
            llvm::Value* direction, llvm::Value* tMax);

        // rq.proceed() → i1 (→ OpRayQueryProceedKHR).
        virtual llvm::Value* rayQueryProceed(
            llvm::IRBuilderBase& b, llvm::Module& m, llvm::Value* rqPtr);

        // rq.committedType()/candidateType() → i32; `intersection` selects
        // committed (1) or candidate (0).
        virtual llvm::Value* rayQueryIntersectionType(
            llvm::IRBuilderBase& b, llvm::Module& m, llvm::Value* rqPtr,
            llvm::Value* intersection);

        // rq.candidatePrimitiveIndex() → i32 — which indexed primitive was hit.
        virtual llvm::Value* rayQueryIntersectionPrimitiveIndex(
            llvm::IRBuilderBase& b, llvm::Module& m, llvm::Value* rqPtr,
            llvm::Value* intersection);

        // Nearest-hit getters + commit; `intersection` selects candidate (0) or
        // committed (1). Default throws — only SpirvTarget overrides.

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

        // --- cooperative matrix -----------------------------------------------
        // Device-only subgroup matrix-core tiles, all at Subgroup scope. Vulkan
        // lowers them to SPV_KHR_cooperative_matrix; other defaults throw (XPU-N03).

        // Which lowering a `CooperativeMatrix<T,Rows,Cols,Use>` gets here: Native (a
        // hardware matrix core) or Portable (a flat-tile loop). Static, no branch.
        virtual ImplTier coopMatrixTier(llvm::Type* /*elem*/,
                                        uint32_t /*rows*/, uint32_t /*cols*/,
                                        uint32_t /*use*/) {
            return ImplTier::Portable;
        }

        // The LLVM type to alloca for a `CooperativeMatrix<T,Rows,Cols,Use>`. Only
        // called on the Native tier; the Portable tier uses a flat tile.
        virtual llvm::Type* coopMatrixType(llvm::Module& m, llvm::Type* elem,
                                           uint32_t rows, uint32_t cols,
                                           uint32_t use);

        // m.load(src, layout, stride) → the loaded tile. `rows`/`cols`/`use` serve
        // per-lane backends; a whole-tile hardware load rejects swizzle/blockPad.
        virtual llvm::Value* coopMatrixLoad(
            llvm::IRBuilderBase& b, llvm::Module& m, llvm::Value* ptr,
            llvm::Value* layout, llvm::Value* stride, llvm::Type* matrixType,
            uint32_t rows, uint32_t cols, uint32_t use,
            uint32_t swizzleStride = 0, LdsBlockPad blockPad = {});

        // m.store(dst, layout, stride): store `matrixVal` to `ptr`. Shape and
        // swizzle operands as in coopMatrixLoad. Void op.
        virtual void coopMatrixStore(
            llvm::IRBuilderBase& b, llvm::Module& m, llvm::Value* ptr,
            llvm::Value* matrixVal, llvm::Value* layout, llvm::Value* stride,
            uint32_t rows, uint32_t cols, uint32_t use,
            uint32_t swizzleStride = 0, LdsBlockPad blockPad = {});

        // c.mma(a, b) → a*b+c. `signFlags` (A 0x1, B 0x2, C 0x4, Result 0x8) carries
        // the multiply's signedness as DATA — int types are signless in LLVM/SPIR-V.
        virtual llvm::Value* coopMatrixMulAdd(
            llvm::IRBuilderBase& b, llvm::Module& m, llvm::Value* a,
            llvm::Value* bMat, llvm::Value* c, llvm::Type* matrixType,
            uint32_t signFlags);

        // m.splat(value) → a tile with every element = `value` (the zero/initial
        // accumulator), result type `matrixType` (→ OpCompositeConstruct).
        virtual llvm::Value* coopMatrixSplat(
            llvm::IRBuilderBase& b, llvm::Module& m, llvm::Value* value,
            llvm::Type* matrixType);

        // Whether this backend lowers the fused GEMM-epilogue verbs on its NATIVE
        // tier; when false the tier scan demotes the kernel to the portable tile, so
        // coopMatrixEpilogueAccum is only ever called where this returned true.
        virtual bool coopMatrixEpilogueSupported() const { return false; }

        // Whether this backend's int8 operand fragment is an EXPLICIT per-lane
        // encoding the generic lowering may build from four i32 words (AMD WMMA).
        // False = opaque (SPIR-V), and the verb is rejected on the native tier.
        virtual bool coopMatrixFromWordsSupported() const { return false; }

        // facc[r][c] += (rowF[r]*colF[c])*acc[r][c] per fragment element of the
        // CURRENT lane; that association is the cross-tier CONTRACT.
        virtual llvm::Value* coopMatrixEpilogueAccum(
            llvm::IRBuilderBase& b, llvm::Module& m, llvm::Value* accVal,
            llvm::Value* faccVal, llvm::Value* rowFPtr, llvm::Type* rowETy,
            llvm::Value* colFPtr, llvm::Type* colETy,
            llvm::Value* rowGPtr = nullptr, llvm::Value* colGPtr = nullptr,
            llvm::Value* colFScalar = nullptr,
            llvm::Value* colGScalar = nullptr);

        // Called once on the kernel the first time a NATIVE coop-matrix tile is
        // allocated: a backend with ABI requirements (AMD WMMA is wave32) sets them.
        virtual void prepareNativeCoopMatrix(llvm::Function* /*fn*/) {}

        // The LLVM type of a Buffer<T> passed BY VALUE to a @Device helper — the
        // base held in bufferBases, which must match how the kernel materialized it.
        // Default: ptr addrspace(1); CPU flat, Vulkan the storage-buffer handle.
        virtual llvm::Type* bufferParamType(llvm::Module& m, llvm::Type* elemTy);

        // The LLVM type of a Texture2D as a kernel parameter in the flat pointer-arg
        // model. AMDGPU: ptr addrspace(4) to the HIP texture object. Default: i64.
        virtual llvm::Type* textureParamType(llvm::Module& m);

        // --- wave / subgroup ops ----------------------------------------------
        // Every backend has hardware wave ops but they diverge in intrinsic, lane
        // width and ballot shape, so these are pure-virtual seam points.

        // Lanes per wave: i32. NVPTX warpsize sreg (32); AMDGPU wavefrontsize
        // intrinsic; Vulkan spv.wave.get_lane_count.
        virtual llvm::Value* waveWidth(llvm::IRBuilderBase& b,
                                       llvm::Module& m) = 0;

        // The COOPERATIVE-GROUP width: on a GPU it is the wave width, so the default
        // delegates. The CPU backend OVERRIDES it to 1 — its cooperative unit is one
        // work-item, and SIMD is exploited below this abstraction.
        virtual llvm::Value* groupWidth(llvm::IRBuilderBase& b,
                                        llvm::Module& m) {
            return waveWidth(b, m);
        }

        // The lane's index within its cooperative group, in [0, groupWidth()). A GPU
        // takes waveLaneId(); CPU OVERRIDES to 0, degrading Group.stripe() to the
        // serial loop that is the correct CPU shape.
        virtual llvm::Value* groupLaneId(llvm::IRBuilderBase& b,
                                         llvm::Module& m) {
            return waveLaneId(b, m);
        }
        // NOTE: groupReduceF32* are declared below, once WaveReduceFOp is in scope.

        // Read i32 `value` from lane `srcLane` (i32), broadcast across the wave
        // (shuffle-by-index / readlane). Returns i32.
        virtual llvm::Value* waveShuffle(llvm::IRBuilderBase& b, llvm::Module& m,
                                         llvm::Value* value,
                                         llvm::Value* srcLane) = 0;

        // Ballot: an i64 bitmask whose bit i is set iff lane i's `pred` (i1) is
        // true. Backends whose native ballot is narrower (i32) zero-extend.
        virtual llvm::Value* waveBallot(llvm::IRBuilderBase& b, llvm::Module& m,
                                        llvm::Value* pred) = 0;

        // Like waveShuffle, but `srcLane` may be DIVERGENT. waveShuffle is
        // uniform-index on some backends (AMDGPU readlane needs an SGPR), so a
        // computed per-lane source uses this. Default = waveShuffle.
        virtual llvm::Value* waveShuffleDivergent(llvm::IRBuilderBase& b,
                                                  llvm::Module& m,
                                                  llvm::Value* value,
                                                  llvm::Value* srcLane) {
            return waveShuffle(b, m, value, srcLane);
        }

        // Wave-wide sum over i32 across the active lanes → i32. A single hardware
        // intrinsic on all three backends (NVPTX redux.sync needs sm_80+).
        virtual llvm::Value* waveReduceSum(llvm::IRBuilderBase& b,
                                           llvm::Module& m,
                                           llvm::Value* value) = 0;

        // The reduction family beyond sum: one i32 reduced across the active lanes,
        // every lane receiving the result. Max/Min are UNSIGNED. Product is
        // intentionally absent (no AMD/NVPTX hardware reduce).
        enum class WaveReduceOp { Max, Min, And, Or, Xor };
        virtual llvm::Value* waveReduce(llvm::IRBuilderBase& b, llvm::Module& m,
                                        WaveReduceOp op, llvm::Value* value) = 0;

        // FLOAT wave reduction (sum / max) of an f32 across the active lanes. NOT
        // pure-virtual: the default is a width-agnostic XOR butterfly over
        // waveShuffleDivergent with f32↔i32 punning. Backends override natively.
        enum class WaveReduceFOp { Sum, Max };
        virtual llvm::Value* waveReduceF32(llvm::IRBuilderBase& b,
                                           llvm::Module& m, WaveReduceFOp op,
                                           llvm::Value* value);

        // SEGMENTED float reduce: the same butterfly bounded at `seg` lanes, each
        // aligned group reducing independently. `seg` must divide the width and be a
        // power of two. This is what keeps a 32-lane block reduce right on wave64.
        virtual llvm::Value* waveReduceF32Segmented(llvm::IRBuilderBase& b,
                                                    llvm::Module& m,
                                                    WaveReduceFOp op,
                                                    llvm::Value* value,
                                                    llvm::Value* seg);

        // Cooperative-group float reduce: on a GPU the group IS the wave, so the
        // default delegates. CPU OVERRIDES to IDENTITY — its SIMD lanes each carry a
        // DIFFERENT row, so summing them would merge rows that must stay apart.
        virtual llvm::Value* groupReduceF32(llvm::IRBuilderBase& b,
                                            llvm::Module& m, WaveReduceFOp op,
                                            llvm::Value* value) {
            return waveReduceF32(b, m, op, value);
        }

        // Segmented cooperative-group float reduce: the GPU default delegates to
        // waveReduceF32Segmented; CPU overrides to identity, as above.
        virtual llvm::Value* groupReduceF32Segmented(llvm::IRBuilderBase& b,
                                                     llvm::Module& m,
                                                     WaveReduceFOp op,
                                                     llvm::Value* value,
                                                     llvm::Value* seg) {
            return waveReduceF32Segmented(b, m, op, value, seg);
        }

        // EXCLUSIVE prefix scan across the lanes (uint32): lane i receives the sum or
        // product of lanes 0..i-1, lane 0 the identity. Default: a width-agnostic
        // Hillis-Steele scan over waveShuffleDivergent.
        enum class WaveScanOp { Sum, Product };
        virtual llvm::Value* waveScan(llvm::IRBuilderBase& b, llvm::Module& m,
                                      WaveScanOp op, llvm::Value* value);

        // The calling work-item's lane index within its wave: i32 in [0, waveWidth).
        // NVPTX laneid; AMDGPU mbcnt; Vulkan SubgroupLocalInvocationId; CPU tid%w.
        virtual llvm::Value* waveLaneId(llvm::IRBuilderBase& b,
                                        llvm::Module& m) = 0;

        // Wave rotate: read i32 `value` from lane `(laneId + delta) mod width`. NOT
        // pure-virtual — the default is a width-agnostic shuffle over the existing
        // seams; Vulkan overrides to OpGroupNonUniformRotateKHR.
        virtual llvm::Value* waveRotate(llvm::IRBuilderBase& b, llvm::Module& m,
                                        llvm::Value* value, llvm::Value* delta);

        // --- quad (2x2) ops ----------------------------------------------------
        // A quad is four invocations with consecutive lane ids (laneId & ~3 .. +3).
        // NOT pure-virtual: the defaults are width-agnostic wave-seam forms.

        // Read i32 `value` from quad lane `index` (0-3); every lane in the quad
        // receives that lane's value. Vulkan: OpGroupNonUniformQuadBroadcast.
        virtual llvm::Value* quadBroadcast(llvm::IRBuilderBase& b,
                                           llvm::Module& m, llvm::Value* value,
                                           llvm::Value* index);

        // Exchange i32 `value` across the 2x2 quad: direction 0 = horizontal, 1 =
        // vertical, 2 = diagonal. The partner lane is laneId ^ (direction+1).
        virtual llvm::Value* quadSwap(llvm::IRBuilderBase& b, llvm::Module& m,
                                      llvm::Value* value, unsigned direction);

        // Quad-wide vote of a per-lane predicate (i1 -> i1): all = true iff `pred`
        // holds for every lane of the quad, any = iff it holds for some. The
        // portable default reads a wave ballot and tests this lane's quad nibble.
        virtual llvm::Value* quadAll(llvm::IRBuilderBase& b, llvm::Module& m,
                                     llvm::Value* pred);
        virtual llvm::Value* quadAny(llvm::IRBuilderBase& b, llvm::Module& m,
                                     llvm::Value* pred);

        // A dynamic `shared T[n]` lowers to an external unsized [0 x T]
        // addrspace(3) global, the native model on NVPTX/AMDGPU. Vulkan cannot, so
        // a backend returning true gets a concrete internal [1 x T] instead.
        virtual bool dynamicSharedNeedsConcreteSize() const { return false; }
    };

    // The explicit-override layer of the degrade seam: apply the
    // CAJETA_GPU_<FEATURE>_IMPL env override to a per-backend BASE tier —
    // "software" → Portable, "native" → Native. The env is read here and ONLY here.
    LoweringTarget::ImplTier resolveImplTier(const char* feature,
                                             LoweringTarget::ImplTier base);

} // namespace xpu
} // namespace cajeta
