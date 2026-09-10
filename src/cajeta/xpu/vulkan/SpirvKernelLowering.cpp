// SPIR-V (Vulkan) kernel lowering — see header.

#include "SpirvKernelLowering.h"
#include "SpirvBackend.h"
#include "SpirvInterface.h"

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

#include <iostream>
#include <string>

namespace cajeta {
namespace xpu {
namespace vulkan {

namespace {

// SPIR-V StorageBuffer pointers live in address space 11 (storage class 12);
// Function (private) allocas live in address space 0.
constexpr unsigned kStorageBufferSC = 12;
constexpr unsigned kStorageBufferAS = 11;
// Bindless descriptor-array size for a Buffer<T>[] param — handlefrombinding's
// `range` is a constant. MUST match the launch marshalling cap and the runtime.
constexpr unsigned kMaxBindlessBuffers = 16;

#ifndef CAJETA_HAS_SPV_RAY_QUERY
// A RayQuery op needs the cajeta-llvm fork's llvm.spv.ray.query.* intrinsics;
// on a stock-LLVM build it becomes this clean lowering-time diagnostic.
[[noreturn]] static void rayQueryNoForkToolchain() {
    throw cajeta::Exception(
        "XPU kernel lowering: SPV_KHR_ray_query is unavailable in this build — the "
        "Cajeta compiler was linked against an LLVM that lacks the ray-query "
        "intrinsics. Rebuild against the cajeta-llvm fork toolchain (plans/c0) to "
        "enable RayQuery / AccelerationStructure.", "XPU-N02");
}
#endif

#ifndef CAJETA_HAS_SPV_COOP_MATRIX
// As above for the SPV_KHR_cooperative_matrix operation intrinsics: the tile
// TYPE lowers on stock LLVM, only the ops need the fork.
[[noreturn]] static void coopMatrixNoForkToolchain() {
    throw cajeta::Exception(
        "XPU kernel lowering: SPV_KHR_cooperative_matrix is unavailable in this "
        "build — the Cajeta compiler was linked against an LLVM that lacks the "
        "cooperative-matrix intrinsics. Rebuild against the cajeta-llvm fork "
        "toolchain (plans/c0) to enable CooperativeMatrix.", "XPU-N03");
}
#endif

// The target("spirv.VulkanBuffer", [0 x elemTy], StorageBuffer, writable) handle
// type handlefrombinding returns for a (RW)StructuredBuffer<elemTy>.
llvm::TargetExtType* vkBufferType(llvm::LLVMContext& ctx, llvm::Type* elemTy,
                                  bool writable) {
    llvm::Type* runtimeArr = llvm::ArrayType::get(elemTy, 0);
    return llvm::TargetExtType::get(ctx, "spirv.VulkanBuffer", {runtimeArr},
                                    {kStorageBufferSC, writable ? 1u : 0u});
}

// The target("spirv.Image") handle type for a SAMPLED texture (Depth=2, MS=0,
// Sampled=1, Format=Unknown). `dimOperand` is the SPIR-V Dim (0=1D, 1=2D, 2=3D,
// 3=Cube) and `arrayed` its Arrayed operand; only those and coord arity differ.
llvm::TargetExtType* vkImageType(llvm::LLVMContext& ctx, llvm::Type* texelTy,
                                 unsigned dimOperand = 1, unsigned arrayed = 0) {
    return llvm::TargetExtType::get(ctx, "spirv.Image", {texelTy},
                                    {dimOperand, 2, arrayed, 0, 1, 0});
}

// The same image type with Sampled=2 — a writable STORAGE image, no sampler.
// Format is R32f, not Unknown: Unknown needs StorageImageWriteWithoutFormat,
// which the backend offers only for SPIR-V >= 1.6, and cajeta's triple pins none.
llvm::TargetExtType* vkStorageImageType(llvm::LLVMContext& ctx,
                                        llvm::Type* texelTy) {
    return llvm::TargetExtType::get(ctx, "spirv.Image", {texelTy},
                                    {1, 2, 0, 0, 2, 3});
}

// The target("spirv.Sampler") handle type for a Sampler descriptor.
llvm::TargetExtType* vkSamplerType(llvm::LLVMContext& ctx) {
    return llvm::TargetExtType::get(ctx, "spirv.Sampler", {}, {});
}

// The target("spirv.AccelerationStructureKHR") handle type: bound as a
// UniformConstant descriptor whose loaded handle feeds OpRayQueryInitializeKHR.
llvm::TargetExtType* vkAccelStructType(llvm::LLVMContext& ctx) {
    return llvm::TargetExtType::get(ctx, "spirv.AccelerationStructureKHR", {}, {});
}

// The function-local target("spirv.RayQueryKHR") opaque, one alloca per local.
llvm::TargetExtType* vkRayQueryType(llvm::LLVMContext& ctx) {
    return llvm::TargetExtType::get(ctx, "spirv.RayQueryKHR", {}, {});
}

// The device cooperative-matrix tile type. Scope is always Subgroup (3): the
// tile is held cooperatively across the wavefront.
llvm::TargetExtType* vkCoopMatrixType(llvm::LLVMContext& ctx, llvm::Type* elem,
                                      uint32_t rows, uint32_t cols,
                                      uint32_t use) {
    constexpr unsigned kSubgroupScope = 3;
    return llvm::TargetExtType::get(ctx, "spirv.CooperativeMatrixKHR", {elem},
                                    {kSubgroupScope, rows, cols, use});
}

// llvm.spv.resource.getpointer(handle, i32 index) -> ptr addrspace(11); `index`
// arrives i64-widened from the shared lowerer, and SPIR-V wants i32.
llvm::Value* getElementPtr(llvm::IRBuilderBase& b, llvm::Module& m,
                           llvm::Value* handle, llvm::Value* index) {
    llvm::LLVMContext& ctx = m.getContext();
    llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
    llvm::PointerType* sbPtr = llvm::PointerType::get(ctx, kStorageBufferAS);
    llvm::Value* i32idx = b.CreateTrunc(index, i32, "bidx");
    // LLVM 23 overloads the index operand, so the intrinsic takes three overload
    // types {result ptr, handle, index}; two ran the decoder off the type array.
    llvm::Function* gp = llvm::Intrinsic::getOrInsertDeclaration(
        &m, llvm::Intrinsic::spv_resource_getpointer,
        {sbPtr, handle->getType(), i32});
    return b.CreateCall(gp, {handle, i32idx}, "elem.ptr");
}

// llvm.spv.resource.handlefrombinding for a descriptor at set 0, `binding`.
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
    // Vulkan kernel params arrive as descriptors (a no-param void main()), so a
    // bindless buffer array binds per access via handlefrombinding, not fn->getArg.
    bool descriptorBoundParams() const override { return true; }

