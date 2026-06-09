//
// SPIR-V (Vulkan) kernel lowering — see header.
//

#include "SpirvKernelLowering.h"
#include "SpirvBackend.h"

#include "../lowering/KernelLowering.h"
#include "../lowering/LoweringTarget.h"
#include "cajeta/error/Exception.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicsSPIRV.h"
#include "llvm/IR/Module.h"

#include <string>

namespace cajeta {
namespace xpu {
namespace vulkan {

namespace {

// SPIR-V StorageBuffer: storage-class enum value 12; the LLVM SPIR-V backend
// represents its pointers in address space 11. Function (private) allocas live
// in address space 0. (Probed against LLVM 23 + spirv-val, 2026-05-30.)
constexpr unsigned kStorageBufferSC = 12;
constexpr unsigned kStorageBufferAS = 11;
// Bindless descriptor-array size for a Buffer<T>[] param: a fixed compile-time
// range (handlefrombinding's `range` operand is a constant). The runtime binds
// `count` (≤ this) real descriptors and pads the rest with a valid buffer; the
// kernel reads only bufs[0..count). MUST match the launch marshalling cap
// (CallExpression.cpp) and the runtime (cajeta_runtime.c).
constexpr unsigned kMaxBindlessBuffers = 16;

#ifndef CAJETA_HAS_SPV_RAY_QUERY
// This Cajeta compiler was built against an LLVM WITHOUT the cajeta-llvm fork's
// SPV_KHR_ray_query intrinsics (llvm.spv.ray.query.*) — see plans/c0 /
// CAJETA-FORK.md. The ray-query *opaque types* are plain string-named
// TargetExtTypes (fine on stock LLVM), but the *operations* need the fork
// intrinsics. Rather than make cajeta itself fail to build against stock LLVM, a
// RayQuery in a kernel becomes a clean lowering-time diagnostic.
[[noreturn]] static void rayQueryNoForkToolchain() {
    throw cajeta::Exception(
        "XPU kernel lowering: SPV_KHR_ray_query is unavailable in this build — the "
        "Cajeta compiler was linked against an LLVM that lacks the ray-query "
        "intrinsics. Rebuild against the cajeta-llvm fork toolchain (plans/c0) to "
        "enable RayQuery / AccelerationStructure.", "XPU-N02");
}
#endif

#ifndef CAJETA_HAS_SPV_COOP_MATRIX
// As above, but for the SPV_KHR_cooperative_matrix operation intrinsics
// (llvm.spv.cooperative.matrix.*). The OpTypeCooperativeMatrixKHR *type* lowers
// on stock LLVM; only the ops need the fork. A CooperativeMatrix op in a kernel
// becomes a clean lowering-time diagnostic on a non-fork toolchain.
[[noreturn]] static void coopMatrixNoForkToolchain() {
    throw cajeta::Exception(
        "XPU kernel lowering: SPV_KHR_cooperative_matrix is unavailable in this "
        "build — the Cajeta compiler was linked against an LLVM that lacks the "
        "cooperative-matrix intrinsics. Rebuild against the cajeta-llvm fork "
        "toolchain (plans/c0) to enable CooperativeMatrix.", "XPU-N03");
}
#endif

// target("spirv.VulkanBuffer", [0 x elemTy], StorageBuffer, writable) — the
// handle type llvm.spv.resource.handlefrombinding returns for a (RW)Structured
// Buffer<elemTy>. Uniqued by LLVM, so rebuilding with the same args yields the
// same type (lets getpointer reuse a handle's own type as its overload).
llvm::TargetExtType* vkBufferType(llvm::LLVMContext& ctx, llvm::Type* elemTy,
                                  bool writable) {
    llvm::Type* runtimeArr = llvm::ArrayType::get(elemTy, 0);
    return llvm::TargetExtType::get(ctx, "spirv.VulkanBuffer", {runtimeArr},
                                    {kStorageBufferSC, writable ? 1u : 0u});
}

// target("spirv.Image", texelTy, Dim, Depth, Arrayed, MS, Sampled, Format) —
// the handle type llvm.spv.resource.handlefrombinding returns for a 2-D sampled
// Texture2D. Dim=1 → 2D, Depth=2 → not-a-depth-image, Arrayed=0, MS=0,
// Sampled=1 → used with a sampler, Format=0 → Unknown. Matches the upstream
// SampleLevel recipe (OpTypeImage <texel> 2D 2 0 0 1 Unknown). (Item 8 Stage B.)
// `dimOperand` is the SPIR-V Dim: 0 = 1D, 1 = 2D, 2 = 3D, 3 = Cube (the texture's
// textureDim kind maps 1→0, 2→1, 3→2, 4→1 (2D-array), 5→3 (cube)). `arrayed` is
// the Arrayed operand (1 for a 2-D array — the 3rd OpTypeImage int). A 1-D /
// 3-D / array / cube sampled image binds the same way; only Dim + Arrayed + the
// coord arity differ. (A 1-D sampled image needs the Sampled1D capability, and a
// cube the SampledCubeArray-adjacent caps, which the SPIR-V backend emits from
// the Dim/Arrayed it sees.)
llvm::TargetExtType* vkImageType(llvm::LLVMContext& ctx, llvm::Type* texelTy,
                                 unsigned dimOperand = 1, unsigned arrayed = 0) {
    return llvm::TargetExtType::get(ctx, "spirv.Image", {texelTy},
                                    {dimOperand, 2, arrayed, 0, 1, 0});
}

// The same OpTypeImage with Sampled=2 → a writable STORAGE image (no sampler) —
// the handle type for a 2-D Image2D. Dim=1 → 2D, Depth=2, Arrayed=0, MS=0,
// Sampled=2 → used WITHOUT a sampler (storage), Format=3 → R32f. The format is
// KNOWN (not Unknown) for two reasons: (1) it matches the runtime image's actual
// VK_FORMAT_R32_SFLOAT, and (2) Unknown would require StorageImageWriteWithoutFormat,
// which the SPIR-V backend only makes available for SPIR-V ≥ 1.6 in the Shader env
// — and cajeta's spirv-unknown-vulkan1.3-compute triple does not pin SPIR-V 1.6,
// so the write would be unsatisfiable. R32f needs only the always-present Shader
// capability. Written via llvm.spv.resource.store.2d (OpImageWrite).
llvm::TargetExtType* vkStorageImageType(llvm::LLVMContext& ctx,
                                        llvm::Type* texelTy) {
    return llvm::TargetExtType::get(ctx, "spirv.Image", {texelTy},
                                    {1, 2, 0, 0, 2, 3});
}

// target("spirv.Sampler") — the handle type for a Sampler descriptor (→
// OpTypeSampler). No type/int params; combined with an image at the sample site.
llvm::TargetExtType* vkSamplerType(llvm::LLVMContext& ctx) {
    return llvm::TargetExtType::get(ctx, "spirv.Sampler", {}, {});
}

// target("spirv.AccelerationStructureKHR") — the handle type for an
// AccelerationStructure descriptor (→ OpTypeAccelerationStructureKHR). No
// type/int params (zero-parameterized opaque). handlefrombinding materializes it
// into a UniformConstant descriptor (set 0, binding = idx) and an OpLoad — the
// path proven spirv-val-clean in cajeta-gpu Part C increment 2c — and the loaded
// handle feeds OpRayQueryInitializeKHR. (cajeta-gpu Part C ray query.)
llvm::TargetExtType* vkAccelStructType(llvm::LLVMContext& ctx) {
    return llvm::TargetExtType::get(ctx, "spirv.AccelerationStructureKHR", {}, {});
}

// target("spirv.RayQueryKHR") — the function-local ray-query opaque
// (→ OpTypeRayQueryKHR). Allocated per RayQuery kernel local; the alloca is the
// OpVariable Function the ray-query ops mutate.
llvm::TargetExtType* vkRayQueryType(llvm::LLVMContext& ctx) {
    return llvm::TargetExtType::get(ctx, "spirv.RayQueryKHR", {}, {});
}

// target("spirv.CooperativeMatrixKHR", elem, scope, rows, cols, use) — the
// device cooperative-matrix tile type (→ OpTypeCooperativeMatrixKHR). Scope is
// always Subgroup (3): the tile is held cooperatively across the wavefront.
llvm::TargetExtType* vkCoopMatrixType(llvm::LLVMContext& ctx, llvm::Type* elem,
                                      uint32_t rows, uint32_t cols,
                                      uint32_t use) {
    constexpr unsigned kSubgroupScope = 3;
    return llvm::TargetExtType::get(ctx, "spirv.CooperativeMatrixKHR", {elem},
                                    {kSubgroupScope, rows, cols, use});
}

// llvm.spv.resource.getpointer(handle, i32 index) -> ptr addrspace(11). `index`
// arrives i64-widened from the shared lowerer; SPIR-V wants i32.
llvm::Value* getElementPtr(llvm::IRBuilderBase& b, llvm::Module& m,
                           llvm::Value* handle, llvm::Value* index) {
    llvm::LLVMContext& ctx = m.getContext();
    llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
    llvm::PointerType* sbPtr = llvm::PointerType::get(ctx, kStorageBufferAS);
    llvm::Value* i32idx = b.CreateTrunc(index, i32, "bidx");
    // LLVM 23 made the index operand of llvm.spv.resource.getpointer an
    // overloaded type (was a fixed i32), so the intrinsic now has three
    // overload types: {result ptr, handle, index}. Passing only two ran the
    // signature decoder off the end of the type array.
    llvm::Function* gp = llvm::Intrinsic::getOrInsertDeclaration(
        &m, llvm::Intrinsic::spv_resource_getpointer,
        {sbPtr, handle->getType(), i32});
    return b.CreateCall(gp, {handle, i32idx}, "elem.ptr");
}

// llvm.spv.resource.handlefrombinding(set, binding, range, index, name) for a
// descriptor at set 0, the given binding.
llvm::Value* bindResource(llvm::IRBuilderBase& b, llvm::Module& m,
                          llvm::TargetExtType* bufTy, unsigned binding,
                          const std::string& name) {
    llvm::LLVMContext& ctx = m.getContext();
    llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
    llvm::Value* nameStr = b.CreateGlobalString(name, "xpu.res." + name);
    llvm::Function* hfb = llvm::Intrinsic::getOrInsertDeclaration(
        &m, llvm::Intrinsic::spv_resource_handlefrombinding, {bufTy});
    return b.CreateCall(
        hfb,
        {llvm::ConstantInt::get(i32, 0), llvm::ConstantInt::get(i32, binding),
         llvm::ConstantInt::get(i32, 1), llvm::ConstantInt::get(i32, 0), nameStr},
        name + ".h");
}

class SpirvTarget : public LoweringTarget {
public:
    const char* name() const override { return "spirv"; }

