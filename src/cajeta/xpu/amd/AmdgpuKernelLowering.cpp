//
// AMDGPU kernel lowering — see header.
//

#include "AmdgpuKernelLowering.h"

#include "../lowering/KernelLowering.h"
#include "../lowering/LoweringTarget.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llvm/IR/Module.h"

namespace cajeta {
namespace xpu {
namespace amd {

namespace {

class AmdgpuTarget : public LoweringTarget {
public:
    const char* name() const override { return "amdgpu"; }

    // AMDGPU allocas MUST live in the private address space (5). An AS-0
    // alloca here is invalid IR for the AMDGPU backend — the classic first
    // bug (cajeta-amd.md §2). mem2reg removes most of them before ISA emit;
    // any survivor is a valid private (scratch) slot.
    unsigned allocaAddressSpace() const override { return 5; }

    llvm::Value* threadId(llvm::IRBuilderBase& b, llvm::Module& m,
                          unsigned dim) override {
        static const llvm::Intrinsic::ID ids[3] = {
            llvm::Intrinsic::amdgcn_workitem_id_x,
            llvm::Intrinsic::amdgcn_workitem_id_y,
            llvm::Intrinsic::amdgcn_workitem_id_z};
        return readId(b, m, ids[dim]);
    }
    llvm::Value* workgroupId(llvm::IRBuilderBase& b, llvm::Module& m,
                             unsigned dim) override {
        static const llvm::Intrinsic::ID ids[3] = {
            llvm::Intrinsic::amdgcn_workgroup_id_x,
            llvm::Intrinsic::amdgcn_workgroup_id_y,
            llvm::Intrinsic::amdgcn_workgroup_id_z};
        return readId(b, m, ids[dim]);
    }

    // Block dim (ntid) is NOT an intrinsic on AMDGPU — it comes from the HSA
    // kernel dispatch packet (cajeta-amd.md §2). llvm.amdgcn.dispatch.ptr
    // returns a ptr addrspace(4) to that packet; workgroup_size_{x,y,z} are
    // uint16 fields at byte offsets 4/6/8 (after the 2-byte header + 2-byte
    // setup). Load the i16 and widen to i32 to match the other coordinates.
    llvm::Value* workgroupDim(llvm::IRBuilderBase& b, llvm::Module& m,
                              unsigned dim) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Function* dp = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::amdgcn_dispatch_ptr);
        llvm::Value* packet = b.CreateCall(dp, {}, "dispatch.ptr");
        llvm::Value* field = b.CreateConstGEP1_32(
            llvm::Type::getInt8Ty(ctx), packet, 4 + 2 * dim, "wgsize.ptr");
        llvm::Value* sz16 = b.CreateLoad(llvm::Type::getInt16Ty(ctx), field,
                                         "wgsize");
        return b.CreateZExt(sz16, llvm::Type::getInt32Ty(ctx), "wgsize.i32");
    }

    // Workgroup barrier with LDS-visibility ordering: a workgroup-scoped
    // release fence, the hardware s_barrier, then a workgroup-scoped acquire
    // fence — the same shape HIP's __syncthreads() lowers to, so shared-memory
    // writes before the barrier are visible to reads after it.
    void workgroupBarrier(llvm::IRBuilderBase& b, llvm::Module& m) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::SyncScope::ID wg = ctx.getOrInsertSyncScopeID("workgroup");
        b.CreateFence(llvm::AtomicOrdering::Release, wg);
        llvm::Function* bar = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::amdgcn_s_barrier);
        b.CreateCall(bar, {});
        b.CreateFence(llvm::AtomicOrdering::Acquire, wg);
    }

    // AMDGPU marks kernels purely by calling convention — no metadata
    // analogue to nvvm.annotations.
    void decorateKernel(llvm::Function* fn, llvm::Module& /*m*/) override {
        fn->setCallingConv(llvm::CallingConv::AMDGPU_KERNEL);
    }

private:
    static llvm::Value* readId(llvm::IRBuilderBase& b, llvm::Module& m,
                               llvm::Intrinsic::ID id) {
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(&m, id);
        return b.CreateCall(f, {}, "id");
    }
};

} // namespace

llvm::Function* lowerKernel(const MethodPtr& method, llvm::Module& deviceModule) {
    AmdgpuTarget target;
    return cajeta::xpu::lowerKernel(method, deviceModule, target);
}

} // namespace amd
} // namespace xpu
} // namespace cajeta