    unsigned allocaAddressSpace() const override { return 0; }

    llvm::Value* threadId(llvm::IRBuilderBase& b, llvm::Module& m,
                          unsigned dim) override {
        return readCoord(b, m, llvm::Intrinsic::spv_thread_id_in_group, dim);
    }
    llvm::Value* workgroupId(llvm::IRBuilderBase& b, llvm::Module& m,
                             unsigned dim) override {
        return readCoord(b, m, llvm::Intrinsic::spv_group_id, dim);
    }
    // The WorkgroupSize BuiltIn must decorate a CONSTANT in a Vulkan shader — as a
    // builtin variable spirv-val rejects the module — so return the baked LocalSize
    // constant createKernel put in numthreads.
    llvm::Value* workgroupDim(llvm::IRBuilderBase& /*b*/, llvm::Module& m,
                              unsigned dim) override {
        return llvm::ConstantInt::get(llvm::Type::getInt32Ty(m.getContext()),
                                      dim == 0 ? kVulkanLocalSizeX : 1);
    }
    llvm::Value* globalId(llvm::IRBuilderBase& b, llvm::Module& m,
                          unsigned dim) override {
        return readCoord(b, m, llvm::Intrinsic::spv_thread_id, dim);
    }
    // Grid-stride stride = NumWorkgroups x the baked LocalSize constant. GlobalSize
    // would require the OpenCL Kernel capability, so it is deliberately unused.
    llvm::Value* gridSize(llvm::IRBuilderBase& b, llvm::Module& m,
                          unsigned dim) override {
        return b.CreateMul(
            readCoord(b, m, llvm::Intrinsic::spv_num_workgroups, dim),
            workgroupDim(b, m, dim), "gridsize");
    }

    void workgroupBarrier(llvm::IRBuilderBase& b, llvm::Module& m) override {
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::spv_group_memory_barrier_with_group_sync);
        b.CreateCall(f, {});
    }

    void memoryFence(llvm::IRBuilderBase& b, llvm::Module& m, FenceScope scope,
                     MemoryOrder /*order*/ = MemoryOrder::Default) override {
        // Memory-only OpMemoryBarrier; the post-emit pass pins it to AcquireRelease.
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, scope == FenceScope::Workgroup
                    ? llvm::Intrinsic::spv_group_memory_barrier
                    : llvm::Intrinsic::spv_device_memory_barrier);
        b.CreateCall(f, {});
    }

    // Specialization constant: LLVM 23 has no spec-constant intrinsic, so emit a
    // uniquely-named Private global seeded with the default and VOLATILE-load it —
    // the post-emit pass rewrites it to an OpSpecConstant, and volatile stops folding.
    llvm::Value* specConstantI32(llvm::IRBuilderBase& b, llvm::Module& m,
                                 unsigned slot, int32_t defaultValue) override {
        constexpr unsigned kPrivateAS = 10;   // addressSpaceToStorageClass → Private
        llvm::Type* i32 = llvm::Type::getInt32Ty(m.getContext());
        unsigned specId = kFirstUserSpecId + slot;
        std::string gname = "cajeta_spec_" + std::to_string(specId);
        llvm::GlobalVariable* gv = m.getGlobalVariable(gname,
                                                       /*AllowInternal=*/true);
        if (!gv) {
            gv = new llvm::GlobalVariable(
                m, i32, /*isConstant=*/false,
                llvm::GlobalValue::InternalLinkage,
                llvm::ConstantInt::get(i32, (uint64_t) (int64_t) defaultValue,
                                       /*isSigned=*/true),
                gname, /*InsertBefore=*/nullptr,
                llvm::GlobalValue::NotThreadLocal, kPrivateAS);
            gv->setAlignment(llvm::MaybeAlign(4));
        }
        llvm::LoadInst* ld = b.CreateLoad(i32, gv, gname + ".v");
        ld->setVolatile(true);
        return ld;
    }

    llvm::Value* specConstantF32(llvm::IRBuilderBase& b, llvm::Module& m,
                                 unsigned slot, float defaultValue) override {
        constexpr unsigned kPrivateAS = 10;   // addressSpaceToStorageClass → Private
        llvm::Type* f32 = llvm::Type::getFloatTy(m.getContext());
        unsigned specId = kFirstUserSpecId + slot;
        std::string gname = "cajeta_spec_" + std::to_string(specId);
        llvm::GlobalVariable* gv = m.getGlobalVariable(gname,
                                                       /*AllowInternal=*/true);
        if (!gv) {
            gv = new llvm::GlobalVariable(
                m, f32, /*isConstant=*/false,
                llvm::GlobalValue::InternalLinkage,
                llvm::ConstantFP::get(f32, (double) defaultValue),
                gname, /*InsertBefore=*/nullptr,
                llvm::GlobalValue::NotThreadLocal, kPrivateAS);
            gv->setAlignment(llvm::MaybeAlign(4));
        }
        llvm::LoadInst* ld = b.CreateLoad(f32, gv, gname + ".v");
        ld->setVolatile(true);
        return ld;
    }