    // Function (private) storage class — SPIR-V allocas live in addrspace 0.
    unsigned allocaAddressSpace() const override { return 0; }

    llvm::Value* threadId(llvm::IRBuilderBase& b, llvm::Module& m,
                          unsigned dim) override {
        return readCoord(b, m, llvm::Intrinsic::spv_thread_id_in_group, dim);
    }
    llvm::Value* workgroupId(llvm::IRBuilderBase& b, llvm::Module& m,
                             unsigned dim) override {
        return readCoord(b, m, llvm::Intrinsic::spv_group_id, dim);
    }
    // Vulkan bakes the workgroup size as a compile-time constant (createKernel
    // sets numthreads = kVulkanLocalSizeX,1,1). The `WorkgroupSize` BuiltIn must
    // decorate a CONSTANT in a Vulkan shader — emitting it as a builtin *variable*
    // (spv_workgroup_size) produces SPIR-V that spirv-val rejects ("BuiltIn
    // WorkgroupSize must be a constant"), which is why any kernel that read the
    // block dim (e.g. a cooperative global→LDS staging copy) mis-lowered. Return
    // the baked constant directly — valid and exactly correct for this fixed size.
    llvm::Value* workgroupDim(llvm::IRBuilderBase& /*b*/, llvm::Module& m,
                              unsigned dim) override {
        return llvm::ConstantInt::get(llvm::Type::getInt32Ty(m.getContext()),
                                      dim == 0 ? kVulkanLocalSizeX : 1);
    }
    // SPIR-V exposes GlobalInvocationId natively — override the computed default.
    llvm::Value* globalId(llvm::IRBuilderBase& b, llvm::Module& m,
                          unsigned dim) override {
        return readCoord(b, m, llvm::Intrinsic::spv_thread_id, dim);
    }
    // Grid-stride stride = NumWorkgroups·WorkgroupSize. NumWorkgroups is a valid
    // builtin; the WorkgroupSize factor is the baked LocalSize constant (the
    // builtin-variable form is invalid Vulkan — see workgroupDim). GlobalSize
    // would require the OpenCL Kernel capability, so it is deliberately not used.
    llvm::Value* gridSize(llvm::IRBuilderBase& b, llvm::Module& m,
                          unsigned dim) override {
        return b.CreateMul(
            readCoord(b, m, llvm::Intrinsic::spv_num_workgroups, dim),
            workgroupDim(b, m, dim), "gridsize");
    }

    void workgroupBarrier(llvm::IRBuilderBase& b, llvm::Module& m) override {
        // Workgroup control + memory barrier in one intrinsic (ordering folded
        // in by the backend) — cleaner than AMD's fence/s_barrier/fence triple.
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::spv_group_memory_barrier_with_group_sync);
        b.CreateCall(f, {});
    }

    // Vulkan marks the entry via createKernel's attributes, not a CC here.
    void decorateKernel(llvm::Function* /*fn*/, llvm::Module& /*m*/) override {}

    // Cross-lane subgroup ops (Wave.shuffle/ballot/reduce) only behave as the
    // source structure implies under maximal reconvergence. Request it: the
    // backend turns this fn-attr into OpExecutionMode MaximallyReconvergesKHR
    // (SPV_KHR_maximal_reconvergence, enabled in SpirvBackend). Emitted ONLY for
    // wave kernels, so non-wave kernels carry no extra device requirement.
    void onSubgroupOpsUsed(llvm::Function* fn, llvm::Module& /*m*/) override {
        fn->addFnAttr("enable-maximal-reconvergence", "true");
    }

    // The Vulkan compute entry takes NO parameters: `void main()`-style, with
    // the HLSL compute markers. LocalSize is baked in (Vulkan fixes workgroup
    // size at SPIR-V compile time). Args arrive via descriptors (materializeParam).
    llvm::Function* createKernel(
        llvm::Module& m, const std::string& kname,
        const std::vector<KernelParam>& /*params*/) override {
        llvm::LLVMContext& ctx = m.getContext();
        auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx),
                                             /*vararg=*/false);
        auto* fn = llvm::Function::Create(fnTy, llvm::Function::ExternalLinkage,
                                          kname, &m);
        fn->addFnAttr("hlsl.shader", "compute");
        fn->addFnAttr("hlsl.numthreads",
                      std::to_string(kVulkanLocalSizeX) + ",1,1");
        return fn;
    }

