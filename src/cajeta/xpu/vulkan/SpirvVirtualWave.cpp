// A virtual wave on Vulkan — see header.

#include "SpirvVirtualWave.h"
#include "SpirvBackend.h"

#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicsSPIRV.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/Scalar/EarlyCSE.h"
#include "llvm/Transforms/Scalar/LoopPassManager.h"
#include "llvm/Transforms/Scalar/LoopRotation.h"
#include "llvm/Transforms/Scalar/SROA.h"
#include "llvm/Transforms/Scalar/Scalarizer.h"
#include "llvm/IR/CFG.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/Mem2Reg.h"
#include "llvm/Transforms/Vectorize/LoopVectorize.h"

#include <cmath>
#include <map>
#include <cstdlib>
#include "llvm/Support/raw_ostream.h"
#include <limits>

namespace cajeta {
namespace xpu {
namespace vulkan {

namespace {

const char* kVirtualStubs[] = {kVirtualShuffle, kVirtualReduceSumF32, kVirtualReduceMaxF32,
                               kVirtualReduceMinF32};

llvm::Function* variantShell(llvm::Module& m, const std::string& name, llvm::FunctionType* ty) {
    if (auto* f = m.getFunction(name)) return f;
    auto* f = llvm::Function::Create(ty, llvm::GlobalValue::InternalLinkage, name, &m);
    f->addFnAttr(llvm::Attribute::AlwaysInline);
    f->addFnAttr(llvm::Attribute::Convergent);
    f->setDoesNotThrow();
    return f;
}

llvm::Value* sublaneId(llvm::IRBuilder<>& b, llvm::Module& m) {
    return b.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
                            &m, llvm::Intrinsic::spv_subgroup_local_invocation_id),
                        {}, "sublane");
}

llvm::Value* readLane(llvm::IRBuilder<>& b, llvm::Module& m, llvm::Value* v, llvm::Value* src) {
    llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
        &m, llvm::Intrinsic::spv_wave_readlane, {b.getInt32Ty()});
    return b.CreateCall(f, {v, src}, "rl");
}

llvm::Value* activeInvocations(llvm::IRBuilder<>& b, llvm::Module& m) {
    llvm::Function* f =
        llvm::Intrinsic::getOrInsertDeclaration(&m, llvm::Intrinsic::spv_subgroup_ballot);
    return b.CreateExtractElement(b.CreateCall(f, {b.getTrue()}, "active"), uint64_t(0));
}

// Shuffle: slot k reads logical lane src[k], held by invocation src % S in slot src / S.
void buildShuffle(llvm::Module& m, llvm::Function* fn, unsigned c, unsigned s) {
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(m.getContext(), "entry", fn));
    llvm::Value* val = fn->getArg(0);
    llvm::Value* src = fn->getArg(1);
    const unsigned logS = (unsigned) std::log2(s);
    llvm::Value* res = llvm::PoisonValue::get(val->getType());
    for (unsigned k = 0; k < c; ++k) {
        llvm::Value* sk = b.CreateExtractElement(src, uint64_t(k));
        llvm::Value* inv = b.CreateAnd(sk, b.getInt32(s - 1));
        llvm::Value* sl = b.CreateLShr(sk, b.getInt32(logS));
        llvm::Value* r = b.getInt32(0);
        for (unsigned j = 0; j < c; ++j) {
            llvm::Value* t = readLane(b, m, b.CreateExtractElement(val, uint64_t(j)), inv);
            r = b.CreateSelect(b.CreateICmpEQ(sl, b.getInt32(j)), t, r);
        }
        res = b.CreateInsertElement(res, r, uint64_t(k));
    }
    b.CreateRet(res);
}