    void decorateKernel(llvm::Function* /*fn*/, llvm::Module& /*m*/) override {}

    // Cross-lane subgroup ops behave as written only under maximal reconvergence:
    // this fn-attr becomes OpExecutionMode MaximallyReconvergesKHR, wave kernels only.
    void onSubgroupOpsUsed(llvm::Function* fn, llvm::Module& /*m*/) override {
        fn->addFnAttr("enable-maximal-reconvergence", "true");
    }

    // The Vulkan compute entry takes NO parameters — `void main()` with the HLSL
    // compute markers, LocalSize baked in; args arrive via descriptors.
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

    // Each kernel arg becomes a descriptor binding at (set 0, binding = idx): a
    // Buffer<T> yields the handle kept in bufferBases, a scalar is read from a
    // single-element storage buffer and returned by value.
    llvm::Value* materializeParam(llvm::IRBuilderBase& b, llvm::Module& m,
                                  llvm::Function* /*fn*/, unsigned idx,
                                  const KernelParam& p) override {
        if (p.isTexture) {
            // textureDim kind -> (Dim, Arrayed): 1D=(0,0), 2D=(1,0), 3D=(2,0), array=(1,1).
            unsigned dimOp = p.textureDim == 3 ? 2u
                           : (p.textureDim == 1 ? 0u
                           : (p.textureDim == 5 ? 3u : 1u));
            unsigned arrayed = (p.textureDim == 4) ? 1u : 0u;
            return bindResource(b, m,
                                vkImageType(m.getContext(), p.type, dimOp, arrayed),
                                idx, p.name);
        }
        if (p.isImage) {
            return bindResource(b, m, vkStorageImageType(m.getContext(), p.type),
                                idx, p.name);
        }
        if (p.isSampler) {
            return bindResource(b, m, vkSamplerType(m.getContext()), idx, p.name);
        }
        if (p.isAccelStruct) {
            return bindResource(b, m, vkAccelStructType(m.getContext()), idx,
                                p.name);
        }
        if (p.isBuffer) {
            return bindResource(b, m, vkBufferType(m.getContext(), p.type, true),
                                idx, p.name);
        }
        llvm::Value* handle = bindResource(
            b, m, vkBufferType(m.getContext(), p.type, false), idx, p.name);
        llvm::Value* ptr = getElementPtr(
            b, m, handle,
            llvm::ConstantInt::get(llvm::Type::getInt64Ty(m.getContext()), 0));
        return b.CreateLoad(p.type, ptr, p.name);
    }

    // Texture2D.sample -> OpImageSampleExplicitLod via llvm.spv.resource.samplelevel:
    // coord <u,v>, explicit LOD (compute has no derivatives), no offset.
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
        llvm::Value* offset = llvm::ConstantAggregateZero::get(v2i);
        llvm::Value* rgba = b.CreateIntrinsic(
            v4f, llvm::Intrinsic::spv_resource_samplelevel,
            {texHandle, samplerHandle, coord, lod, offset});
        return rgba;
    }

