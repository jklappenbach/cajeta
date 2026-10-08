// A virtual wave on Vulkan — see header.

#include "SpirvVirtualWave.h"
#include "SpirvBackend.h"
#include "../cpu/CpuBarrierFission.h"
#include "cajeta/error/Exception.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Dominators.h"

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
#include "llvm/Transforms/Scalar/LoopUnrollPass.h"
#include "llvm/ADT/DepthFirstIterator.h"
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
    const bool segmented = std::string(stub) == kVirtualSegSumF32
                        || std::string(stub) == kVirtualSegMaxF32;
    const bool isSum = std::string(stub) == kVirtualReduceSumF32
                    || std::string(stub) == kVirtualSegSumF32;
    const bool isMax = std::string(stub) == kVirtualReduceMaxF32
                    || std::string(stub) == kVirtualSegMaxF32;
    llvm::Value* seg = segmented ? b.CreateExtractElement(fn->getArg(1), uint64_t(0)) : nullptr;
    llvm::Constant* ident = isSum ? llvm::ConstantFP::get(f32, 0.0)
        : llvm::ConstantFP::getInfinity(f32, /*Negative=*/isMax);
    auto combine = [&](llvm::Value* a, llvm::Value* p) -> llvm::Value* {
        if (isSum) return b.CreateFAdd(a, p, "vred.sum");
        return b.CreateBinaryIntrinsic(isMax ? llvm::Intrinsic::maxnum : llvm::Intrinsic::minnum,
                                       a, p, llvm::FMFSource(), "vred.mm");
    };
    llvm::Value* x = fn->getArg(0);
    if (masked)
        x = b.CreateSelect(fn->getArg(segmented ? 2 : 1), x, b.CreateVectorSplat(c, ident),
                           "vred.in");
    llvm::Value* lane = sublaneId(b, m);
    llvm::Value* active = activeInvocations(b, m);
    llvm::Function* cttz = llvm::Intrinsic::getOrInsertDeclaration(
        &m, llvm::Intrinsic::spv_firstbitlow, {b.getInt32Ty()});
    for (unsigned d = 1; d < w; d <<= 1) {
        llvm::Value* before = x;
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
        if (seg)
            x = b.CreateSelect(b.CreateICmpULT(b.getInt32(d), seg), x, before, "vred.seg");
    }
    b.CreateRet(x);
}

llvm::Value* sliceMasked(llvm::IRBuilder<>& b, llvm::Function* fn, unsigned maskArg, unsigned k,
                         llvm::Value* x, llvm::Value* ident) {
    if (fn->arg_size() <= maskArg) return x;
    return b.CreateSelect(b.CreateExtractElement(fn->getArg(maskArg), uint64_t(k)), x, ident);
}

// Ballot: slot k's subgroup ballot holds logical lanes k * S to k * S + S - 1.
void buildBallot(llvm::Module& m, llvm::Function* fn, unsigned c, unsigned s) {
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(m.getContext(), "entry", fn));
    llvm::Function* ballot =
        llvm::Intrinsic::getOrInsertDeclaration(&m, llvm::Intrinsic::spv_subgroup_ballot);
    llvm::Value* acc = b.getInt64(0);
    for (unsigned k = 0; k < c; ++k) {
        llvm::Value* p = sliceMasked(b, fn, 1, k, b.CreateExtractElement(fn->getArg(0), uint64_t(k)),
                                     b.getFalse());
        llvm::Value* lo = b.CreateExtractElement(b.CreateCall(ballot, {p}), uint64_t(0));
        llvm::Value* bits = b.CreateZExt(lo, b.getInt64Ty());
        if (s < 32) bits = b.CreateAnd(bits, b.getInt64((1ull << s) - 1));
        acc = b.CreateOr(acc, b.CreateShl(bits, b.getInt64((uint64_t) s * k)));
    }
    b.CreateRet(b.CreateVectorSplat(c, acc));
}