    // Each kernel arg becomes a descriptor binding at (set 0, binding = idx).
    // A Buffer<T> yields the storage-buffer handle (kept in bufferBases); a
    // scalar is read from a single-element storage buffer at that binding and
    // returned by value (the caller stores it into a mutable slot).
    llvm::Value* materializeParam(llvm::IRBuilderBase& b, llvm::Module& m,
                                  llvm::Function* /*fn*/, unsigned idx,
                                  const KernelParam& p) override {
        // Texture2D (Item 8): a SAMPLED_IMAGE descriptor; the handle is consumed
        // only by `tex.sample(...)` via sampleTexture. (set 0, binding = idx.)
        if (p.isTexture) {
            // textureDim kind → (SPIR-V Dim, Arrayed): 1D=(0,0), 2D=(1,0),
            // 3D=(2,0), 2D-array=(1,1), cube=(3,0).
            unsigned dimOp = p.textureDim == 3 ? 2u
                           : (p.textureDim == 1 ? 0u
                           : (p.textureDim == 5 ? 3u : 1u));
            unsigned arrayed = (p.textureDim == 4) ? 1u : 0u;
            return bindResource(b, m,
                                vkImageType(m.getContext(), p.type, dimOp, arrayed),
                                idx, p.name);
        }
        // Image2D (writable images): a STORAGE_IMAGE descriptor; the handle is
        // consumed only by `img.store(...)` via storeImage. (set 0, binding = idx.)
        if (p.isImage) {
            return bindResource(b, m, vkStorageImageType(m.getContext(), p.type),
                                idx, p.name);
        }
        // Sampler (Item 8): a SAMPLER descriptor. The filter/address modes live
        // in the runtime VkSampler, not the SPIR-V — here it's just a handle.
        if (p.isSampler) {
            return bindResource(b, m, vkSamplerType(m.getContext()), idx, p.name);
        }
        // AccelerationStructure (Part C): an ACCELERATION_STRUCTURE_KHR
        // descriptor; the loaded handle is consumed only by `rq.initialize(as,
        // ...)`. (set 0, binding = idx — same handlefrombinding path as buffers.)
        if (p.isAccelStruct) {
            return bindResource(b, m, vkAccelStructType(m.getContext()), idx,
                                p.name);
        }
        if (p.isBuffer) {
            return bindResource(b, m, vkBufferType(m.getContext(), p.type, true),
                                idx, p.name);
        }
        // scalar -> single-element (read-only) SSBO, load element 0.
        llvm::Value* handle = bindResource(
            b, m, vkBufferType(m.getContext(), p.type, false), idx, p.name);
        llvm::Value* ptr = getElementPtr(
            b, m, handle,
            llvm::ConstantInt::get(llvm::Type::getInt64Ty(m.getContext()), 0));
        return b.CreateLoad(p.type, ptr, p.name);
    }

    // Texture2D.sample(sampler, u, v) → OpImageSampleExplicitLod … Lod, native
    // and compute-valid via llvm.spv.resource.samplelevel (LLVM 23). texHandle is
    // the spirv.Image, samplerHandle the spirv.Sampler (both descriptor handles
    // from materializeParam). coord = <u, v>; explicit LOD 0 (compute has no
    // implicit derivatives); no constant offset. Returns the R channel of the
    // <4 x float> gather (Texture2D's texel is a scalar float in v1). (Stage B.)
    llvm::Value* sampleTexture(llvm::IRBuilderBase& b, llvm::Module& m,
                               llvm::Value* texHandle, llvm::Value* samplerHandle,
                               llvm::Value* u, llvm::Value* v,
                               llvm::Value* lod) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* f32 = llvm::Type::getFloatTy(ctx);
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        auto* v2f = llvm::FixedVectorType::get(f32, 2);
        auto* v4f = llvm::FixedVectorType::get(f32, 4);
        auto* v2i = llvm::FixedVectorType::get(i32, 2);
        llvm::Value* coord = llvm::PoisonValue::get(v2f);
        coord = b.CreateInsertElement(coord, u, uint64_t(0));
        coord = b.CreateInsertElement(coord, v, uint64_t(1), "tex.coord");
        // Explicit LOD (0.0 for plain sample; the user's value for sampleLod).
        llvm::Value* offset = llvm::ConstantAggregateZero::get(v2i);
        // CreateIntrinsic infers the (result, image, sampler, coord, offset)
        // overloads from the operand types, exactly as clang's HLSL SampleLevel.
        llvm::Value* rgba = b.CreateIntrinsic(
            v4f, llvm::Intrinsic::spv_resource_samplelevel,
            {texHandle, samplerHandle, coord, lod, offset});
        // Return the full <4 x float> RGBA — Texture2D.sample is typed
        // Vector<float32,4>; the caller selects a channel with .r/.x. (R32f
        // images keep the value in lane 0; the bridge stops discarding the rest.)
        return rgba;
    }