    // Texture2D.fetch -> OpImageFetch via llvm.spv.resource.load.level: texHandle is
    // a SAMPLED image, and the Lod operand OpImageFetch requires comes from load.level.
    llvm::Value* fetchTexture(llvm::IRBuilderBase& b, llvm::Module& m,
                              llvm::Value* texHandle, llvm::Value* x,
                              llvm::Value* y, llvm::Type* texelTy,
                              llvm::Value* lod) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        auto* v2i = llvm::FixedVectorType::get(i32, 2);
        auto* v4t = llvm::FixedVectorType::get(texelTy, 4);
        llvm::Value* coord = llvm::PoisonValue::get(v2i);
        coord = b.CreateInsertElement(coord, x, uint64_t(0));
        coord = b.CreateInsertElement(coord, y, uint64_t(1), "tex.fetch.coord");
        return b.CreateIntrinsic(v4t, llvm::Intrinsic::spv_resource_load_level,
                                 {texHandle, coord, lod}, nullptr, "tex.fetch");
    }

    // Texture3D.sample -> OpImageSampleExplicitLod on a 3-D image: coord <u,v,w>,
    // explicit LOD 0, and a <3 x i32> offset.
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

    // Texture3D.fetch -> OpImageFetch at an integer voxel, mip 0. Result <4 x T>.
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

    // Texture1D.sample -> OpImageSampleExplicitLod on a 1-D image: the coord is a
    // SCALAR float and the offset a scalar i32, not vectors.
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

    // Texture1D.fetch -> OpImageFetch at a SCALAR i32 texel, mip 0. Result <4 x T>.
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

    // Texture2DArray.sample -> OpImageSampleExplicitLod on an Arrayed image: the
    // coord is <u, v, layer>, the layer un-normalized and converted to float.
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

    // Texture2DArray.fetch -> OpImageFetch, coord <3 x i32> {x, y, layer}.
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

    // TextureCube.sample -> OpImageSampleExplicitLod on a cube image: the coord is a
    // DIRECTION the hardware projects, and the zero offset is elided (illegal here).
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

    // Image2D.store -> OpImageWrite via llvm.spv.resource.store.2d. OpImageWrite
    // needs a 4-component texel, so a scalar value is splatted into lane 0.
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
        b.CreateIntrinsic(llvm::Intrinsic::spv_resource_store_2d,
                          {imgHandle->getType(), v4f},
                          {imgHandle, coord, texel});
    }

    // Image2D.load -> OpImageRead via llvm.spv.resource.load.2d; the result is asked
    // for as a scalar f32, so the backend extracts component 0 of the texel.
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
        return b.CreateIntrinsic(llvm::Intrinsic::spv_resource_load_2d,
                                 {f32, imgHandle->getType()},
                                 {imgHandle, coord}, nullptr, "img.load");
    }

    // --- integer dot product (SPV_KHR_integer_dot_product, DP4a) --------------
    // Pack four int8 lanes into an i32 — lane 0 is the LOW byte, matching
    // PackedVectorFormat4x8Bit — then emit llvm.spv.dot4add.{i8,u8}packed.
    llvm::Value* integerDot4x8(llvm::IRBuilderBase& b, llvm::Module& m,
                               llvm::Value* a, llvm::Value* c, llvm::Value* acc,
                               bool aSigned, bool cSigned) override {
        // MIXED signedness maps to OpSUDot, whose FIRST operand packs the signed bytes
        // and second the unsigned. Stock LLVM has only the symmetric pair, so the fork
        // adds spv_dot4add_su8packed; without it this falls back to a scalar widen.
        llvm::Type* i32 = llvm::Type::getInt32Ty(m.getContext());
        llvm::Value* x = b.CreateBitCast(a, i32, "dp4a.x");
        llvm::Value* y = b.CreateBitCast(c, i32, "dp4a.y");
        if (aSigned != cSigned) {
            // OpSUDot wants (signed, unsigned); integer dot is order-symmetric in value.
            llvm::Value* xs = aSigned ? x : y;
            llvm::Value* yu = aSigned ? y : x;
            llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
                &m, llvm::Intrinsic::spv_dot4add_su8packed);
            return b.CreateCall(f, {acc, xs, yu}, "dp4a.su");
        }
        llvm::Intrinsic::ID id = aSigned
            ? llvm::Intrinsic::spv_dot4add_i8packed
            : llvm::Intrinsic::spv_dot4add_u8packed;
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(&m, id);
        return b.CreateCall(f, {acc, x, y}, "dp4a");
    }

    // Atomic memory scope from the pointer's storage class: Workgroup for `shared`
    // (addrspace 3), Device for a StorageBuffer. Vulkan rejects CrossDevice.
    llvm::SyncScope::ID atomicScope(llvm::Module& m, llvm::Value* ptr) {
        constexpr unsigned kSharedAS = 3;   // matches lowerSharedDecl's addrspace
        const char* name =
            ptr->getType()->getPointerAddressSpace() == kSharedAS ? "workgroup"
                                                                  : "device";
        return m.getContext().getOrInsertSyncScopeID(name);
    }

    // Vulkan clamp for a requested order: a device-scope atomic with Monotonic or
    // SequentiallyConsistent semantics fails spirv-val, so both raise to AcquireRelease.
    static llvm::AtomicOrdering vkClamp(llvm::AtomicOrdering o) {
        return (o == llvm::AtomicOrdering::Monotonic ||
                o == llvm::AtomicOrdering::SequentiallyConsistent)
                   ? llvm::AtomicOrdering::AcquireRelease : o;
    }

    // --- float atomics (SPV_EXT_shader_atomic_float_add / _min_max) -----------
    // AcquireRelease plus the storage-matched scope; FMin/FMax additionally need
    // VK_EXT_shader_atomic_float2 (absent on NVIDIA, and no portable fallback).
    llvm::Value* atomicFloatRMW(llvm::IRBuilderBase& b, llvm::Module& m,
                                AtomicFloatOp op, llvm::Value* ptr,
                                llvm::Value* value,
                                MemoryOrder order = MemoryOrder::Default) override {
        llvm::AtomicRMWInst::BinOp binop =
            op == AtomicFloatOp::Add ? llvm::AtomicRMWInst::FAdd
          : op == AtomicFloatOp::Min ? llvm::AtomicRMWInst::FMin
                                     : llvm::AtomicRMWInst::FMax;
        return b.CreateAtomicRMW(
            binop, ptr, value, llvm::MaybeAlign(),
            vkClamp(toAtomicOrdering(order,
                                          llvm::AtomicOrdering::AcquireRelease)),
            atomicScope(m, ptr));
    }

    // --- integer atomics (core SPIR-V): same memory-model constraint as floats --
    llvm::Value* atomicIntRMW(llvm::IRBuilderBase& b, llvm::Module& m,
                              AtomicIntOp op, llvm::Value* ptr,
                              llvm::Value* value, bool isSigned,
                              MemoryOrder order = MemoryOrder::Default) override {
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
        return b.CreateAtomicRMW(
            binop, ptr, value, llvm::MaybeAlign(),
            vkClamp(toAtomicOrdering(order,
                                        llvm::AtomicOrdering::AcquireRelease)),
            atomicScope(m, ptr));
    }

    // Compare-exchange -> OpAtomicCompareExchange with the storage-matched scope:
    // AcquireRelease on success, Acquire on failure. Returns the OLD value.
    llvm::Value* atomicCompareExchange(llvm::IRBuilderBase& b, llvm::Module& m,
                                       llvm::Value* ptr, llvm::Value* expected,
                                       llvm::Value* desired,
                                       MemoryOrder order = MemoryOrder::Default)
                                       override {
        llvm::AtomicOrdering success = vkClamp(
            toAtomicOrdering(order, llvm::AtomicOrdering::AcquireRelease));
        llvm::Value* pair = b.CreateAtomicCmpXchg(
            ptr, expected, desired, llvm::MaybeAlign(),
            success, casFailureOrdering(success), atomicScope(m, ptr));
        return b.CreateExtractValue(pair, 0, "atomic.cas.old");
    }

    // --- shader clock: OpReadClockKHR at Subgroup scope (SPV_KHR_shader_clock) --
    llvm::Value* readClock(llvm::IRBuilderBase& b, llvm::Module& m) override {
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::spv_read_clock);
        return b.CreateCall(f, {b.getInt32(3)}, "clock");
    }

    // --- ray query: lowers to the fork's llvm.spv.ray.query.* intrinsics -------

    // A RayQuery local is an alloca of target("spirv.RayQueryKHR").
    llvm::Type* rayQueryType(llvm::Module& m) override {
        return vkRayQueryType(m.getContext());
    }

    // Native SPV_KHR_cooperative_matrix only for the dtype configs drivers advertise
    // (f16 and 8-bit integer operands); bf16 has none and takes the portable matmul.
    ImplTier coopMatrixTier(llvm::Type* elem, uint32_t /*rows*/,
                            uint32_t /*cols*/, uint32_t /*use*/) override {
#if CAJETA_HAS_SPV_COOP_MATRIX
        if (elem->isBFloatTy()) return ImplTier::Portable;
        if (elem->isHalfTy() || elem->isFloatTy() || elem->isIntegerTy())
            return ImplTier::Native;
#endif
        return ImplTier::Portable;
    }

    // A CooperativeMatrix local is an alloca of the opaque tile type.
    llvm::Type* coopMatrixType(llvm::Module& m, llvm::Type* elem, uint32_t rows,
                               uint32_t cols, uint32_t use) override {
        return vkCoopMatrixType(m.getContext(), elem, rows, cols, use);
    }

