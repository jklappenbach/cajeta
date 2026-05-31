//
// CPU kernel lowering — see header. The host LoweringTarget + thin wrapper.
//

#include "CpuKernelLowering.h"

#include "../lowering/KernelLowering.h"
#include "../lowering/LoweringTarget.h"
#include "../../error/Exception.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"

namespace cajeta {
namespace xpu {
namespace cpu {

namespace {

// The CPU LoweringTarget. The SIMT backends read coordinates from hardware
// intrinsics; the CPU has none, so the grid→threads model passes a work-item's
// coordinates as the 9 trailing i32 kernel args and these reads pull them back.
class CpuTarget : public LoweringTarget {
public:
    const char* name() const override { return "cpu"; }

    // Flat host address space.
    unsigned allocaAddressSpace() const override { return 0; }

    llvm::Value* threadId(llvm::IRBuilderBase& b, llvm::Module&,
                          unsigned dim) override {
        return coord(b, /*group=*/0, dim);     // tid.{x,y,z}
    }
    llvm::Value* workgroupId(llvm::IRBuilderBase& b, llvm::Module&,
                             unsigned dim) override {
        return coord(b, /*group=*/3, dim);     // ctaid.{x,y,z}
    }
    llvm::Value* workgroupDim(llvm::IRBuilderBase& b, llvm::Module&,
                              unsigned dim) override {
        return coord(b, /*group=*/6, dim);     // ntid.{x,y,z}
    }
    // globalId uses the shared default: ctaid*ntid + tid.

    void workgroupBarrier(llvm::IRBuilderBase&, llvm::Module&) override {
        // A CPU workgroup barrier needs work-item loop fission (split the
        // kernel at the barrier and loop each region over the block) — a later
        // increment (cajeta-cpu.md Inc 6). Barrier-free kernels first.
        throw cajeta::Exception(
            "XPU kernel lowering: unsupported construct — workgroup barrier on "
            "the CPU backend (work-item fission not yet implemented)",
            "XPU-N01");
    }

    void decorateKernel(llvm::Function*, llvm::Module&) override {
        // Default C calling convention + external linkage (set at creation) is
        // exactly what the host driver / JIT looks up. Nothing to mark.
    }

    // Buffers as flat addrspace(0) pointers + scalars by value, then the 9
    // trailing i32 coordinate params. (Overrides the addrspace(1) default.)
    llvm::Function* createKernel(
        llvm::Module& m, const std::string& name,
        const std::vector<KernelParam>& params) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        std::vector<llvm::Type*> tys;
        tys.reserve(params.size() + kNumCoordParams);
        for (auto& p : params) {
            tys.push_back(p.isBuffer
                              ? (llvm::Type*) llvm::PointerType::get(ctx, 0)
                              : p.type);
        }
        for (unsigned i = 0; i < kNumCoordParams; ++i) tys.push_back(i32);

        auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), tys,
                                             /*vararg=*/false);
        auto* fn = llvm::Function::Create(fnTy, llvm::Function::ExternalLinkage,
                                          name, &m);
        unsigned i = 0;
        for (auto& p : params) fn->getArg(i++)->setName(p.name);
        static const char* kCoordNames[kNumCoordParams] = {
            "tid.x", "tid.y", "tid.z", "ctaid.x", "ctaid.y",
            "ctaid.z", "ntid.x", "ntid.y", "ntid.z"};
        for (unsigned c = 0; c < kNumCoordParams; ++c)
            fn->getArg(i++)->setName(kCoordNames[c]);
        decorateKernel(fn, m);
        return fn;
    }
    // materializeParam (fn->getArg) + bufferElementPtr (GEP) defaults are
    // correct: host pointers are addrspace 0, and the coordinate params live
    // past the materialized param indices, so they never collide.

    // Wave ops, width 1 (one work-item per host invocation): the single-lane
    // semantics, matching the __cajeta_xpu_wave_* runtime emulation stubs.
    llvm::Value* waveWidth(llvm::IRBuilderBase& b, llvm::Module& m) override {
        return llvm::ConstantInt::get(llvm::Type::getInt32Ty(m.getContext()), 1);
    }
    llvm::Value* waveShuffle(llvm::IRBuilderBase&, llvm::Module&,
                             llvm::Value* value, llvm::Value* /*srcLane*/) override {
        return value;  // only lane 0 exists; any read returns this lane's value
    }
    llvm::Value* waveBallot(llvm::IRBuilderBase& b, llvm::Module& m,
                            llvm::Value* pred) override {
        return b.CreateZExt(pred, llvm::Type::getInt64Ty(m.getContext()));
    }
    llvm::Value* waveReduceSum(llvm::IRBuilderBase&, llvm::Module&,
                               llvm::Value* value) override {
        return value;  // sum over a single lane
    }

private:
    // Read coordinate (group + dim) from the 9 trailing kernel args, laid out
    // [tid.xyz, ctaid.xyz, ntid.xyz]. group ∈ {0,3,6}, dim ∈ {0,1,2}.
    static llvm::Value* coord(llvm::IRBuilderBase& b, unsigned group,
                              unsigned dim) {
        llvm::Function* fn = b.GetInsertBlock()->getParent();
        unsigned n = fn->arg_size();
        return fn->getArg(n - kNumCoordParams + group + dim);
    }
};

} // namespace

llvm::Function* lowerKernel(const MethodPtr& method,
                            llvm::Module& deviceModule) {
    CpuTarget target;
    return cajeta::xpu::lowerKernel(method, deviceModule, target);
}

} // namespace cpu
} // namespace xpu
} // namespace cajeta