// An integer reduce is exact in any order: each slot reduces across the subgroup, then the slots combine.
void buildIntReduce(llvm::Module& m, llvm::Function* fn, const std::string& op, unsigned c) {
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(m.getContext(), "entry", fn));
    llvm::Intrinsic::ID id = op == "sum" ? llvm::Intrinsic::spv_wave_reduce_sum
        : op == "umax" ? llvm::Intrinsic::spv_wave_reduce_umax
        : op == "umin" ? llvm::Intrinsic::spv_wave_reduce_umin
        : op == "smax" ? llvm::Intrinsic::spv_wave_reduce_max
        : op == "smin" ? llvm::Intrinsic::spv_wave_reduce_min
        : op == "and" ? llvm::Intrinsic::spv_wave_reduce_and
        : op == "or" ? llvm::Intrinsic::spv_wave_reduce_or : llvm::Intrinsic::spv_wave_reduce_xor;
    uint32_t ident = (op == "and" || op == "umin") ? 0xFFFFFFFFu : op == "smax" ? 0x80000000u
                   : op == "smin" ? 0x7FFFFFFFu : 0u;
    llvm::Function* red = llvm::Intrinsic::getOrInsertDeclaration(&m, id, {b.getInt32Ty()});
    auto combine = [&](llvm::Value* a, llvm::Value* x) -> llvm::Value* {
        if (op == "sum") return b.CreateAdd(a, x);
        if (op == "and") return b.CreateAnd(a, x);
        if (op == "or") return b.CreateOr(a, x);
        if (op == "xor") return b.CreateXor(a, x);
        llvm::Value* aWins = op == "umax" ? b.CreateICmpUGT(a, x) : op == "umin" ? b.CreateICmpULT(a, x)
                           : op == "smax" ? b.CreateICmpSGT(a, x) : b.CreateICmpSLT(a, x);
        return b.CreateSelect(aWins, a, x);
    };
    llvm::Value* acc = nullptr;
    for (unsigned k = 0; k < c; ++k) {
        llvm::Value* x = sliceMasked(b, fn, 1, k, b.CreateExtractElement(fn->getArg(0), uint64_t(k)),
                                     b.getInt32(ident));
        llvm::Value* r = b.CreateCall(red, {x});
        acc = acc ? combine(acc, r) : r;
    }
    b.CreateRet(b.CreateVectorSplat(c, acc));
}

// Exclusive scan in logical lane order: every lane of earlier slots, then the earlier
// invocations of this slot.
void buildScan(llvm::Module& m, llvm::Function* fn, bool product, unsigned c, unsigned s) {
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(m.getContext(), "entry", fn));
    llvm::Function* pre = llvm::Intrinsic::getOrInsertDeclaration(
        &m, product ? llvm::Intrinsic::spv_wave_prefix_product : llvm::Intrinsic::spv_wave_prefix_sum,
        {b.getInt32Ty()});
    auto op = [&](llvm::Value* a, llvm::Value* x) {
        return product ? b.CreateMul(a, x) : b.CreateAdd(a, x);
    };
    llvm::Value* before = b.getInt32(product ? 1 : 0);
    llvm::Value* res = llvm::PoisonValue::get(fn->getReturnType());
    for (unsigned k = 0; k < c; ++k) {
        llvm::Value* x = sliceMasked(b, fn, 1, k, b.CreateExtractElement(fn->getArg(0), uint64_t(k)),
                                     b.getInt32(product ? 1 : 0));
        llvm::Value* excl = b.CreateCall(pre, {x});
        res = b.CreateInsertElement(res, op(before, excl), uint64_t(k));
        before = op(before, readLane(b, m, op(excl, x), b.getInt32(s - 1)));
    }
    b.CreateRet(res);
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
    } else if (name == kVirtualBallot) {
        buildBallot(m, un, c, s);
        buildBallot(m, mk, c, s);
    } else if (name.rfind(kVirtualIntReduce, 0) == 0) {
        const std::string op = name.substr(std::string(kVirtualIntReduce).size());
        buildIntReduce(m, un, op, c);
        buildIntReduce(m, mk, op, c);
    } else if (name == kVirtualScanSum || name == kVirtualScanProduct) {
        buildScan(m, un, name == kVirtualScanProduct, c, s);
        buildScan(m, mk, name == kVirtualScanProduct, c, s);
    } else {
        buildReduceF32(m, un, name.c_str(), w, c, s, false);
        buildReduceF32(m, mk, name.c_str(), w, c, s, true);
    }
    std::string tokens(stub->arg_size(), 'v');
    stub->addFnAttr("vector-function-abi-variant",
                    "_ZGV_LLVM_N" + sc + tokens + "_" + name + "(" + un->getName().str() + "),"
                    + "_ZGV_LLVM_M" + sc + tokens + "_" + name + "(" + mk->getName().str() + ")");
}