    // Texture2D.fetch(x, y) → OpImageFetch at the exact integer coord, mip 0,
    // no sampler. Uses the fork intrinsic llvm.spv.resource.load.level
    // (image, coord, lod): because `texHandle` is the *sampled* image (Sampled=1,
    // from vkImageType), the backend's read/fetch selection picks OpImageFetch
    // (vs OpImageRead for a Sampled=2 storage image — that is how this differs
    // from loadImage). The Lod operand is mandatory for OpImageFetch on a
    // non-multisampled image, and load.level supplies it (= 0 here). Returns the
    // <4 x float> texel directly (Texture2D.fetch is typed Vector<float32,4>).
    llvm::Value* fetchTexture(llvm::IRBuilderBase& b, llvm::Module& m,
                              llvm::Value* texHandle, llvm::Value* x,
                              llvm::Value* y, llvm::Type* texelTy,
                              llvm::Value* lod) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        auto* v2i = llvm::FixedVectorType::get(i32, 2);
        // The result is <4 x T>: <4 x float> for the float formats, <4 x i32> for
        // the raw-integer formats. `texHandle` is already an integer-sampled image
        // when T = i32 (materializeParam built spirv.Image with sampled type i32
        // from p.type), so OpImageFetch on it yields integer texels natively.
        auto* v4t = llvm::FixedVectorType::get(texelTy, 4);
        llvm::Value* coord = llvm::PoisonValue::get(v2i);
        coord = b.CreateInsertElement(coord, x, uint64_t(0));
        coord = b.CreateInsertElement(coord, y, uint64_t(1), "tex.fetch.coord");
        // Explicit mip LOD (0 for plain fetch; the user value for fetchLod) — the
        // mandatory OpImageFetch Lod operand load.level supplies.
        return b.CreateIntrinsic(v4t, llvm::Intrinsic::spv_resource_load_level,
                                 {texHandle, coord, lod}, nullptr, "tex.fetch");
    }

    // Texture3D.sample(sampler, u, v, w) → OpImageSampleExplicitLod on a 3-D image
    // — the 3-coord twin of sampleTexture. texHandle is the 3-D spirv.Image (Dim=2,
    // bound by materializeParam from textureDim). coord = <u, v, w>; explicit LOD 0;
    // the constant offset is a <3 x i32> for a 3-D image.
    llvm::Value* sampleTexture3D(llvm::IRBuilderBase& b, llvm::Module& m,
                                 llvm::Value* texHandle, llvm::Value* samplerHandle,
                                 llvm::Value* u, llvm::Value* v,
                                 llvm::Value* w) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* f32 = llvm::Type::getFloatTy(ctx);
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        auto* v3f = llvm::FixedVectorType::get(f32, 3);
        auto* v4f = llvm::FixedVectorType::get(f32, 4);
        auto* v3i = llvm::FixedVectorType::get(i32, 3);
        llvm::Value* coord = llvm::PoisonValue::get(v3f);
        coord = b.CreateInsertElement(coord, u, uint64_t(0));
        coord = b.CreateInsertElement(coord, v, uint64_t(1));
        coord = b.CreateInsertElement(coord, w, uint64_t(2), "tex3d.coord");
        llvm::Value* lod = llvm::ConstantFP::get(f32, 0.0);
        llvm::Value* offset = llvm::ConstantAggregateZero::get(v3i);
        return b.CreateIntrinsic(v4f, llvm::Intrinsic::spv_resource_samplelevel,
                                 {texHandle, samplerHandle, coord, lod, offset});
    }

    // Texture3D.fetch(x, y, z) → OpImageFetch on a 3-D image at the exact integer
    // voxel, mip 0, no sampler — the 3-coord twin of fetchTexture. Result <4 x T>.
    llvm::Value* fetchTexture3D(llvm::IRBuilderBase& b, llvm::Module& m,
                                llvm::Value* texHandle, llvm::Value* x,
                                llvm::Value* y, llvm::Value* z,
                                llvm::Type* texelTy) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        auto* v3i = llvm::FixedVectorType::get(i32, 3);
        auto* v4t = llvm::FixedVectorType::get(texelTy, 4);
        llvm::Value* coord = llvm::PoisonValue::get(v3i);
        coord = b.CreateInsertElement(coord, x, uint64_t(0));
        coord = b.CreateInsertElement(coord, y, uint64_t(1));
        coord = b.CreateInsertElement(coord, z, uint64_t(2), "tex3d.fetch.coord");
        llvm::Value* lod = llvm::ConstantInt::get(i32, 0);
        return b.CreateIntrinsic(v4t, llvm::Intrinsic::spv_resource_load_level,
                                 {texHandle, coord, lod}, nullptr, "tex3d.fetch");
    }

    // Texture1D.sample(sampler, u) → OpImageSampleExplicitLod on a 1-D image — the
    // single-coord twin of sampleTexture. texHandle is the 1-D spirv.Image (Dim=0,
    // bound by materializeParam from textureDim). The coord is a SCALAR float (a
    // 1-D image takes a 1-component coordinate, not a vector) and the offset a
    // scalar i32; explicit LOD 0 (compute has no implicit derivatives).
    llvm::Value* sampleTexture1D(llvm::IRBuilderBase& b, llvm::Module& m,
                                 llvm::Value* texHandle, llvm::Value* samplerHandle,
                                 llvm::Value* u) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* f32 = llvm::Type::getFloatTy(ctx);
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        auto* v4f = llvm::FixedVectorType::get(f32, 4);
        llvm::Value* lod = llvm::ConstantFP::get(f32, 0.0);
        llvm::Value* offset = llvm::ConstantInt::get(i32, 0);
        return b.CreateIntrinsic(v4f, llvm::Intrinsic::spv_resource_samplelevel,
                                 {texHandle, samplerHandle, u, lod, offset});
    }

    // Texture1D.fetch(x) → OpImageFetch on a 1-D image at the exact integer texel,
    // mip 0, no sampler — the single-coord twin of fetchTexture. The coord is a
    // SCALAR i32. Result <4 x T>.
    llvm::Value* fetchTexture1D(llvm::IRBuilderBase& b, llvm::Module& m,
                                llvm::Value* texHandle, llvm::Value* x,
                                llvm::Type* texelTy) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        auto* v4t = llvm::FixedVectorType::get(texelTy, 4);
        llvm::Value* lod = llvm::ConstantInt::get(i32, 0);
        return b.CreateIntrinsic(v4t, llvm::Intrinsic::spv_resource_load_level,
                                 {texHandle, x, lod}, nullptr, "tex1d.fetch");
    }

    // Texture2DArray.sample(sampler, u, v, layer) → OpImageSampleExplicitLod on an
    // Arrayed 2-D image — the layered twin of sampleTexture. The coord is a
    // <3 x float> {u, v, layer} where the 3rd component is the (un-normalized)
    // array layer; `layer` arrives as i32 and is converted to float. The image is
    // Arrayed=1 (materializeParam), so the backend reads the 3rd coord as the
    // layer, not a 3-D w. Explicit LOD 0; <3 x i32> offset.
    llvm::Value* sampleTexture2DArray(llvm::IRBuilderBase& b, llvm::Module& m,
                                      llvm::Value* texHandle,
                                      llvm::Value* samplerHandle, llvm::Value* u,
                                      llvm::Value* v, llvm::Value* layer) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* f32 = llvm::Type::getFloatTy(ctx);
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        auto* v3f = llvm::FixedVectorType::get(f32, 3);
        auto* v4f = llvm::FixedVectorType::get(f32, 4);
        auto* v3i = llvm::FixedVectorType::get(i32, 3);
        llvm::Value* layerF = b.CreateSIToFP(layer, f32, "layer.f");
        llvm::Value* coord = llvm::PoisonValue::get(v3f);
        coord = b.CreateInsertElement(coord, u, uint64_t(0));
        coord = b.CreateInsertElement(coord, v, uint64_t(1));
        coord = b.CreateInsertElement(coord, layerF, uint64_t(2), "tex2da.coord");
        llvm::Value* lod = llvm::ConstantFP::get(f32, 0.0);
        llvm::Value* offset = llvm::ConstantAggregateZero::get(v3i);
        return b.CreateIntrinsic(v4f, llvm::Intrinsic::spv_resource_samplelevel,
                                 {texHandle, samplerHandle, coord, lod, offset});
    }

    // Texture2DArray.fetch(x, y, layer) → OpImageFetch on an Arrayed 2-D image at
    // the exact integer texel of `layer`, mip 0, no sampler — the layered twin of
    // fetchTexture. coord = <3 x i32> {x, y, layer}. Result <4 x T>.
    llvm::Value* fetchTexture2DArray(llvm::IRBuilderBase& b, llvm::Module& m,
                                     llvm::Value* texHandle, llvm::Value* x,
                                     llvm::Value* y, llvm::Value* layer,
                                     llvm::Type* texelTy) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        auto* v3i = llvm::FixedVectorType::get(i32, 3);
        auto* v4t = llvm::FixedVectorType::get(texelTy, 4);
        llvm::Value* coord = llvm::PoisonValue::get(v3i);
        coord = b.CreateInsertElement(coord, x, uint64_t(0));
        coord = b.CreateInsertElement(coord, y, uint64_t(1));
        coord = b.CreateInsertElement(coord, layer, uint64_t(2), "tex2da.fetch.coord");
        llvm::Value* lod = llvm::ConstantInt::get(i32, 0);
        return b.CreateIntrinsic(v4t, llvm::Intrinsic::spv_resource_load_level,
                                 {texHandle, coord, lod}, nullptr, "tex2da.fetch");
    }

    // TextureCube.sample(sampler, x, y, z) → OpImageSampleExplicitLod on a cube
    // image (Dim=Cube, bound by materializeParam from textureDim 5). The coord is
    // a <3 x float> DIRECTION (x, y, z) — the HW picks the face + projects; no
    // normalization needed. Explicit LOD 0. The <3 x i32> offset is constant zero,
    // which the backend drops (a ConstOffset is illegal on a cube image — but a
    // zero offset is elided by generateSampleImage, so this stays spirv-val clean).
    llvm::Value* sampleTextureCube(llvm::IRBuilderBase& b, llvm::Module& m,
                                   llvm::Value* texHandle, llvm::Value* samplerHandle,
                                   llvm::Value* x, llvm::Value* y,
                                   llvm::Value* z) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* f32 = llvm::Type::getFloatTy(ctx);
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        auto* v3f = llvm::FixedVectorType::get(f32, 3);
        auto* v4f = llvm::FixedVectorType::get(f32, 4);
        auto* v3i = llvm::FixedVectorType::get(i32, 3);
        llvm::Value* coord = llvm::PoisonValue::get(v3f);
        coord = b.CreateInsertElement(coord, x, uint64_t(0));
        coord = b.CreateInsertElement(coord, y, uint64_t(1));
        coord = b.CreateInsertElement(coord, z, uint64_t(2), "texcube.dir");
        llvm::Value* lod = llvm::ConstantFP::get(f32, 0.0);
        llvm::Value* offset = llvm::ConstantAggregateZero::get(v3i);
        return b.CreateIntrinsic(v4f, llvm::Intrinsic::spv_resource_samplelevel,
                                 {texHandle, samplerHandle, coord, lod, offset});
    }

    // Image2D.store(x, y, value) → a single OpImageWrite, native via the fork
    // intrinsic llvm.spv.resource.store.2d (cajeta-spirv): operands are the
    // spirv.Image storage handle (Sampled=2), the integer coord <x, y>, and the
    // texel. OpImageWrite requires a 4-component texel, so the scalar `value` is
    // splatted into <value, 0, 0, 0> (the R32f image keeps lane 0).
    void storeImage(llvm::IRBuilderBase& b, llvm::Module& m,
                    llvm::Value* imgHandle, llvm::Value* x, llvm::Value* y,
                    llvm::Value* value) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* f32 = llvm::Type::getFloatTy(ctx);
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        auto* v2i = llvm::FixedVectorType::get(i32, 2);
        auto* v4f = llvm::FixedVectorType::get(f32, 4);
        llvm::Value* coord = llvm::PoisonValue::get(v2i);
        coord = b.CreateInsertElement(coord, x, uint64_t(0));
        coord = b.CreateInsertElement(coord, y, uint64_t(1), "img.coord");
        llvm::Value* texel = llvm::ConstantAggregateZero::get(v4f);
        texel = b.CreateInsertElement(texel, value, uint64_t(0), "img.texel");
        // Overload types: the image handle (any) and the texel vector (anyvector);
        // the coord (v2i32) is a fixed operand, not overloaded.
        b.CreateIntrinsic(llvm::Intrinsic::spv_resource_store_2d,
                          {imgHandle->getType(), v4f},
                          {imgHandle, coord, texel});
    }

    // Image2D.load(x, y) → a single OpImageRead, native via the fork intrinsic
    // llvm.spv.resource.load.2d (cajeta-spirv): operands are the spirv.Image
    // storage handle (Sampled=2) and the integer coord <x, y>. The result is
    // requested as a scalar f32 — the SPIR-V backend reads the texel as a
    // <4 x f32> (OpImageRead) and extracts component 0 (the R32f channel).
    llvm::Value* loadImage(llvm::IRBuilderBase& b, llvm::Module& m,
                           llvm::Value* imgHandle, llvm::Value* x,
                           llvm::Value* y) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* f32 = llvm::Type::getFloatTy(ctx);
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        auto* v2i = llvm::FixedVectorType::get(i32, 2);
        llvm::Value* coord = llvm::PoisonValue::get(v2i);
        coord = b.CreateInsertElement(coord, x, uint64_t(0));
        coord = b.CreateInsertElement(coord, y, uint64_t(1), "img.coord");
        // Overload types: the scalar result (f32) and the image handle (any);
        // the coord (v2i32) is a fixed operand, not overloaded.
        return b.CreateIntrinsic(llvm::Intrinsic::spv_resource_load_2d,
                                 {f32, imgHandle->getType()},
                                 {imgHandle, coord}, nullptr, "img.load");
    }

    // --- integer dot product (SPV_KHR_integer_dot_product, DP4a) --------------
    // Pack the four int8 lanes into an i32 (lane 0 -> low byte = the first
    // packed component, matching PackedVectorFormat4x8Bit on little-endian GPUs)
    // and emit llvm.spv.dot4add.{i8,u8}packed(acc, x, y) -> i32. The SPIR-V
    // backend selects OpSDot/OpUDot ... PackedVectorFormat4x8Bit + OpIAdd, and
    // injects the DotProduct / DotProductInput4x8BitPacked capabilities + the
    // SPV_KHR_integer_dot_product extension (or the 1.5/1.6 bit-field expansion
    // when they are unavailable). Unlike coop-matrix/ray-query, dot4add is a
    // stock-LLVM intrinsic (the HLSL path), so no fork guard is needed.
    llvm::Value* integerDot4x8(llvm::IRBuilderBase& b, llvm::Module& m,
                               llvm::Value* a, llvm::Value* c, llvm::Value* acc,
                               bool isSigned) override {
        llvm::Type* i32 = llvm::Type::getInt32Ty(m.getContext());
        llvm::Value* x = b.CreateBitCast(a, i32, "dp4a.x");
        llvm::Value* y = b.CreateBitCast(c, i32, "dp4a.y");
        llvm::Intrinsic::ID id = isSigned
            ? llvm::Intrinsic::spv_dot4add_i8packed
            : llvm::Intrinsic::spv_dot4add_u8packed;
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(&m, id);
        return b.CreateCall(f, {acc, x, y}, "dp4a");
    }

    // Atomic memory scope from the target pointer's storage class. A shared
    // (Workgroup-storage) pointer — cajeta's `shared T[]`, LLVM addrspace 3 —
    // needs Workgroup scope; a global StorageBuffer pointer needs Device scope.
    // Vulkan rejects CrossDevice scope, and an atomic's scope must match where the
    // memory lives. (The storage-class bit of the memory semantics is derived by
    // the backend from the pointer itself; here we only pick the scope.)
    llvm::SyncScope::ID atomicScope(llvm::Module& m, llvm::Value* ptr) {
        constexpr unsigned kSharedAS = 3;   // matches lowerSharedDecl's addrspace
        const char* name =
            ptr->getType()->getPointerAddressSpace() == kSharedAS ? "workgroup"
                                                                  : "device";
        return m.getContext().getOrInsertSyncScopeID(name);
    }

    // --- float atomics (SPV_EXT_shader_atomic_float_add / _min_max) -----------
    // Vulkan rejects CrossDevice scope and SequentiallyConsistent / relaxed-with-
    // storage-class memory semantics on OpAtomicF*EXT, so emit the atomicrmw with
    // AcquireRelease + the storage-matched scope (Device for global buffers,
    // Workgroup for `shared` memory — see atomicScope). The op
    // (OpAtomicFAddEXT/FMinEXT/FMaxEXT) and its capability + extension are
    // selected by the backend from the BinOp.
    llvm::Value* atomicFloatRMW(llvm::IRBuilderBase& b, llvm::Module& m,
                                AtomicFloatOp op, llvm::Value* ptr,
                                llvm::Value* value) override {
        llvm::AtomicRMWInst::BinOp binop =
            op == AtomicFloatOp::Add ? llvm::AtomicRMWInst::FAdd
          : op == AtomicFloatOp::Min ? llvm::AtomicRMWInst::FMin
                                     : llvm::AtomicRMWInst::FMax;
        return b.CreateAtomicRMW(binop, ptr, value, llvm::MaybeAlign(),
                                 llvm::AtomicOrdering::AcquireRelease,
                                 atomicScope(m, ptr));
    }

    // --- integer atomics (core SPIR-V) ----------------------------------------
    // Same memory-model constraint as the float path: Device scope + AcquireRelease
    // (Vulkan rejects CrossDevice scope / SequentiallyConsistent). The backend maps
    // the BinOp to OpAtomicIAdd/ISub/And/Or/Xor/Exchange/SMin/UMin/SMax/UMax.
    llvm::Value* atomicIntRMW(llvm::IRBuilderBase& b, llvm::Module& m,
                              AtomicIntOp op, llvm::Value* ptr,
                              llvm::Value* value, bool isSigned) override {
        llvm::AtomicRMWInst::BinOp binop;
        switch (op) {
            case AtomicIntOp::Add:      binop = llvm::AtomicRMWInst::Add; break;
            case AtomicIntOp::Sub:      binop = llvm::AtomicRMWInst::Sub; break;
            case AtomicIntOp::And:      binop = llvm::AtomicRMWInst::And; break;
            case AtomicIntOp::Or:       binop = llvm::AtomicRMWInst::Or; break;
            case AtomicIntOp::Xor:      binop = llvm::AtomicRMWInst::Xor; break;
            case AtomicIntOp::Exchange: binop = llvm::AtomicRMWInst::Xchg; break;
            case AtomicIntOp::Min:
                binop = isSigned ? llvm::AtomicRMWInst::Min
                                 : llvm::AtomicRMWInst::UMin; break;
            case AtomicIntOp::Max:
                binop = isSigned ? llvm::AtomicRMWInst::Max
                                 : llvm::AtomicRMWInst::UMax; break;
            default:                    binop = llvm::AtomicRMWInst::Add; break;
        }
        return b.CreateAtomicRMW(binop, ptr, value, llvm::MaybeAlign(),
                                 llvm::AtomicOrdering::AcquireRelease,
                                 atomicScope(m, ptr));
    }

    // Compare-exchange → OpAtomicCompareExchange. Storage-matched scope (Device
    // for global, Workgroup for shared); AcquireRelease on success, Acquire on
    // failure (the failure ordering may not be stronger than success nor carry a
    // release). Returns the OLD value (element 0).
    llvm::Value* atomicCompareExchange(llvm::IRBuilderBase& b, llvm::Module& m,
                                       llvm::Value* ptr, llvm::Value* expected,
                                       llvm::Value* desired) override {
        llvm::Value* pair = b.CreateAtomicCmpXchg(
            ptr, expected, desired, llvm::MaybeAlign(),
            llvm::AtomicOrdering::AcquireRelease, llvm::AtomicOrdering::Acquire,
            atomicScope(m, ptr));
        return b.CreateExtractValue(pair, 0, "atomic.cas.old");
    }

    // --- shader clock (SPV_KHR_shader_clock) ----------------------------------
    // OpReadClockKHR at Subgroup scope (3), via the fork's llvm.spv.read.clock
    // intrinsic — the Shader flavor's only reach to the op (the OpReadClockKHR
    // builtin path is OpenCL-only). The SPIR-V backend injects the ShaderClockKHR
    // capability + SPV_KHR_shader_clock extension.
    llvm::Value* readClock(llvm::IRBuilderBase& b, llvm::Module& m) override {
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::spv_read_clock);
        return b.CreateCall(f, {b.getInt32(3)}, "clock");
    }

    // --- ray query (SPV_KHR_ray_query) ----------------------------------------
    // The ops lower to the llvm.spv.ray.query.* intrinsics + GlobalISel
    // selection (cajeta-gpu Part C increments 1/2/2c, in the cajeta-llvm fork).
    // The builtin __spirv_* path can't reach them: it is shader-gated to OpenCL
    // and ray query is [EnvVulkan]-only.

    // A RayQuery local is an alloca of target("spirv.RayQueryKHR")
    // (→ OpVariable Function of OpTypeRayQueryKHR).
    llvm::Type* rayQueryType(llvm::Module& m) override {
        return vkRayQueryType(m.getContext());
    }

    // Tier: native SPV_KHR_cooperative_matrix only for the dtype configs RADV (and
    // the conformant Vulkan coop-matrix set) actually advertise — f16 and 8-bit
    // integer A/B operands. bfloat16 is deliberately Software: no Vulkan driver
    // exposes a bf16 cooperative-matrix config (it needs SPV_INTEL_bfloat16_
    // arithmetic, an Intel-CPU-only path), so a bf16 tile takes the portable
    // flat-matmul fallback here — and lights up native WMMA on the AMD backend,
    // which has a bf16 matrix-core path (llvm.amdgcn.wmma.*.bf16). Without the
    // fork toolchain the ops can't be emitted at all, so everything is Software.
    CoopMatrixTier coopMatrixTier(llvm::Type* elem, uint32_t /*rows*/,
                                  uint32_t /*cols*/, uint32_t /*use*/) override {
#if CAJETA_HAS_SPV_COOP_MATRIX
        // bf16 has no Vulkan cooperative-matrix config — Software here (and its
        // accumulator must be bf16 too, so the whole GEMM stays one tier).
        if (elem->isBFloatTy()) return CoopMatrixTier::Software;
        // f16/int8 multiplicands and their f32/i32 accumulators are the
        // advertised native set; an integer accumulator is any width.
        if (elem->isHalfTy() || elem->isFloatTy() || elem->isIntegerTy())
            return CoopMatrixTier::Native;
#endif
        return CoopMatrixTier::Software;
    }

    // A CooperativeMatrix local is an alloca of the opaque tile type. Like the
    // ray-query types, OpTypeCooperativeMatrixKHR lowers via the BuiltinType
    // machinery on stock LLVM too, so the TYPE builder is unguarded; only the
    // OPS need the fork intrinsics.
    llvm::Type* coopMatrixType(llvm::Module& m, llvm::Type* elem, uint32_t rows,
                               uint32_t cols, uint32_t use) override {
        return vkCoopMatrixType(m.getContext(), elem, rows, cols, use);
    }