// Float reduce in the reference order: an xor butterfly at distance 1, 2 ... W/2. Below S a
// partner block is read from its first active invocation, or is the identity when none is.
void buildReduceF32(llvm::Module& m, llvm::Function* fn, const char* stub, unsigned w,
                    unsigned c, unsigned s, bool masked) {
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(m.getContext(), "entry", fn));
    llvm::Type* f32 = b.getFloatTy();
    const bool isSum = std::string(stub) == kVirtualReduceSumF32;
    const bool isMax = std::string(stub) == kVirtualReduceMaxF32;
    llvm::Constant* ident = isSum ? llvm::ConstantFP::get(f32, 0.0)
        : llvm::ConstantFP::getInfinity(f32, /*Negative=*/isMax);
    auto combine = [&](llvm::Value* a, llvm::Value* p) -> llvm::Value* {
        if (isSum) return b.CreateFAdd(a, p, "vred.sum");
        return b.CreateBinaryIntrinsic(isMax ? llvm::Intrinsic::maxnum : llvm::Intrinsic::minnum,
                                       a, p, llvm::FMFSource(), "vred.mm");
    };
    llvm::Value* x = fn->getArg(0);
    if (masked)
        x = b.CreateSelect(fn->getArg(1), x, b.CreateVectorSplat(c, ident), "vred.in");
    llvm::Value* lane = sublaneId(b, m);
    llvm::Value* active = activeInvocations(b, m);
    llvm::Function* cttz = llvm::Intrinsic::getOrInsertDeclaration(
        &m, llvm::Intrinsic::spv_firstbitlow, {b.getInt32Ty()});
    for (unsigned d = 1; d < w; d <<= 1) {
        if (d < s) {
            llvm::Value* base = b.CreateAnd(b.CreateXor(lane, b.getInt32(d)), b.getInt32(~(d - 1)));
            llvm::Value* blk = b.CreateAnd(b.CreateLShr(active, base), b.getInt32((1u << d) - 1));
            llvm::Value* none = b.CreateICmpEQ(blk, b.getInt32(0));
            llvm::Value* first = b.CreateCall(cttz, {blk});
            llvm::Value* src = b.CreateSelect(none, lane, b.CreateAdd(base, first), "vred.src");
            llvm::Value* next = llvm::PoisonValue::get(x->getType());
            for (unsigned k = 0; k < c; ++k) {
                llvm::Value* xk = b.CreateExtractElement(x, uint64_t(k));
                llvm::Value* got = b.CreateBitCast(
                    readLane(b, m, b.CreateBitCast(xk, b.getInt32Ty()), src), f32);
                llvm::Value* p = b.CreateSelect(none, ident, got);
                next = b.CreateInsertElement(next, combine(xk, p), uint64_t(k));
            }
            x = next;
        } else {
            llvm::SmallVector<int, 16> mask;
            for (unsigned k = 0; k < c; ++k) mask.push_back((int) (k ^ (d / s)));
            x = combine(x, b.CreateShuffleVector(x, mask, "vred.partner"));
        }
    }
    b.CreateRet(x);
}

void attachVariants(llvm::Module& m, llvm::Function* stub, unsigned w, unsigned c, unsigned s) {
    llvm::LLVMContext& ctx = m.getContext();
    const std::string name = stub->getName().str();
    const std::string sc = std::to_string(c);
    auto* maskTy = llvm::FixedVectorType::get(llvm::Type::getInt1Ty(ctx), c);
    std::vector<llvm::Type*> vargs;
    for (llvm::Type* t : stub->getFunctionType()->params())
        vargs.push_back(llvm::FixedVectorType::get(t, c));
    llvm::Type* vret = llvm::FixedVectorType::get(stub->getReturnType(), c);
    std::vector<llvm::Type*> margs = vargs;
    margs.push_back(maskTy);
    llvm::Function* un = variantShell(m, name + "_v" + sc, llvm::FunctionType::get(vret, vargs, false));
    llvm::Function* mk = variantShell(m, name + "_Mv" + sc, llvm::FunctionType::get(vret, margs, false));
    if (name == kVirtualShuffle) {
        buildShuffle(m, un, c, s);
        buildShuffle(m, mk, c, s);
    } else {
        buildReduceF32(m, un, name.c_str(), w, c, s, false);
        buildReduceF32(m, mk, name.c_str(), w, c, s, true);
    }
    std::string tokens(stub->arg_size(), 'v');
    stub->addFnAttr("vector-function-abi-variant",
                    "_ZGV_LLVM_N" + sc + tokens + "_" + name + "(" + un->getName().str() + "),"
                    + "_ZGV_LLVM_M" + sc + tokens + "_" + name + "(" + mk->getName().str() + ")");
}


bool inLoop(llvm::BasicBlock* bb, llvm::BasicBlock* pre, llvm::BasicBlock* done) {
    return bb != pre && bb != done;
}