#if CAJETA_HAS_SPV_RAY_QUERY
    // OpRayQueryInitializeKHR rq, as, rayFlags, cullMask, origin, tMin, dir, tMax;
    // `as` is overloaded on the intrinsic and origin/direction are <3 x float>.
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
    // OpRayQueryGetIntersectionTKHR rq, intersection — returns f32 (inc 3b).
    llvm::Value* rayQueryIntersectionT(llvm::IRBuilderBase& b, llvm::Module& m,
                                       llvm::Value* rqPtr,
                                       llvm::Value* intersection) override {
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::spv_ray_query_get_intersection_t);
        return b.CreateCall(f, {rqPtr, intersection}, "rq.t");
    }
    // OpRayQueryGetIntersectionBarycentricsKHR rq, intersection — returns <2xf32>.
    llvm::Value* rayQueryIntersectionBarycentrics(
            llvm::IRBuilderBase& b, llvm::Module& m, llvm::Value* rqPtr,
            llvm::Value* intersection) override {
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::spv_ray_query_get_intersection_barycentrics);
        return b.CreateCall(f, {rqPtr, intersection}, "rq.bary");
    }
    // OpRayQueryGetIntersectionFrontFaceKHR rq, intersection — returns i1.
    llvm::Value* rayQueryIntersectionFrontFace(
            llvm::IRBuilderBase& b, llvm::Module& m, llvm::Value* rqPtr,
            llvm::Value* intersection) override {
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::spv_ray_query_get_intersection_front_face);
        return b.CreateCall(f, {rqPtr, intersection}, "rq.front");
    }
    // OpRayQueryConfirmIntersectionKHR rq — void.
    void rayQueryConfirmIntersection(llvm::IRBuilderBase& b, llvm::Module& m,
                                     llvm::Value* rqPtr) override {
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::spv_ray_query_confirm_intersection);
        b.CreateCall(f, {rqPtr});
    }
    // OpRayQueryGenerateIntersectionKHR rq, tHit — void.
    void rayQueryGenerateIntersection(llvm::IRBuilderBase& b, llvm::Module& m,
                                      llvm::Value* rqPtr,
                                      llvm::Value* tHit) override {
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::spv_ray_query_generate_intersection);
        b.CreateCall(f, {rqPtr, tHit});
    }
#else
    // Stock-LLVM build: each ray-query op throws the fork-toolchain diagnostic.
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
    llvm::Value* rayQueryIntersectionT(llvm::IRBuilderBase&, llvm::Module&,
                                       llvm::Value*, llvm::Value*) override {
        rayQueryNoForkToolchain();
    }
    llvm::Value* rayQueryIntersectionBarycentrics(llvm::IRBuilderBase&,
            llvm::Module&, llvm::Value*, llvm::Value*) override {
        rayQueryNoForkToolchain();
    }
    llvm::Value* rayQueryIntersectionFrontFace(llvm::IRBuilderBase&, llvm::Module&,
                                               llvm::Value*, llvm::Value*) override {
        rayQueryNoForkToolchain();
    }
    void rayQueryConfirmIntersection(llvm::IRBuilderBase&, llvm::Module&,
                                     llvm::Value*) override {
        rayQueryNoForkToolchain();
    }
    void rayQueryGenerateIntersection(llvm::IRBuilderBase&, llvm::Module&,
                                      llvm::Value*, llvm::Value*) override {
        rayQueryNoForkToolchain();
    }
#endif