bool isSlotInvariant(llvm::Instruction& in) {
    for (llvm::Value* op : in.operands())
        if (llvm::isa<llvm::Instruction>(op) || llvm::isa<llvm::Argument>(op)) return false;
    if (auto* ii = llvm::dyn_cast<llvm::IntrinsicInst>(&in)) {
        switch (ii->getIntrinsicID()) {
            case llvm::Intrinsic::spv_resource_handlefrombinding:
            case llvm::Intrinsic::spv_thread_id_in_group:
            case llvm::Intrinsic::spv_thread_id:
            case llvm::Intrinsic::spv_group_id:
            case llvm::Intrinsic::spv_num_workgroups:
            case llvm::Intrinsic::spv_subgroup_local_invocation_id:
            case llvm::Intrinsic::spv_subgroup_size:
                return true;
            default:
                return false;
        }
    }
    if (auto* ld = llvm::dyn_cast<llvm::LoadInst>(&in))
        if (auto* gv = llvm::dyn_cast<llvm::GlobalVariable>(ld->getPointerOperand()))
            return gv->getName().starts_with(kWorkgroupDimWitness)
                || gv->getName().starts_with("cajeta_spec_");
    return false;
}

// Each read of a handle, a builtin or a launch witness is redone at its use, so none is live
// across a barrier and fission never parks one in a slot.
void rematerializeInvariants(llvm::Function& f) {
    llvm::SmallVector<llvm::Instruction*, 16> defs;
    for (llvm::BasicBlock& bb : f)
        for (llvm::Instruction& in : bb)
            if (isSlotInvariant(in)) defs.push_back(&in);
    for (llvm::Instruction* def : defs) {
        for (llvm::Use& u : llvm::make_early_inc_range(def->uses())) {
            auto* user = llvm::cast<llvm::Instruction>(u.getUser());
            llvm::Instruction* at = user;
            if (auto* phi = llvm::dyn_cast<llvm::PHINode>(user))
                at = phi->getIncomingBlock(u)->getTerminator();
            llvm::Instruction* copy = def->clone();
            copy->insertBefore(at->getIterator());
            u.set(copy);
        }
        def->eraseFromParent();
    }
}

// SPIR-V has no array allocation: a context array of N slots becomes one [N x T] local.
bool fixArrayAllocas(llvm::Function& f, std::string* whyNot) {
    llvm::SmallVector<llvm::AllocaInst*, 8> arrays;
    for (llvm::BasicBlock& bb : f)
        for (llvm::Instruction& in : bb)
            if (auto* a = llvm::dyn_cast<llvm::AllocaInst>(&in); a && a->isArrayAllocation())
                arrays.push_back(a);
    for (llvm::AllocaInst* a : arrays) {
        auto* n = llvm::dyn_cast<llvm::ConstantInt>(a->getArraySize());
        if (!n) {
            *whyNot = "a per-slot local whose size is not a constant";
            return false;
        }
        llvm::IRBuilder<> b(a);
        llvm::Type* elemTy = a->getAllocatedType();
        auto* arrTy = llvm::ArrayType::get(elemTy, n->getZExtValue());
        llvm::AllocaInst* fixed = b.CreateAlloca(arrTy, nullptr, a->getName());
        fixed->setAlignment(a->getAlign());
        for (llvm::User* u : llvm::make_early_inc_range(a->users())) {
            auto* ui = llvm::cast<llvm::Instruction>(u);
            llvm::IRBuilder<> ub(ui);
            if (auto* g = llvm::dyn_cast<llvm::GetElementPtrInst>(ui);
                    g && g->getSourceElementType() == elemTy && g->getNumIndices() == 1) {
                llvm::Value* idx = g->getOperand(1);
                g->replaceAllUsesWith(ub.CreateInBoundsGEP(
                    arrTy, fixed, {llvm::ConstantInt::get(idx->getType(), 0), idx}, g->getName()));
                g->eraseFromParent();
            } else if (auto* ms = llvm::dyn_cast<llvm::MemSetInst>(ui);
                       ms && llvm::isa<llvm::ConstantInt>(ms->getValue())
                       && llvm::cast<llvm::ConstantInt>(ms->getValue())->isZero()) {
                for (uint64_t k = 0; k < n->getZExtValue(); ++k)
                    ub.CreateStore(llvm::Constant::getNullValue(elemTy),
                                   ub.CreateConstInBoundsGEP2_32(arrTy, fixed, 0, (unsigned) k));
                ms->eraseFromParent();
            } else if (llvm::isa<llvm::LoadInst>(ui) || llvm::isa<llvm::StoreInst>(ui)) {
                ui->replaceUsesOfWith(a, ub.CreateConstInBoundsGEP2_32(arrTy, fixed, 0, 0));
            } else {
                *whyNot = "a per-slot local addressed in a way a SPIR-V array cannot be";
                return false;
            }
        }
        a->eraseFromParent();
    }
    return true;
}