// Builtin coordinates, descriptor handles and launch witnesses are the same for every slot.
void hoistInvariants(llvm::Function& f, llvm::BasicBlock* pre, llvm::BasicBlock* done) {
    llvm::SmallVector<llvm::Instruction*, 16> moves;
    for (llvm::BasicBlock& bb : f) {
        if (!inLoop(&bb, pre, done)) continue;
        for (llvm::Instruction& in : bb) {
            bool invariantOperands = true;
            for (llvm::Value* op : in.operands())
                if (auto* oi = llvm::dyn_cast<llvm::Instruction>(op))
                    if (inLoop(oi->getParent(), pre, done)) invariantOperands = false;
            if (!invariantOperands) continue;
            if (auto* ii = llvm::dyn_cast<llvm::IntrinsicInst>(&in)) {
                switch (ii->getIntrinsicID()) {
                    case llvm::Intrinsic::spv_resource_handlefrombinding:
                    case llvm::Intrinsic::spv_thread_id_in_group:
                    case llvm::Intrinsic::spv_thread_id:
                    case llvm::Intrinsic::spv_group_id:
                    case llvm::Intrinsic::spv_num_workgroups:
                    case llvm::Intrinsic::spv_subgroup_local_invocation_id:
                    case llvm::Intrinsic::spv_subgroup_size:
                        moves.push_back(&in);
                        break;
                    default:
                        break;
                }
            } else if (auto* ld = llvm::dyn_cast<llvm::LoadInst>(&in)) {
                if (auto* gv = llvm::dyn_cast<llvm::GlobalVariable>(ld->getPointerOperand()))
                    if (gv->getName().starts_with(kWorkgroupDimWitness)
                            || gv->getName().starts_with("cajeta_spec_"))
                        moves.push_back(&in);
            }
        }
    }
    for (llvm::Instruction* in : moves) in->moveBefore(pre->getTerminator()->getIterator());
}

struct BufferBase {
    llvm::CallInst* base;
    llvm::Value* handle;
    llvm::Function* getptr;
};

// Each descriptor element pointer in the loop becomes a GEP off one hoisted base, so the
// vectorizer sees plain memory; restoreBufferAccesses turns each back after it has run.
std::vector<BufferBase> baseBufferAccesses(llvm::Function& f, llvm::BasicBlock* pre,
                                           llvm::BasicBlock* done) {
    std::vector<BufferBase> bases;
    llvm::SmallVector<llvm::CallInst*, 16> gps;
    for (llvm::BasicBlock& bb : f) {
        if (!inLoop(&bb, pre, done)) continue;
        for (llvm::Instruction& in : bb)
            if (auto* ii = llvm::dyn_cast<llvm::IntrinsicInst>(&in))
                if (ii->getIntrinsicID() == llvm::Intrinsic::spv_resource_getpointer)
                    gps.push_back(ii);
    }
    for (llvm::CallInst* gp : gps) {
        llvm::Value* h = gp->getArgOperand(0);
        auto* tt = llvm::dyn_cast<llvm::TargetExtType>(h->getType());
        auto* arr = tt && tt->getNumTypeParameters() > 0
            ? llvm::dyn_cast<llvm::ArrayType>(tt->getTypeParameter(0)) : nullptr;
        if (!arr) continue;
        BufferBase* bb = nullptr;
        for (auto& x : bases)
            if (x.handle == h && x.getptr == gp->getCalledFunction()) bb = &x;
        if (!bb) {
            llvm::IRBuilder<> b(pre->getTerminator());
            llvm::CallInst* base = b.CreateCall(gp->getCalledFunction(),
                                                {h, b.getInt32(0)}, "vbuf.base");
            bases.push_back({base, h, gp->getCalledFunction()});
            bb = &bases.back();
        }
        llvm::IRBuilder<> b(gp);
        llvm::Value* g = b.CreateGEP(arr->getElementType(), bb->base, gp->getArgOperand(1),
                                     "vbuf.elem");
        gp->replaceAllUsesWith(g);
        gp->eraseFromParent();
    }
    return bases;
}

// Every address the vectorizer left off a base becomes a descriptor element pointer again.
bool restoreBufferAccesses(std::vector<BufferBase>& bases, std::string* whyNot) {
    for (BufferBase& bb : bases) {
        for (llvm::User* u : llvm::make_early_inc_range(bb.base->users())) {
            auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(u);
            if (!gep) continue;
            if (gep->getNumIndices() != 1 || gep->getType()->isVectorTy()) {
                *whyNot = "a buffer address the slot vectorizer widened";
                return false;
            }
            llvm::IRBuilder<> b(gep);
            llvm::Value* idx = b.CreateZExtOrTrunc(gep->getOperand(1), b.getInt32Ty());
            llvm::Value* p = b.CreateCall(bb.getptr, {bb.handle, idx}, "vbuf.ptr");
            gep->replaceAllUsesWith(p);
            gep->eraseFromParent();
        }
        for (llvm::User* u : bb.base->users())
            if (auto* ld = llvm::dyn_cast<llvm::LoadInst>(u); ld && ld->getType()->isVectorTy()) {
                *whyNot = "a buffer load the slot vectorizer widened";
                return false;
            } else if (auto* st = llvm::dyn_cast<llvm::StoreInst>(u);
                       st && st->getValueOperand()->getType()->isVectorTy()) {
                *whyNot = "a buffer store the slot vectorizer widened";
                return false;
            }
        if (bb.base->use_empty()) bb.base->eraseFromParent();
    }
    return true;
}