#if CAJETA_HAS_SPV_COOP_MATRIX
    // result = OpCooperativeMatrixLoadKHR ptr layout stride, overloaded on (result
    // matrix type, pointer type). ptr may be StorageBuffer or Workgroup (LDS); the
    // Workgroup path relies on two fork SPIR-V backend fixes.
    llvm::Value* coopMatrixLoad(llvm::IRBuilderBase& b, llvm::Module& m,
                                llvm::Value* ptr, llvm::Value* layout,
                                llvm::Value* stride, llvm::Type* matrixType,
                                uint32_t /*rows*/, uint32_t /*cols*/,
                                uint32_t /*use*/, uint32_t swz = 0,
                                LdsBlockPad /*blk*/ = {}) override {
        if (swz) {
            // Degrade to identity rather than reject: the tile was staged unpermuted.
            std::cerr << "note: [swizzle-tier] CooperativeMatrix.load from a "
                         "Swizzled<T,S> tile uses the IDENTITY layout on SPIR-V "
                         "(no per-element coop-matrix swizzle); correct, unaccelerated.\n";
        }
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::spv_cooperative_matrix_load,
            {matrixType, ptr->getType()});
        return b.CreateCall(f, {ptr, layout, stride}, "cm.load");
    }
    // OpCooperativeMatrixStoreKHR ptr matrix layout stride (void).
    void coopMatrixStore(llvm::IRBuilderBase& b, llvm::Module& m, llvm::Value* ptr,
                         llvm::Value* matrixVal, llvm::Value* layout,
                         llvm::Value* stride, uint32_t /*rows*/, uint32_t /*cols*/,
                         uint32_t /*use*/, uint32_t swz = 0,
                         LdsBlockPad /*blk*/ = {}) override {
        if (swz) {
            std::cerr << "note: [swizzle-tier] CooperativeMatrix.store to a "
                         "Swizzled<T,S> tile uses the IDENTITY layout on SPIR-V "
                         "(no per-element coop-matrix swizzle); correct, unaccelerated.\n";
        }
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::spv_cooperative_matrix_store,
            {ptr->getType(), matrixVal->getType()});
        b.CreateCall(f, {ptr, matrixVal, layout, stride});
    }
    // result = OpCooperativeMatrixMulAddKHR A B C [operands]. `signFlags` is the KHR
    // Operands mask; without it integer components multiply as UNSIGNED (-1 reads 255).
    llvm::Value* coopMatrixMulAdd(llvm::IRBuilderBase& b, llvm::Module& m,
                                  llvm::Value* a, llvm::Value* bMat,
                                  llvm::Value* c, llvm::Type* matrixType,
                                  uint32_t signFlags) override {
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::spv_cooperative_matrix_muladd,
            {matrixType, a->getType(), bMat->getType(), c->getType()});
        llvm::Value* flags = llvm::ConstantInt::get(
            llvm::Type::getInt32Ty(m.getContext()), signFlags);
        return b.CreateCall(f, {a, bMat, c, flags}, "cm.mma");
    }
    // result = OpCompositeConstruct value (single-scalar splat).
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
                                  llvm::Value*, llvm::Value*, llvm::Type*,
                                  uint32_t) override {
        coopMatrixNoForkToolchain();
    }
    llvm::Value* coopMatrixSplat(llvm::IRBuilderBase&, llvm::Module&, llvm::Value*,
                                 llvm::Type*) override {
        coopMatrixNoForkToolchain();
    }