// `gep T, (gep [N x T], a, 0, i), j` is `gep [N x T], a, 0, i + j`: SPIR-V cannot step a
// pointer to one element of a local array onto the next.
void foldElementGeps(llvm::Function& f) {
    for (bool again = true; again;) {
        again = false;
        for (llvm::BasicBlock& bb : f)
            for (llvm::Instruction& in : llvm::make_early_inc_range(bb)) {
                auto* g = llvm::dyn_cast<llvm::GetElementPtrInst>(&in);
                if (!g || g->getNumIndices() != 1) continue;
                auto* p = llvm::dyn_cast<llvm::GetElementPtrInst>(g->getPointerOperand());
                if (!p || p->getNumIndices() != 2) continue;
                auto* arr = llvm::dyn_cast<llvm::ArrayType>(p->getSourceElementType());
                if (!arr || arr->getElementType() != g->getSourceElementType()) continue;
                auto* z = llvm::dyn_cast<llvm::ConstantInt>(p->getOperand(1));
                if (!z || !z->isZero()) continue;
                llvm::IRBuilder<> b(g);
                llvm::Value* i = p->getOperand(2);
                llvm::Value* j = b.CreateSExtOrTrunc(g->getOperand(1), i->getType());
                llvm::Value* flat = b.CreateInBoundsGEP(arr, p->getPointerOperand(),
                                                        {z, b.CreateAdd(i, j)}, g->getName());
                g->replaceAllUsesWith(flat);
                g->eraseFromParent();
                again = true;
            }
    }
}

// Fission's work-item loops run a known C, 1 and 1 times: each is unrolled in full, so every
// context-array index becomes a constant and SROA can keep the slots in registers.
void markSlotLoopsForUnroll(llvm::Function& f) {
    llvm::DominatorTree dt(f);
    llvm::LoopInfo li(dt);
    llvm::LLVMContext& ctx = f.getContext();
    for (llvm::Loop* top : li)
        for (llvm::Loop* l : llvm::depth_first(top)) {
            if (!l->getHeader()->getName().starts_with("wi.")) continue;
            llvm::BasicBlock* latch = l->getLoopLatch();
            if (!latch) continue;
            llvm::MDNode* id = llvm::MDNode::getDistinct(
                ctx, {nullptr, llvm::MDNode::get(ctx, {llvm::MDString::get(ctx, "llvm.loop.unroll.full")})});
            id->replaceOperandWith(0, id);
            latch->getTerminator()->setMetadata(llvm::LLVMContext::MD_loop, id);
        }
}