// The vectorizer's scalar twin runs only when its index-overflow check fails, which needs an
// element index past 2^31, out of bounds on any buffer. Every path into it goes to the vector loop.
void retireScalarTwin(llvm::Function& f) {
    llvm::BasicBlock* twin = nullptr;
    for (llvm::BasicBlock& bb : f)
        if (bb.getName() == "scalar.ph") twin = &bb;
    if (!twin) return;
    for (llvm::BasicBlock* pred : llvm::to_vector(llvm::predecessors(twin))) {
        auto* br = llvm::dyn_cast<llvm::BranchInst>(pred->getTerminator());
        if (!br || !br->isConditional()) continue;
        llvm::BasicBlock* other = br->getSuccessor(0) == twin ? br->getSuccessor(1)
                                                              : br->getSuccessor(0);
        twin->removePredecessor(pred);
        llvm::BranchInst::Create(other, br->getIterator());
        br->eraseFromParent();
    }
    llvm::removeUnreachableBlocks(f);
}
void forceSlotLoop(llvm::BranchInst* latch, unsigned c) {
    llvm::LLVMContext& ctx = latch->getContext();
    llvm::Function* f = latch->getFunction();
    llvm::MDNode* group = llvm::MDNode::getDistinct(ctx, {});
    for (llvm::BasicBlock& bb : *f)
        for (llvm::Instruction& in : bb)
            if (in.mayReadOrWriteMemory())
                in.setMetadata(llvm::LLVMContext::MD_access_group, group);
    auto md = [&](const char* key, llvm::Constant* v) {
        return llvm::MDNode::get(ctx, {llvm::MDString::get(ctx, key),
                                       llvm::ConstantAsMetadata::get(v)});
    };
    llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
    llvm::SmallVector<llvm::Metadata*, 6> ops = {
        nullptr,
        md("llvm.loop.vectorize.width", llvm::ConstantInt::get(i32, c)),
        md("llvm.loop.vectorize.enable", llvm::ConstantInt::getTrue(ctx)),
        md("llvm.loop.interleave.count", llvm::ConstantInt::get(i32, 1)),
        llvm::MDNode::get(ctx, {llvm::MDString::get(ctx, "llvm.loop.parallel_accesses"), group}),
    };
    llvm::MDNode* id = llvm::MDNode::getDistinct(ctx, ops);
    id->replaceOperandWith(0, id);
    latch->setMetadata(llvm::LLVMContext::MD_loop, id);
}

} // namespace

llvm::Function* virtualWaveStub(llvm::Module& m, const char* name, llvm::Type* ret,
                                const std::vector<llvm::Type*>& args) {
    if (auto* f = m.getFunction(name)) return f;
    auto* f = llvm::Function::Create(llvm::FunctionType::get(ret, args, false),
                                     llvm::GlobalValue::ExternalLinkage, name, &m);
    f->setDoesNotThrow();
    f->setWillReturn();
    f->addFnAttr(llvm::Attribute::Convergent);
    f->setMemoryEffects(llvm::MemoryEffects::none());
    return f;
}