#endif

    // A @Device helper's Buffer<T> param is the storage-buffer HANDLE, taken by value.
    llvm::Type* bufferParamType(llvm::Module& m, llvm::Type* elemTy) override {
        return vkBufferType(m.getContext(), elemTy, /*writable=*/true);
    }

    // Descriptor handles route through getpointer; shared-mem globals keep the GEP.
    llvm::Value* bufferElementPtr(llvm::IRBuilderBase& b, llvm::Module& m,
                                  llvm::Value* base, llvm::Type* elemTy,
                                  llvm::Value* index) override {
        if (auto* tet = llvm::dyn_cast<llvm::TargetExtType>(base->getType())) {
            if (tet->getName() == "spirv.VulkanBuffer")
                return getElementPtr(b, m, base, index);
        }
        return b.CreateGEP(elemTy, base, {index}, "idx");
    }

    // SPIR-V uses LOGICAL addressing: there is no packed <N x T> load from a scalar
    // element pointer, so build the vector from per-lane scalar loads. Truncate the
    // index to i32 once — a 64-bit chain stops Mesa folding +j into the ds_* offset.
    static llvm::Value* laneIndexBase(llvm::IRBuilderBase& b,
                                      llvm::Value* base, llvm::Value* index) {
        bool isShared = base->getType()->isPointerTy()
            && base->getType()->getPointerAddressSpace() == 3;
        if (isShared && !index->getType()->isIntegerTy(32))
            return b.CreateTrunc(index, b.getInt32Ty(), "idx32");
        return index;
    }

    llvm::Value* vectorLoad(llvm::IRBuilderBase& b, llvm::Module& m,
                            llvm::Value* base, llvm::Type* elemTy,
                            unsigned lanes, llvm::Value* index) override {
        auto* vecTy = llvm::FixedVectorType::get(elemTy, lanes);
        llvm::Value* vec = llvm::UndefValue::get(vecTy);
        index = laneIndexBase(b, base, index);
        llvm::Type* idxTy = index->getType();
        for (unsigned j = 0; j < lanes; ++j) {
            llvm::Value* jIdx = b.CreateAdd(
                index, llvm::ConstantInt::get(idxTy, j), "", /*HasNUW=*/true,
                /*HasNSW=*/true);
            llvm::Value* ptr = bufferElementPtr(b, m, base, elemTy, jIdx);
            llvm::Value* sc = b.CreateLoad(elemTy, ptr, "vl.lane");
            vec = b.CreateInsertElement(
                vec, sc, llvm::ConstantInt::get(b.getInt32Ty(), j), "vl.ins");
        }
        return vec;
    }

    void vectorStore(llvm::IRBuilderBase& b, llvm::Module& m, llvm::Value* base,
                     llvm::Type* elemTy, unsigned lanes, llvm::Value* index,
                     llvm::Value* value) override {
        index = laneIndexBase(b, base, index);
        llvm::Type* idxTy = index->getType();
        for (unsigned j = 0; j < lanes; ++j) {
            llvm::Value* jIdx = b.CreateAdd(
                index, llvm::ConstantInt::get(idxTy, j), "", /*HasNUW=*/true,
                /*HasNSW=*/true);
            llvm::Value* ptr = bufferElementPtr(b, m, base, elemTy, jIdx);
            llvm::Value* sc = b.CreateExtractElement(
                value, llvm::ConstantInt::get(b.getInt32Ty(), j), "vs.lane");
            b.CreateStore(sc, ptr);
        }
    }

    // bufs[idx] -> the idx-th descriptor of the array bound at `binding`, via
    // handlefrombinding(set 0, binding, range = kMaxBindlessBuffers, index); the
    // inner [i] then runs through bufferElementPtr.
    llvm::Value* bufferArrayElement(llvm::IRBuilderBase& b, llvm::Module& m,
                                    llvm::Function* /*fn*/, unsigned binding,
                                    llvm::Value* /*arrayBase*/,
                                    llvm::Type* elemTy,
                                    llvm::Value* descIndex) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        llvm::TargetExtType* bufTy = vkBufferType(ctx, elemTy, /*writable=*/true);
        // v1 treats the descriptor index as DYNAMICALLY UNIFORM: no NonUniformEXT.
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

    // Wave ops via the SPIR-V subgroup intrinsics. Use spv.subgroup.size, NOT
    // spv.wave.get.lane.count, which the backend still never wired into isel.
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
        // spv.subgroup.ballot yields a <4 x i32> mask; its low two lanes make the i64.
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
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::spv_wave_reduce_sum,
            {llvm::Type::getInt32Ty(m.getContext())});
        return b.CreateCall(f, {value}, "wavered");
    }
    llvm::Value* waveReduce(llvm::IRBuilderBase& b, llvm::Module& m,
                            WaveReduceOp op, llvm::Value* value) override {
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
    llvm::Value* waveReduceF32(llvm::IRBuilderBase& b, llvm::Module& m,
                               WaveReduceFOp op, llvm::Value* value) override {
        llvm::Intrinsic::ID id = op == WaveReduceFOp::Sum
            ? llvm::Intrinsic::spv_wave_reduce_sum
            : llvm::Intrinsic::spv_wave_reduce_max;
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, id, {llvm::Type::getFloatTy(m.getContext())});
        return b.CreateCall(f, {value}, "wavered.f");
    }
    llvm::Value* waveScan(llvm::IRBuilderBase& b, llvm::Module& m,
                          WaveScanOp op, llvm::Value* value) override {
        llvm::Intrinsic::ID id = op == WaveScanOp::Sum
            ? llvm::Intrinsic::spv_wave_prefix_sum
            : llvm::Intrinsic::spv_wave_prefix_product;
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, id, {llvm::Type::getInt32Ty(m.getContext())});
        return b.CreateCall(f, {value}, "wavescan");
    }
    llvm::Value* waveLaneId(llvm::IRBuilderBase& b, llvm::Module& m) override {
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::spv_subgroup_local_invocation_id);
        return b.CreateCall(f, {}, "laneid");
    }
    llvm::Value* waveRotate(llvm::IRBuilderBase& b, llvm::Module& m,
                            llvm::Value* value, llvm::Value* delta) override {
        // Native single-instruction rotate: spv.subgroup.rotate at Subgroup scope.
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::spv_subgroup_rotate,
            {llvm::Type::getInt32Ty(m.getContext())});
        return b.CreateCall(f, {value, delta}, "waverotate");
    }

    // Quad ops -> the fork llvm.spv.quad.* intrinsics -> native quad opcodes.
    llvm::Value* quadBroadcast(llvm::IRBuilderBase& b, llvm::Module& m,
                               llvm::Value* value,
                               llvm::Value* index) override {
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::spv_quad_broadcast,
            {llvm::Type::getInt32Ty(m.getContext())});
        return b.CreateCall(f, {value, index}, "quadbcast");
    }

    llvm::Value* quadSwap(llvm::IRBuilderBase& b, llvm::Module& m,
                          llvm::Value* value, unsigned direction) override {
        llvm::Type* i32 = llvm::Type::getInt32Ty(m.getContext());
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::spv_quad_swap, {i32});
        return b.CreateCall(f, {value, llvm::ConstantInt::get(i32, direction)},
                            "quadswap");
    }

    llvm::Value* quadAll(llvm::IRBuilderBase& b, llvm::Module& m,
                         llvm::Value* pred) override {
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::spv_quad_all);
        return b.CreateCall(f, {pred}, "quadall");
    }

    llvm::Value* quadAny(llvm::IRBuilderBase& b, llvm::Module& m,
                         llvm::Value* pred) override {
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::spv_quad_any);
        return b.CreateCall(f, {pred}, "quadany");
    }

    // Vulkan workgroup arrays need a concrete length: emit an internal array whose
    // length the post-emit pass turns into a spec constant set by sharedBytes.
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

// Software ray-query variant: the AccelerationStructure is a software BVH bound
// as a plain float32 storage buffer, so the lowerer emits the portable walk.
class SpirvSoftwareTarget : public SpirvTarget {
public:
    NounImpl accelImpl() const override { return NounImpl::SoftwareBvh; }

    llvm::Value* materializeParam(llvm::IRBuilderBase& b, llvm::Module& m,
                                  llvm::Function* fn, unsigned idx,
                                  const KernelParam& p) override {
        if (p.isAccelStruct) {
            return bindResource(
                b, m,
                vkBufferType(m.getContext(),
                             llvm::Type::getFloatTy(m.getContext()), true),
                idx, p.name);
        }
        return SpirvTarget::materializeParam(b, m, fn, idx, p);
    }
};