#if CAJETA_HAS_SPV_RAY_QUERY
    // OpRayQueryInitializeKHR rq, as, rayFlags, cullMask, origin, tMin, dir, tMax.
    // `as` is overloaded on the intrinsic (the AS handle type); origin/direction
    // are <3 x float>.
    void rayQueryInitialize(llvm::IRBuilderBase& b, llvm::Module& m,
                            llvm::Value* rqPtr, llvm::Value* asHandle,
                            llvm::Value* rayFlags, llvm::Value* cullMask,
                            llvm::Value* origin, llvm::Value* tMin,
                            llvm::Value* direction, llvm::Value* tMax) override {
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::spv_ray_query_initialize, {asHandle->getType()});
        b.CreateCall(f, {rqPtr, asHandle, rayFlags, cullMask, origin, tMin,
                         direction, tMax});
    }

    // OpRayQueryProceedKHR — returns i1.
    llvm::Value* rayQueryProceed(llvm::IRBuilderBase& b, llvm::Module& m,
                                 llvm::Value* rqPtr) override {
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::spv_ray_query_proceed);
        return b.CreateCall(f, {rqPtr}, "rq.proceed");
    }

    // OpRayQueryGetIntersectionTypeKHR rq, intersection — returns i32.
    llvm::Value* rayQueryIntersectionType(llvm::IRBuilderBase& b, llvm::Module& m,
                                          llvm::Value* rqPtr,
                                          llvm::Value* intersection) override {
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::spv_ray_query_get_intersection_type);
        return b.CreateCall(f, {rqPtr, intersection}, "rq.type");
    }
    // OpRayQueryGetIntersectionPrimitiveIndexKHR rq, intersection — returns i32.
    llvm::Value* rayQueryIntersectionPrimitiveIndex(
            llvm::IRBuilderBase& b, llvm::Module& m, llvm::Value* rqPtr,
            llvm::Value* intersection) override {
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::spv_ray_query_get_intersection_primitive_index);
        return b.CreateCall(f, {rqPtr, intersection}, "rq.primidx");
    }