llvm::Function* buildVirtualEntry(llvm::Function* slotFn, llvm::Module& m,
                                  llvm::TargetMachine& tm, const std::string& entryName,
                                  unsigned waveWidth, unsigned subgroup, std::string* whyNot) {
    llvm::LLVMContext& ctx = m.getContext();
    const unsigned c = waveWidth / subgroup;
    llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
    auto* entry = llvm::Function::Create(llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), false),
                                         llvm::Function::ExternalLinkage, entryName, &m);
    for (const llvm::Attribute& a : slotFn->getAttributes().getFnAttrs()) entry->addFnAttr(a);
    entry->addFnAttr("hlsl.shader", "compute");
    entry->addFnAttr("hlsl.numthreads", std::to_string(kVulkanLocalSizeX) + ",1,1");

    llvm::BasicBlock* pre = llvm::BasicBlock::Create(ctx, "entry", entry);
    llvm::BasicBlock* loop = llvm::BasicBlock::Create(ctx, "slot", entry);
    llvm::BasicBlock* exit = llvm::BasicBlock::Create(ctx, "done", entry);
    llvm::IRBuilder<> b(pre);
    b.CreateBr(loop);
    b.SetInsertPoint(loop);
    llvm::PHINode* k = b.CreatePHI(i32, 2, "k");
    k->addIncoming(b.getInt32(0), pre);
    llvm::CallInst* call = b.CreateCall(slotFn, {k});
    llvm::Value* next = b.CreateAdd(k, b.getInt32(1), "k.next");
    k->addIncoming(next, loop);
    llvm::BranchInst* latch = b.CreateCondBr(b.CreateICmpULT(next, b.getInt32(c)), loop, exit);
    b.SetInsertPoint(exit);
    b.CreateRetVoid();

    llvm::InlineFunctionInfo ifi;
    if (!llvm::InlineFunction(*call, ifi).isSuccess()) {
        *whyNot = "the per-slot body could not be inlined into the slot loop";
        entry->eraseFromParent();
        return nullptr;
    }
    slotFn->eraseFromParent();

    llvm::PassBuilder pb(&tm);
    llvm::LoopAnalysisManager lam;
    llvm::FunctionAnalysisManager fam;
    llvm::CGSCCAnalysisManager cgam;
    llvm::ModuleAnalysisManager mam;
    pb.registerModuleAnalyses(mam);
    pb.registerCGSCCAnalyses(cgam);
    pb.registerFunctionAnalyses(fam);
    pb.registerLoopAnalyses(lam);
    pb.crossRegisterProxies(lam, fam, cgam, mam);
    {
        llvm::FunctionPassManager fpm;
        fpm.addPass(llvm::SROAPass(llvm::SROAOptions::ModifyCFG));
        fpm.addPass(llvm::PromotePass());
        fpm.addPass(llvm::EarlyCSEPass());
        fpm.run(*entry, fam);
    }
    hoistInvariants(*entry, pre, exit);
    std::vector<BufferBase> bases = baseBufferAccesses(*entry, pre, exit);
    for (const char* name : kVirtualStubs)
        if (llvm::Function* stub = m.getFunction(name))
            attachVariants(m, stub, waveWidth, c, subgroup);
    latch = llvm::cast<llvm::BranchInst>(exit->getSinglePredecessor()->getTerminator());
    forceSlotLoop(latch, c);
    fam.clear();
    if (const char* dir = std::getenv("CAJETA_XPU_VK_VIRTUAL_DUMP")) {
        std::error_code ec;
        llvm::raw_fd_ostream os(std::string(dir) + "/" + entryName + ".prelv.ll", ec);
        if (!ec) m.print(os, nullptr);
    }

    llvm::FunctionPassManager fpm;
    fpm.addPass(llvm::createFunctionToLoopPassAdaptor(llvm::LoopRotatePass()));
    fpm.addPass(llvm::LoopVectorizePass());
    fpm.run(*entry, fam);
    retireScalarTwin(*entry);
    if (!restoreBufferAccesses(bases, whyNot)) return nullptr;

    for (llvm::BasicBlock& bb : *entry)
        for (llvm::Instruction& in : bb)
            if (auto* cl = llvm::dyn_cast<llvm::CallInst>(&in))
                if (llvm::Function* cf = cl->getCalledFunction())
                    if (cf->getName().starts_with("__cajeta_vk_v")
                            && !cf->getName().contains("_v" + std::to_string(c))
                            && !cf->getName().contains("_Mv" + std::to_string(c))) {
                        *whyNot = cf->getName().str() + " was left per lane (the slot loop "
                                  "did not vectorize at " + std::to_string(c) + ")";
                        return nullptr;
                    }
    if (c > 4) {
        llvm::SmallVector<llvm::CallInst*, 16> calls;
        for (llvm::BasicBlock& bb : *entry)
            for (llvm::Instruction& in : bb)
                if (auto* cl = llvm::dyn_cast<llvm::CallInst>(&in))
                    if (llvm::Function* cf = cl->getCalledFunction())
                        if (cf->getName().starts_with("__cajeta_vk_v") && !cf->isDeclaration())
                            calls.push_back(cl);
        for (llvm::CallInst* cl : calls) {
            llvm::InlineFunctionInfo vi;
            llvm::InlineFunction(*cl, vi);
        }
        llvm::ScalarizerPassOptions so;
        so.ScalarizeMinBits = 0;
        llvm::FunctionPassManager split;
        split.addPass(llvm::ScalarizerPass(so));
        split.run(*entry, fam);
    }
    return entry;
}

} // namespace vulkan
} // namespace xpu
} // namespace cajeta
