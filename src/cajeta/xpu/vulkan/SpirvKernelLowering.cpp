//
// SPIR-V (Vulkan) kernel lowering — see header.
//

#include "SpirvKernelLowering.h"
#include "SpirvBackend.h"

#include "../lowering/KernelLowering.h"
#include "../lowering/LoweringTarget.h"

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
// in address space 0. (Probed against LLVM 22 + spirv-val, 2026-05-30.)
constexpr unsigned kStorageBufferSC = 12;
constexpr unsigned kStorageBufferAS = 11;

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
llvm::TargetExtType* vkImageType(llvm::LLVMContext& ctx, llvm::Type* texelTy) {
    return llvm::TargetExtType::get(ctx, "spirv.Image", {texelTy},
                                    {1, 2, 0, 0, 1, 0});
}

// target("spirv.Sampler") — the handle type for a Sampler descriptor (→
// OpTypeSampler). No type/int params; combined with an image at the sample site.
llvm::TargetExtType* vkSamplerType(llvm::LLVMContext& ctx) {
    return llvm::TargetExtType::get(ctx, "spirv.Sampler", {}, {});
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
    llvm::Value* workgroupDim(llvm::IRBuilderBase& b, llvm::Module& m,
                              unsigned dim) override {
        return readCoord(b, m, llvm::Intrinsic::spv_workgroup_size, dim);
    }
    // SPIR-V exposes GlobalInvocationId natively — override the computed default.
    llvm::Value* globalId(llvm::IRBuilderBase& b, llvm::Module& m,
                          unsigned dim) override {
        return readCoord(b, m, llvm::Intrinsic::spv_thread_id, dim);
    }
    // Grid-stride stride = NumWorkgroups·WorkgroupSize. Both builtins are valid
    // in the GLCompute execution model; GlobalSize would require the OpenCL
    // Kernel capability (invalid in a Vulkan shader), so it's deliberately not used.
    llvm::Value* gridSize(llvm::IRBuilderBase& b, llvm::Module& m,
                          unsigned dim) override {
        return b.CreateMul(
            readCoord(b, m, llvm::Intrinsic::spv_num_workgroups, dim),
            readCoord(b, m, llvm::Intrinsic::spv_workgroup_size, dim),
            "gridsize");
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
            return bindResource(b, m, vkImageType(m.getContext(), p.type), idx,
                                p.name);
        }
        // Sampler (Item 8): a SAMPLER descriptor. The filter/address modes live
        // in the runtime VkSampler, not the SPIR-V — here it's just a handle.
        if (p.isSampler) {
            return bindResource(b, m, vkSamplerType(m.getContext()), idx, p.name);
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
                               llvm::Value* u, llvm::Value* v) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* f32 = llvm::Type::getFloatTy(ctx);
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        auto* v2f = llvm::FixedVectorType::get(f32, 2);
        auto* v4f = llvm::FixedVectorType::get(f32, 4);
        auto* v2i = llvm::FixedVectorType::get(i32, 2);
        llvm::Value* coord = llvm::PoisonValue::get(v2f);
        coord = b.CreateInsertElement(coord, u, uint64_t(0));
        coord = b.CreateInsertElement(coord, v, uint64_t(1), "tex.coord");
        llvm::Value* lod = llvm::ConstantFP::get(f32, 0.0);
        llvm::Value* offset = llvm::ConstantAggregateZero::get(v2i);
        // CreateIntrinsic infers the (result, image, sampler, coord, offset)
        // overloads from the operand types, exactly as clang's HLSL SampleLevel.
        llvm::Value* rgba = b.CreateIntrinsic(
            v4f, llvm::Intrinsic::spv_resource_samplelevel,
            {texHandle, samplerHandle, coord, lod, offset});
        return b.CreateExtractElement(rgba, uint64_t(0), "tex.sample");
    }

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

    // Wave ops via the SPIR-V subgroup intrinsics (→ OpGroupNonUniform*).
    llvm::Value* waveWidth(llvm::IRBuilderBase& b, llvm::Module& m) override {
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::spv_wave_get_lane_count);
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
    llvm::Value* waveLaneId(llvm::IRBuilderBase& b, llvm::Module& m) override {
        // SubgroupLocalInvocationId — this invocation's index within the
        // subgroup (→ OpLoad of the builtin).
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::spv_subgroup_local_invocation_id);
        return b.CreateCall(f, {}, "laneid");
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