#else
    // Stock-LLVM build: the ray-query ops need the fork intrinsics. Unnamed params
    // (no unused-arg warnings); each throws the clean fork-toolchain diagnostic.
    void rayQueryInitialize(llvm::IRBuilderBase&, llvm::Module&, llvm::Value*,
                            llvm::Value*, llvm::Value*, llvm::Value*, llvm::Value*,
                            llvm::Value*, llvm::Value*, llvm::Value*) override {
        rayQueryNoForkToolchain();
    }
    llvm::Value* rayQueryProceed(llvm::IRBuilderBase&, llvm::Module&,
                                 llvm::Value*) override {
        rayQueryNoForkToolchain();
    }
    llvm::Value* rayQueryIntersectionType(llvm::IRBuilderBase&, llvm::Module&,
                                          llvm::Value*, llvm::Value*) override {
        rayQueryNoForkToolchain();
    }
    llvm::Value* rayQueryIntersectionPrimitiveIndex(llvm::IRBuilderBase&,
            llvm::Module&, llvm::Value*, llvm::Value*) override {
        rayQueryNoForkToolchain();
    }
#endif

#if CAJETA_HAS_SPV_COOP_MATRIX
    // ptr may be a StorageBuffer (global Buffer<T>) OR a Workgroup (Shared<T>, LDS)
    // pointer — both are valid OpCooperativeMatrixLoad/StoreKHR sources. The
    // Workgroup-source (LDS-staged) path relies on two fork SPIR-V backend fixes:
    // (1) SPIRVEmitIntrinsics keeps undef non-constant array globals correctly
    // typed (so the staging copy's dynamic index survives), and (2) the
    // cooperative-matrix selection access-chains an aggregate pointer to its first
    // element (so a constant-offset Workgroup tile pointer is a scalar pointer, as
    // the op requires). See cajeta-llvm/UPSTREAM-PRS.md.
    //
    // result = OpCooperativeMatrixLoadKHR ptr layout stride. The intrinsic is
    // overloaded on (result matrix type, pointer type), in signature order.
    llvm::Value* coopMatrixLoad(llvm::IRBuilderBase& b, llvm::Module& m,
                                llvm::Value* ptr, llvm::Value* layout,
                                llvm::Value* stride, llvm::Type* matrixType,
                                uint32_t /*rows*/, uint32_t /*cols*/,
                                uint32_t /*use*/) override {
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::spv_cooperative_matrix_load,
            {matrixType, ptr->getType()});
        return b.CreateCall(f, {ptr, layout, stride}, "cm.load");
    }
    // OpCooperativeMatrixStoreKHR ptr matrix layout stride (void; overloaded on
    // pointer type then matrix type).
    void coopMatrixStore(llvm::IRBuilderBase& b, llvm::Module& m, llvm::Value* ptr,
                         llvm::Value* matrixVal, llvm::Value* layout,
                         llvm::Value* stride, uint32_t /*rows*/, uint32_t /*cols*/,
                         uint32_t /*use*/) override {
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::spv_cooperative_matrix_store,
            {ptr->getType(), matrixVal->getType()});
        b.CreateCall(f, {ptr, matrixVal, layout, stride});
    }
    // result = OpCooperativeMatrixMulAddKHR A B C (overloaded on result, A, B, C).
    llvm::Value* coopMatrixMulAdd(llvm::IRBuilderBase& b, llvm::Module& m,
                                  llvm::Value* a, llvm::Value* bMat,
                                  llvm::Value* c, llvm::Type* matrixType) override {
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::spv_cooperative_matrix_muladd,
            {matrixType, a->getType(), bMat->getType(), c->getType()});
        return b.CreateCall(f, {a, bMat, c}, "cm.mma");
    }
    // result = OpCompositeConstruct value (single-scalar splat; overloaded on
    // result matrix type then scalar value type).
    llvm::Value* coopMatrixSplat(llvm::IRBuilderBase& b, llvm::Module& m,
                                 llvm::Value* value,
                                 llvm::Type* matrixType) override {
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::spv_cooperative_matrix_splat,
            {matrixType, value->getType()});
        return b.CreateCall(f, {value}, "cm.splat");
    }