// A dynamic index into a local or workgroup array gains a zero loaded at the access, so loop
// strength reduction cannot turn it into a pointer induction variable SPIR-V cannot express.
void launderArrayIndices(llvm::Function& f) {
    llvm::Module& m = *f.getParent();
    llvm::Type* i32 = llvm::Type::getInt32Ty(f.getContext());
    llvm::Value* zero = nullptr;
    for (llvm::BasicBlock& bb : f)
        for (llvm::Instruction& in : bb) {
            auto* g = llvm::dyn_cast<llvm::GetElementPtrInst>(&in);
            if (!g || g->getNumIndices() < 2) continue;
            llvm::Value* base = g->getPointerOperand()->stripPointerCasts();
            auto* gv = llvm::dyn_cast<llvm::GlobalVariable>(base);
            if (!llvm::isa<llvm::AllocaInst>(base) && !(gv && gv->getAddressSpace() == 3)) continue;
            llvm::Use& last = g->getOperandUse(g->getNumOperands() - 1);
            if (llvm::isa<llvm::Constant>(last.get())) continue;
            if (!zero)
                zero = new llvm::GlobalVariable(m, i32, false, llvm::GlobalValue::InternalLinkage,
                                                llvm::ConstantInt::get(i32, 0), "cajeta_vk_zero",
                                                nullptr, llvm::GlobalValue::NotThreadLocal, 10);
            llvm::IRBuilder<> b(g);
            llvm::LoadInst* z = b.CreateLoad(i32, zero, "vzero");
            z->setVolatile(true);
            last.set(b.CreateAdd(last.get(), b.CreateZExtOrTrunc(z, last.get()->getType())));
        }
}
// Builtin coordinates, descriptor handles and launch witnesses are the same for every slot.
void hoistInvariants(llvm::Function& f) {
    llvm::BasicBlock* entry = &f.getEntryBlock();
    llvm::SmallVector<llvm::Instruction*, 16> moves;
    for (llvm::BasicBlock& bb : f) {
        if (&bb == entry) continue;
        for (llvm::Instruction& in : bb) {
            bool invariantOperands = true;
            for (llvm::Value* op : in.operands())
                if (auto* oi = llvm::dyn_cast<llvm::Instruction>(op))
                    if (oi->getParent() != entry) invariantOperands = false;
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
    for (llvm::Instruction* in : moves) in->moveBefore(entry->getTerminator()->getIterator());
}

struct BufferBase {
    llvm::CallInst* base;
    llvm::Value* handle;
    llvm::Function* getptr;
};

// Each descriptor element pointer becomes a GEP off one base in the entry block, so the
// vectorizer sees plain memory; restoreBufferAccesses turns each back after it has run.
std::vector<BufferBase> baseBufferAccesses(llvm::Function& f) {
    llvm::BasicBlock* entry = &f.getEntryBlock();
    std::vector<BufferBase> bases;
    llvm::SmallVector<llvm::CallInst*, 16> gps;
    for (llvm::BasicBlock& bb : f) {
        if (&bb == entry) continue;
        for (llvm::Instruction& in : bb)
            if (auto* ii = llvm::dyn_cast<llvm::IntrinsicInst>(&in))
                if (ii->getIntrinsicID() == llvm::Intrinsic::spv_resource_getpointer)
                    gps.push_back(ii);
    }
    for (llvm::CallInst* gp : gps) {
        llvm::Value* h = gp->getArgOperand(0);
        auto* hi = llvm::dyn_cast<llvm::Instruction>(h);
        if (hi && hi->getParent() != entry) continue;
        auto* tt = llvm::dyn_cast<llvm::TargetExtType>(h->getType());
        auto* arr = tt && tt->getNumTypeParameters() > 0
            ? llvm::dyn_cast<llvm::ArrayType>(tt->getTypeParameter(0)) : nullptr;
        if (!arr) continue;
        BufferBase* bb = nullptr;
        for (auto& x : bases)
            if (x.handle == h && x.getptr == gp->getCalledFunction()) bb = &x;
        if (!bb) {
            llvm::IRBuilder<> b(entry->getTerminator());
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

// A scalar twin runs only when its index-overflow check fails, which needs an element index
// past 2^31, out of bounds on any buffer. Every path into one goes to its vector loop.
void retireScalarTwins(llvm::Function& f) {
    llvm::SmallVector<llvm::BasicBlock*, 4> twins;
    for (llvm::BasicBlock& bb : f)
        if (bb.getName().starts_with("scalar.ph")) twins.push_back(&bb);
    for (llvm::BasicBlock* twin : twins)
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

// The slot loop ending at `latch` vectorizes at exactly C, its slots independent.
void forceSlotLoop(llvm::Instruction* latch, unsigned c, llvm::LoopInfo& li) {
    llvm::LLVMContext& ctx = latch->getContext();
    llvm::Loop* loop = li.getLoopFor(latch->getParent());
    if (!loop) return;
    llvm::MDNode* group = llvm::MDNode::getDistinct(ctx, {});
    for (llvm::BasicBlock* bb : loop->blocks())
        for (llvm::Instruction& in : *bb)
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

void inlineHelpers(llvm::Function& f) {
    for (bool again = true; again;) {
        again = false;
        llvm::SmallVector<llvm::CallInst*, 16> calls;
        for (llvm::BasicBlock& bb : f)
            for (llvm::Instruction& in : bb)
                if (auto* cl = llvm::dyn_cast<llvm::CallInst>(&in))
                    if (llvm::Function* cf = cl->getCalledFunction())
                        if (!cf->isDeclaration() && cf != &f) calls.push_back(cl);
        for (llvm::CallInst* cl : calls) {
            llvm::InlineFunctionInfo ifi;
            if (llvm::InlineFunction(*cl, ifi).isSuccess()) again = true;
        }
    }
}

bool hasLoop(llvm::Function& f) {
    llvm::DominatorTree dt(f);
    llvm::LoopInfo li(dt);
    return !li.empty();
}

bool isVirtualStub(llvm::Function* f) {
    return f->getName().starts_with(kVirtualPrefix);
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
    llvm::AttributeSet slotAttrs = slotFn->getAttributes().getFnAttrs();
    inlineHelpers(*slotFn);
    rematerializeInvariants(*slotFn);
    auto fail = [&](const std::string& why) -> llvm::Function* {
        *whyNot = why;
        entry->eraseFromParent();
        if (slotFn->getParent()) slotFn->eraseFromParent();
        return nullptr;
    };

    std::vector<llvm::Instruction*> latches;
    llvm::Value* zero = llvm::ConstantInt::get(i32, 0);
    llvm::Value* one = llvm::ConstantInt::get(i32, 1);
    llvm::Value* cv = llvm::ConstantInt::get(i32, c);
    if (cpu::usesBarrier(*slotFn) || hasLoop(*slotFn)) {
        std::vector<llvm::UncondBrInst*> wi;
        std::vector<llvm::BasicBlock*> bars;
        cpu::FissionHooks hooks;
        hooks.keepSharedMemory = true;
        hooks.barrierBlocks = &bars;
        try {
            cpu::fissionBarrierKernel(slotFn, entry, 0, {zero, zero, zero}, {cv, one, one},
                                      {one, one, one}, m, &wi, nullptr,
                                      /*scaffoldUniformLoops=*/true, hooks);
        } catch (cajeta::Exception& e) {
            return fail("slot fission: " + e.getMessage());
        }
        llvm::Function* bar = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::spv_group_memory_barrier_with_group_sync);
        for (llvm::BasicBlock* bb : bars) {
            llvm::IRBuilder<> b(&*bb->getFirstInsertionPt());
            b.CreateCall(bar, {});
        }
        latches.assign(wi.begin(), wi.end());
        slotFn->eraseFromParent();
        std::string why;
        if (!fixArrayAllocas(*entry, &why)) {
            *whyNot = why;
            entry->eraseFromParent();
            return nullptr;
        }
    } else {
        llvm::BasicBlock* pre = llvm::BasicBlock::Create(ctx, "entry", entry);
        llvm::BasicBlock* loop = llvm::BasicBlock::Create(ctx, "slot", entry);
        llvm::BasicBlock* exit = llvm::BasicBlock::Create(ctx, "done", entry);
        llvm::IRBuilder<> b(pre);
        b.CreateBr(loop);
        b.SetInsertPoint(loop);
        llvm::PHINode* k = b.CreatePHI(i32, 2, "k");
        k->addIncoming(b.getInt32(0), pre);
        llvm::CallInst* call = b.CreateCall(slotFn, {k, zero, zero, zero, zero, zero, cv, one,
                                                     one, one, one, one});
        llvm::Value* next = b.CreateAdd(k, b.getInt32(1), "k.next");
        k->addIncoming(next, loop);
        latches.push_back(b.CreateCondBr(b.CreateICmpULT(next, b.getInt32(c)), loop, exit));
        b.SetInsertPoint(exit);
        b.CreateRetVoid();
        llvm::InlineFunctionInfo ifi;
        if (!llvm::InlineFunction(*call, ifi).isSuccess())
            return fail("the per-slot body could not be inlined into the slot loop");
        slotFn->eraseFromParent();
    }
    entry->setAttributes(llvm::AttributeList());
    for (const llvm::Attribute& a : slotAttrs) entry->addFnAttr(a);
    entry->addFnAttr("hlsl.shader", "compute");
    entry->addFnAttr("hlsl.numthreads", std::to_string(kVulkanLocalSizeX) + ",1,1");

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
    hoistInvariants(*entry);
    {
        llvm::FunctionPassManager cse;
        cse.addPass(llvm::EarlyCSEPass());
        cse.run(*entry, fam);
    }
    std::vector<BufferBase> bases = baseBufferAccesses(*entry);
    for (llvm::Function& stub : llvm::make_early_inc_range(m))
        if (isVirtualStub(&stub) && stub.isDeclaration())
            attachVariants(m, &stub, waveWidth, c, subgroup);
    {
        llvm::DominatorTree dt(*entry);
        llvm::LoopInfo li(dt);
        for (llvm::Instruction* latch : latches) forceSlotLoop(latch, c, li);
    }
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
    retireScalarTwins(*entry);

    const std::string vsuf = "_v" + std::to_string(c), msuf = "_Mv" + std::to_string(c);
    for (llvm::BasicBlock& bb : *entry)
        for (llvm::Instruction& in : bb)
            if (auto* cl = llvm::dyn_cast<llvm::CallInst>(&in))
                if (llvm::Function* cf = cl->getCalledFunction())
                    if (isVirtualStub(cf) && !cf->getName().ends_with(vsuf)
                            && !cf->getName().ends_with(msuf)) {
                        *whyNot = cf->getName().str() + " was left per lane (the slot loop "
                                  "did not vectorize at " + std::to_string(c) + ")";
                        return nullptr;
                    }
    {
        llvm::SmallVector<llvm::CallInst*, 16> calls;
        for (llvm::BasicBlock& bb : *entry)
            for (llvm::Instruction& in : bb)
                if (auto* cl = llvm::dyn_cast<llvm::CallInst>(&in))
                    if (llvm::Function* cf = cl->getCalledFunction())
                        if (isVirtualStub(cf) && !cf->isDeclaration()) calls.push_back(cl);
        for (llvm::CallInst* cl : calls) {
            llvm::InlineFunctionInfo vi;
            llvm::InlineFunction(*cl, vi);
        }
        llvm::ScalarizerPassOptions so;
        so.ScalarizeMinBits = 0;
        so.ScalarizeLoadStore = true;
        llvm::FunctionPassManager split;
        split.addPass(llvm::ScalarizerPass(so));
        fam.clear();
        split.run(*entry, fam);
        foldElementGeps(*entry);
        if (!restoreBufferAccesses(bases, whyNot)) return nullptr;
        markSlotLoopsForUnroll(*entry);
        llvm::FunctionPassManager tidy;
        tidy.addPass(llvm::createFunctionToLoopPassAdaptor(llvm::LoopFullUnrollPass(2)));
        tidy.addPass(llvm::SROAPass(llvm::SROAOptions::ModifyCFG));
        tidy.addPass(llvm::PromotePass());
        tidy.addPass(llvm::EarlyCSEPass());
        fam.clear();
        tidy.run(*entry, fam);
    }
    launderArrayIndices(*entry);
    return entry;
}

} // namespace vulkan
} // namespace xpu
} // namespace cajeta