// The graphics fork of SpirvTarget: it reuses the whole body walk and forks only
// the pipeline interface — `void main()` with the stage's shader attr, params as
// Input interface variables, and the return stored to an Output variable.
class SpirvGraphicsTarget : public SpirvTarget {
public:
    explicit SpirvGraphicsTarget(ShaderStage stage) : stage_(stage) {}

    llvm::Function* createKernel(
        llvm::Module& m, const std::string& kname,
        const std::vector<KernelParam>& params) override {
        llvm::LLVMContext& ctx = m.getContext();
        auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx),
                                             /*vararg=*/false);
        auto* fn = llvm::Function::Create(fnTy, llvm::Function::ExternalLinkage,
                                          kname, &m);
        fn->addFnAttr("hlsl.shader", hlslShaderAttr(stage_));
        buildPushConstantBlock(m, params);
        return fn;
    }

    llvm::Value* materializeParam(llvm::IRBuilderBase& b, llvm::Module& m,
                                  llvm::Function* fn, unsigned idx,
                                  const KernelParam& p) override {
        // A @PushConstant param reads its member from the stage's single push-constant
        // block; the fork's SPIRVPushConstantAccess pass rewrites the global access.
        if (p.isPushConstant && pcGlobal_ && idx < pcMemberOf_.size() &&
            pcMemberOf_[idx] >= 0) {
            llvm::Type* i32 = llvm::Type::getInt32Ty(m.getContext());
            llvm::Value* member = b.CreateGEP(
                pcStructTy_, pcGlobal_,
                {llvm::ConstantInt::get(i32, 0),
                 llvm::ConstantInt::get(i32, (uint32_t) pcMemberOf_[idx])},
                p.name + ".pc");
            return b.CreateLoad(p.type, member, p.name);
        }
        if (p.isBuffer || p.isTexture || p.isImage || p.isSampler ||
            p.isAccelStruct) {
            return SpirvTarget::materializeParam(b, m, fn, idx, p);
        }
        llvm::GlobalVariable* in = createLocationVar(
            m, p.type, InterfaceStorage::Input, nextInputLocation_++, p.name);
        return b.CreateLoad(p.type, in, p.name);
    }

    bool shaderOutputReturn() const override { return true; }

    void storeShaderOutput(llvm::IRBuilderBase& b, llvm::Module& m,
                           llvm::Function* /*fn*/, llvm::Value* value) override {
        // A @ValueType STRUCT return is one output per field, mapped positionally.
        if (auto* st = llvm::dyn_cast<llvm::StructType>(value->getType())) {
            for (unsigned i = 0; i < st->getNumElements(); ++i) {
                llvm::Value* field = b.CreateExtractValue(value, i, "out.field");
                b.CreateStore(field, shaderOutputVar(m, i, field->getType()));
            }
            return;
        }
        b.CreateStore(value, shaderOutputVar(m, 0, value->getType()));
    }

private:
    // Collect every @PushConstant param into ONE struct global in the PushConstant
    // address space (one block per stage); pcMemberOf_[i] is param i's member index.
    void buildPushConstantBlock(llvm::Module& m,
                                const std::vector<KernelParam>& params) {
        pcMemberOf_.assign(params.size(), -1);
        std::vector<llvm::Type*> members;
        for (unsigned i = 0; i < params.size(); ++i) {
            if (params[i].isPushConstant && params[i].type) {
                pcMemberOf_[i] = (int) members.size();
                members.push_back(params[i].type);
            }
        }
        if (members.empty()) return;
        pcStructTy_ = llvm::StructType::create(m.getContext(), members,
                                               "cajeta.pushconst");
        pcGlobal_ = createPushConstantBlock(m, pcStructTy_, "cajeta_pc");
    }

    // The Output interface variable for output `index`, created on first use.
    llvm::GlobalVariable* shaderOutputVar(llvm::Module& m, unsigned index,
                                          llvm::Type* ty) {
        if (outputVars_.size() <= index) outputVars_.resize(index + 1, nullptr);
        if (outputVars_[index]) return outputVars_[index];
        llvm::GlobalVariable* ov;
        if (stage_ == ShaderStage::Vertex && index == 0) {
            ov = createBuiltInVar(m, ty, InterfaceStorage::Output,
                                  SpirvBuiltIn::Position, "gl_Position");
        } else {
            unsigned loc = (stage_ == ShaderStage::Vertex) ? index - 1 : index;
            ov = createLocationVar(m, ty, InterfaceStorage::Output, loc,
                                   "out_" + std::to_string(loc));
        }
        outputVars_[index] = ov;
        return ov;
    }

    ShaderStage stage_;
    unsigned nextInputLocation_ = 0;
    std::vector<llvm::GlobalVariable*> outputVars_;
    llvm::StructType* pcStructTy_ = nullptr;
    llvm::GlobalVariable* pcGlobal_ = nullptr;
    std::vector<int> pcMemberOf_;
};

} // namespace

llvm::Function* lowerKernel(const MethodPtr& method, llvm::Module& deviceModule,
                            bool softwareRayQuery, const std::string& entryName) {
    if (softwareRayQuery) {
        SpirvSoftwareTarget target;
        return cajeta::xpu::lowerKernel(method, deviceModule, target, entryName);
    }
    SpirvTarget target;
    return cajeta::xpu::lowerKernel(method, deviceModule, target, entryName);
}

llvm::Function* lowerGraphicsShader(const MethodPtr& method,
                                    llvm::Module& deviceModule, ShaderStage stage,
                                    const std::string& entryName) {
    SpirvGraphicsTarget target(stage);
    return cajeta::xpu::lowerKernel(method, deviceModule, target, entryName);
}

} // namespace vulkan
} // namespace xpu
} // namespace cajeta