#else
    // Stock-LLVM build: the cooperative-matrix ops need the fork intrinsics.
    llvm::Value* coopMatrixLoad(llvm::IRBuilderBase&, llvm::Module&, llvm::Value*,
                                llvm::Value*, llvm::Value*, llvm::Type*) override {
        coopMatrixNoForkToolchain();
    }
    void coopMatrixStore(llvm::IRBuilderBase&, llvm::Module&, llvm::Value*,
                         llvm::Value*, llvm::Value*, llvm::Value*) override {
        coopMatrixNoForkToolchain();
    }
    llvm::Value* coopMatrixMulAdd(llvm::IRBuilderBase&, llvm::Module&, llvm::Value*,
                                  llvm::Value*, llvm::Value*, llvm::Type*) override {
        coopMatrixNoForkToolchain();
    }
    llvm::Value* coopMatrixSplat(llvm::IRBuilderBase&, llvm::Module&, llvm::Value*,
                                 llvm::Type*) override {
        coopMatrixNoForkToolchain();
    }
#endif

    // A @Device helper's Buffer<T> param is the storage-buffer HANDLE the kernel
    // holds in bufferBases (not a pointer) — so the helper takes it by value and
    // bufferElementPtr's getElementPtr path works inside the helper too. writable
    // matches the kernel's binding (materializeParam binds buffers writable=true).
    llvm::Type* bufferParamType(llvm::Module& m, llvm::Type* elemTy) override {
        return vkBufferType(m.getContext(), elemTy, /*writable=*/true);
    }

    // Descriptor-buffer handles route through resource.getpointer; shared-mem
    // globals (addrspace 3) keep the default GEP.
    llvm::Value* bufferElementPtr(llvm::IRBuilderBase& b, llvm::Module& m,
                                  llvm::Value* base, llvm::Type* elemTy,
                                  llvm::Value* index) override {
        if (auto* tet = llvm::dyn_cast<llvm::TargetExtType>(base->getType())) {
            if (tet->getName() == "spirv.VulkanBuffer")
                return getElementPtr(b, m, base, index);
        }
        return b.CreateGEP(elemTy, base, {index}, "idx");
    }

    // bufs[idx] → the idx-th descriptor of the runtime descriptor array bound at
    // `binding`. handlefrombinding(set 0, binding, range = kMaxBindlessBuffers,
    // index = nonuniformindex(idx)) yields the per-buffer spirv.VulkanBuffer
    // handle; the inner [i] then runs through bufferElementPtr (getpointer). The
    // index is wrapped in resource.nonuniformindex so the descriptor access
    // carries NonUniformEXT (the index may be per-invocation — required by the
    // spec and emitted by the fork's selectResourceNonUniformIndex).
    llvm::Value* bufferArrayElement(llvm::IRBuilderBase& b, llvm::Module& m,
                                    llvm::Function* /*fn*/, unsigned binding,
                                    llvm::Value* /*arrayBase*/,
                                    llvm::Type* elemTy,
                                    llvm::Value* descIndex) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        llvm::TargetExtType* bufTy = vkBufferType(ctx, elemTy, /*writable=*/true);
        // v1: the descriptor index is treated as DYNAMICALLY UNIFORM (e.g. a loop
        // counter, the same value across the wave) — so no NonUniformEXT
        // decoration is needed. resource.nonuniformindex (for a per-invocation
        // index) is a follow-on. The index is passed directly to
        // handlefrombinding as the descriptor-array element selector.
        llvm::Value* nameStr = b.CreateGlobalString("bufarr", "xpu.res.bufarr");
        llvm::Function* hfb = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::spv_resource_handlefrombinding, {bufTy});
        return b.CreateCall(
            hfb,
            {llvm::ConstantInt::get(i32, 0),
             llvm::ConstantInt::get(i32, binding),
             llvm::ConstantInt::get(i32, kMaxBindlessBuffers),
             descIndex, nameStr},
            "bufarr.h");
    }

    // Wave ops via the SPIR-V subgroup intrinsics (→ OpGroupNonUniform*).
    // SubgroupSize builtin (→ OpLoad of the SubgroupSize builtin) — the sibling
    // of the SubgroupLocalInvocationId read used by waveLaneId. NOT
    // spv.wave.get.lane.count, which the SPIR-V backend never wired into isel
    // ("intrinsic selection not implemented", still true through LLVM 23);
    // spv.subgroup.size IS selected (loadBuiltinInputID), so Wave.width() now
    // runs on-device on Vulkan.
    llvm::Value* waveWidth(llvm::IRBuilderBase& b, llvm::Module& m) override {
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::spv_subgroup_size);
        return b.CreateCall(f, {}, "lanecount");
    }
    llvm::Value* waveShuffle(llvm::IRBuilderBase& b, llvm::Module& m,
                             llvm::Value* value, llvm::Value* srcLane) override {
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::spv_wave_readlane,
            {llvm::Type::getInt32Ty(m.getContext())});
        return b.CreateCall(f, {value, srcLane}, "readlane");
    }
    llvm::Value* waveBallot(llvm::IRBuilderBase& b, llvm::Module& m,
                            llvm::Value* pred) override {
        // spv.subgroup.ballot (→ OpGroupNonUniformBallot) yields a <4 x i32>
        // (128-bit) mask; combine the low two lanes (covering up to 64 wave
        // lanes) into the i64 API value. (Renamed from spv.wave.ballot in LLVM 23.)
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* i64 = llvm::Type::getInt64Ty(ctx);
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::spv_subgroup_ballot);
        llvm::Value* vec = b.CreateCall(f, {pred}, "ballot");
        llvm::Value* lo = b.CreateZExt(
            b.CreateExtractElement(vec, uint64_t(0)), i64);
        llvm::Value* hi = b.CreateZExt(
            b.CreateExtractElement(vec, uint64_t(1)), i64);
        return b.CreateOr(lo, b.CreateShl(hi, llvm::ConstantInt::get(i64, 32)));
    }
    llvm::Value* waveReduceSum(llvm::IRBuilderBase& b, llvm::Module& m,
                               llvm::Value* value) override {
        // spv.wave.reduce.sum → OpGroupNonUniformIAdd with the Reduce operation.
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::spv_wave_reduce_sum,
            {llvm::Type::getInt32Ty(m.getContext())});
        return b.CreateCall(f, {value}, "wavered");
    }
    llvm::Value* waveReduce(llvm::IRBuilderBase& b, llvm::Module& m,
                            WaveReduceOp op, llvm::Value* value) override {
        // The GroupNonUniformArithmetic Reduce family (already Shader-reachable).
        llvm::Intrinsic::ID id;
        switch (op) {
            case WaveReduceOp::Max: id = llvm::Intrinsic::spv_wave_reduce_umax; break;
            case WaveReduceOp::Min: id = llvm::Intrinsic::spv_wave_reduce_umin; break;
            case WaveReduceOp::And: id = llvm::Intrinsic::spv_wave_reduce_and; break;
            case WaveReduceOp::Or:  id = llvm::Intrinsic::spv_wave_reduce_or; break;
            case WaveReduceOp::Xor: id = llvm::Intrinsic::spv_wave_reduce_xor; break;
        }
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, id, {llvm::Type::getInt32Ty(m.getContext())});
        return b.CreateCall(f, {value}, "wavered");
    }
    llvm::Value* waveScan(llvm::IRBuilderBase& b, llvm::Module& m,
                          WaveScanOp op, llvm::Value* value) override {
        // The native single-op exclusive scan: spv.wave.prefix.{sum,product} →
        // OpGroupNonUniform{IAdd,IMul} with the ExclusiveScan group operation.
        llvm::Intrinsic::ID id = op == WaveScanOp::Sum
            ? llvm::Intrinsic::spv_wave_prefix_sum
            : llvm::Intrinsic::spv_wave_prefix_product;
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, id, {llvm::Type::getInt32Ty(m.getContext())});
        return b.CreateCall(f, {value}, "wavescan");
    }
    llvm::Value* waveLaneId(llvm::IRBuilderBase& b, llvm::Module& m) override {
        // SubgroupLocalInvocationId — this invocation's index within the
        // subgroup (→ OpLoad of the builtin).
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::spv_subgroup_local_invocation_id);
        return b.CreateCall(f, {}, "laneid");
    }
    llvm::Value* waveRotate(llvm::IRBuilderBase& b, llvm::Module& m,
                            llvm::Value* value, llvm::Value* delta) override {
        // The native single-instruction rotate: spv.subgroup.rotate (the fork
        // intrinsic) → OpGroupNonUniformRotateKHR at Subgroup scope. Replaces
        // the base default's laneId+shuffle arithmetic with the hardware op.
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::spv_subgroup_rotate,
            {llvm::Type::getInt32Ty(m.getContext())});
        return b.CreateCall(f, {value, delta}, "waverotate");
    }

    // Vulkan workgroup arrays need a concrete length and can't be external
    // imports — emit a concrete internal array; emitSpirv's post-emit pass turns
    // its length into a spec constant set by the launch's sharedBytes.
    bool dynamicSharedNeedsConcreteSize() const override { return true; }

private:
    static llvm::Value* readCoord(llvm::IRBuilderBase& b, llvm::Module& m,
                                  llvm::Intrinsic::ID id, unsigned dim) {
        llvm::Type* i32 = llvm::Type::getInt32Ty(m.getContext());
        llvm::Function* f =
            llvm::Intrinsic::getOrInsertDeclaration(&m, id, {i32});
        return b.CreateCall(f, {llvm::ConstantInt::get(i32, dim)}, "coord");
    }
};

} // namespace

llvm::Function* lowerKernel(const MethodPtr& method, llvm::Module& deviceModule) {
    SpirvTarget target;
    return cajeta::xpu::lowerKernel(method, deviceModule, target);
}

} // namespace vulkan
} // namespace xpu
} // namespace cajeta
