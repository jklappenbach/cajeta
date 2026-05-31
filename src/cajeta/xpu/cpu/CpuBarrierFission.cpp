//
// CPU work-item loop fission for workgroup barriers — see header.
//

#include "CpuBarrierFission.h"

#include "cajeta/error/Exception.h"

#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <functional>
#include <vector>

namespace cajeta {
namespace xpu {
namespace cpu {

namespace {

const char* kBarrier = "__cajeta_xpu_cpu_barrier";

[[noreturn]] void unsupported(const std::string& what) {
    throw cajeta::Exception(
        "XPU kernel lowering: unsupported barrier construct on the CPU backend — "
        + what, "XPU-N02");
}

std::vector<llvm::CallInst*> barrierCalls(llvm::Function& f) {
    std::vector<llvm::CallInst*> out;
    for (auto& bb : f)
        for (auto& in : bb)
            if (auto* c = llvm::dyn_cast<llvm::CallInst>(&in))
                if (auto* cf = c->getCalledFunction())
                    if (cf->getName() == kBarrier) out.push_back(c);
    return out;
}

// A barrier-free region to wrap in a work-item loop.
struct RegionJob {
    llvm::BasicBlock* entry = nullptr;          // region's first block
    std::vector<llvm::BasicBlock*> blocks;
    llvm::BasicBlock* predBlock = nullptr;      // feeds the region (edge → entry)
    llvm::BasicBlock* doneTarget = nullptr;     // where the loop exits when done
    llvm::Value* iv = nullptr;                  // work-item index in this region
};

// Per-work-item value set: the least fixpoint over SSA def-use AND memory
// round-trips through per-work-item alloca slots. Used to tell per-work-item
// locals (context arrays across a barrier) from block-uniform ones.
//
// Pre-mem2reg, a plain def-use walk is not enough: `uint32 t = Thread.x()`
// stores the work-item index into `t`'s slot, and `int x = a[t] + …` reads it
// back — the SSA chain is broken by that store/load, so the value derived from
// `t` (and stored into `x`'s slot) would look block-uniform and `x` would not
// get a context array. So we also taint every load from any slot that ever
// receives a tainted value (a per-work-item slot), and re-propagate, to a
// fixpoint. Over-approximation is safe: a wrongly-widened uniform slot still
// computes the right value, just with redundant per-lane storage.
void computeTaint(llvm::Value* seed,
                  llvm::ArrayRef<llvm::AllocaInst*> slots,
                  llvm::SmallPtrSetImpl<llvm::Value*>& tainted) {
    llvm::SmallVector<llvm::Value*, 32> work{seed};
    for (;;) {
        while (!work.empty()) {                       // SSA def-use propagation
            llvm::Value* v = work.pop_back_val();
            if (!tainted.insert(v).second) continue;
            for (llvm::User* u : v->users())
                if (auto* i = llvm::dyn_cast<llvm::Instruction>(u))
                    work.push_back(i);
        }
        bool added = false;                           // memory propagation
        for (llvm::AllocaInst* a : slots) {
            bool perWorkItem = false;
            for (llvm::User* u : a->users())
                if (auto* st = llvm::dyn_cast<llvm::StoreInst>(u))
                    if (tainted.count(st->getValueOperand())) {
                        perWorkItem = true; break;
                    }
            if (!perWorkItem) continue;
            for (llvm::User* u : a->users())
                if (auto* ld = llvm::dyn_cast<llvm::LoadInst>(u))
                    if (!tainted.count(ld)) { work.push_back(ld); added = true; }
        }
        if (!added) break;
    }
}

} // namespace

bool usesBarrier(llvm::Function& linked) { return !barrierCalls(linked).empty(); }

void fissionBarrierKernel(llvm::Function* linked, llvm::Function* wrapper,
                          unsigned nReal,
                          const std::vector<llvm::Value*>& ctaid,
                          const std::vector<llvm::Value*>& ntid,
                          llvm::Module& hostModule,
                          std::vector<llvm::BranchInst*>* workItemLatches) {
    llvm::LLVMContext& ctx = wrapper->getContext();
    llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
    llvm::Value* ntidX = ntid[0];

    // --- 1. True-entry + the work-item-index placeholder --------------------
    auto* trueEntry = llvm::BasicBlock::Create(ctx, "entry", wrapper);
    llvm::IRBuilder<> eb(trueEntry);
    auto* placeholder = llvm::cast<llvm::Instruction>(
        eb.CreateFreeze(llvm::PoisonValue::get(i32), "wiid.ph"));

    // --- 2. Clone the per-work-item kernel body in --------------------------
    llvm::ValueToValueMapTy vmap;
    for (unsigned i = 0; i < nReal; ++i)
        vmap[linked->getArg(i)] = wrapper->getArg(i);
    vmap[linked->getArg(nReal + 0)] = placeholder;                    // tid.x
    vmap[linked->getArg(nReal + 1)] = llvm::ConstantInt::get(i32, 0); // tid.y
    vmap[linked->getArg(nReal + 2)] = llvm::ConstantInt::get(i32, 0); // tid.z
    for (unsigned d = 0; d < 3; ++d) {
        vmap[linked->getArg(nReal + 3 + d)] = ctaid[d];
        vmap[linked->getArg(nReal + 6 + d)] = ntid[d];
    }
    llvm::SmallVector<llvm::ReturnInst*, 4> returns;
    llvm::CloneFunctionInto(wrapper, linked, vmap,
                            llvm::CloneFunctionChangeType::LocalChangesOnly,
                            returns);
    llvm::BasicBlock* bodyEntry = nullptr;
    for (auto& bb : *wrapper)
        if (&bb != trueEntry) { bodyEntry = &bb; break; }

    // Connect the entry to the body so the CFG is reachable for DominatorTree /
    // LoopInfo (region wrapping later redirects this edge). Keep the entry
    // builder inserting *before* this terminator for the alloca setup below.
    auto* entryBr = eb.CreateBr(bodyEntry);
    eb.SetInsertPoint(entryBr);

    // --- 3. Split at barriers into empty boundary blocks --------------------
    llvm::SmallPtrSet<llvm::BasicBlock*, 4> boundarySet;
    for (llvm::CallInst* bc : barrierCalls(*wrapper)) {
        llvm::BasicBlock* bbBar = llvm::SplitBlock(bc->getParent(), bc);
        llvm::SplitBlock(bbBar, bc->getNextNode());
        bc->eraseFromParent();              // bbBar = `br <after>`
        boundarySet.insert(bbBar);
    }

    // --- 4. Analyses + uniformity guardrails --------------------------------
    llvm::DominatorTree DT(*wrapper);
    llvm::LoopInfo LI(DT);
    // The kernel's locals (entry-block allocas) — needed both to flow taint
    // through per-work-item slots (below) and to widen barrier-crossing ones
    // into context arrays (step 7).
    llvm::SmallVector<llvm::AllocaInst*, 8> allocas;
    for (auto& in : *bodyEntry)
        if (auto* a = llvm::dyn_cast<llvm::AllocaInst>(&in)) allocas.push_back(a);
    llvm::SmallPtrSet<llvm::Value*, 32> tainted;
    computeTaint(placeholder, allocas, tainted);

    auto loopHasBarrier = [&](llvm::Loop* L) {
        for (llvm::BasicBlock* b : boundarySet)
            if (L->contains(b)) return true;
        return false;
    };
    auto inLoopSuccOf = [&](llvm::Loop* L) -> llvm::BasicBlock* {
        for (llvm::BasicBlock* s : llvm::successors(L->getHeader()))
            if (L->contains(s)) return s;
        return L->getHeader();
    };
    for (llvm::Loop* top : LI) {
        for (llvm::Loop* L : llvm::depth_first(top)) {
            if (!loopHasBarrier(L)) continue;
            if (!L->getLoopLatch() || !L->getExitBlock())
                unsupported("a barrier in a loop without a single latch/exit");
            llvm::SmallVector<llvm::BasicBlock*, 4> exiting;
            L->getExitingBlocks(exiting);
            for (llvm::BasicBlock* xb : exiting)
                if (auto* br = llvm::dyn_cast<llvm::BranchInst>(xb->getTerminator()))
                    if (br->isConditional() && tainted.count(br->getCondition()))
                        unsupported("a barrier in a loop with a work-item-"
                                    "dependent trip count (must be uniform)");
        }
    }
    // Every barrier must be block-uniform: reached by all work-items, not under
    // work-item-divergent control flow. A barrier post-dominates its level's
    // entry (the function body, or its loop's in-loop successor) iff every path
    // through that level hits it — otherwise it sits in one arm of an `if` and
    // only some work-items would reach it (GPU-undefined; reject, don't
    // miscompile).
    llvm::PostDominatorTree PDT(*wrapper);
    for (llvm::BasicBlock* bar : boundarySet) {
        llvm::Loop* bl = LI.getLoopFor(bar);
        llvm::BasicBlock* levelEntry = bl ? inLoopSuccOf(bl) : bodyEntry;
        if (!PDT.dominates(bar, levelEntry))
            unsupported("a barrier under work-item-divergent control flow "
                        "(all work-items must reach every barrier)");
    }
    // Wave ops compose with barriers: fission produces clean counted work-item
    // loops, and the caller forces VF=W + attaches the wave SIMD variants on each
    // (`workItemLatches`), so a wave op inside a region becomes W SIMD lanes while
    // the barriers delimit regions (Inc 9). Nothing to reject here.

    // --- 5. Per-block shared memory -----------------------------------------
    // A `Shared<T>` array lowers to one module-level addrspace(3) global — one
    // instance, which would race across the blocks/threads that each call this
    // wrapper. Replace it with a wrapper-local buffer (fresh per block call);
    // the addrspacecast 0→3 is a no-op the CPU backend folds.
    llvm::SmallPtrSet<llvm::GlobalVariable*, 4> sharedGlobals;
    for (auto& bb : *wrapper)
        for (auto& in : bb)
            for (llvm::Value* op : in.operands())
                if (auto* gv = llvm::dyn_cast<llvm::GlobalVariable>(op))
                    if (gv->getAddressSpace() == 3) sharedGlobals.insert(gv);
    for (llvm::GlobalVariable* gv : sharedGlobals) {
        if (!gv->hasInitializer())
            unsupported("dynamic-sized shared memory with a barrier (require a "
                        "static size)");
        auto* buf = eb.CreateAlloca(gv->getValueType(), 0, nullptr,
                                    gv->getName() + ".blk");
        buf->setAlignment(llvm::Align(16));
        gv->replaceAllUsesWith(
            eb.CreateAddrSpaceCast(buf, llvm::PointerType::get(ctx, 3)));
        if (gv->use_empty()) gv->eraseFromParent();
    }

    // --- 6. Identify regions (loop-aware structured walk) -------------------
    // Top level is a sequence of regions and uniform loops; a uniform loop is
    // kept as the outer scalar scaffold (its header/latch run once per
    // iteration) and its body region is wrapped in a work-item loop nested
    // inside. A `wrapEnd` block holds the function's single return.
    auto* wrapEnd = llvm::BasicBlock::Create(ctx, "wrap.end", wrapper);
    llvm::ReturnInst::Create(ctx, wrapEnd);

    std::vector<RegionJob> jobs;
    auto hasReal = [](const std::vector<llvm::BasicBlock*>& bs) {
        for (llvm::BasicBlock* b : bs)
            for (auto& in : *b)
                if (!in.isTerminator() && !llvm::isa<llvm::PHINode>(in)
                    && !llvm::isa<llvm::AllocaInst>(in))
                    return true;
        return false;
    };
    struct Collected {
        std::vector<llvm::BasicBlock*> blocks;
        llvm::BasicBlock* barrier = nullptr;
        llvm::BasicBlock* subloop = nullptr;
        bool reachedRet = false;
        bool reachedLatch = false;
    };
    auto collect = [&](llvm::BasicBlock* start, llvm::Loop* encLoop) {
        Collected R;
        llvm::SmallPtrSet<llvm::BasicBlock*, 16> seen;
        llvm::SmallVector<llvm::BasicBlock*, 16> work{start};
        while (!work.empty()) {
            llvm::BasicBlock* bb = work.pop_back_val();
            if (!seen.insert(bb).second) continue;
            R.blocks.push_back(bb);
            auto* term = bb->getTerminator();
            if (llvm::isa<llvm::ReturnInst>(term)) { R.reachedRet = true; continue; }
            for (llvm::BasicBlock* s : llvm::successors(bb)) {
                if (boundarySet.count(s)) { R.barrier = s; continue; }
                if (encLoop && s == encLoop->getLoopLatch()) {
                    R.reachedLatch = true; continue;
                }
                llvm::Loop* sl = LI.getLoopFor(s);
                if (sl != encLoop && sl && sl->getHeader() == s
                    && loopHasBarrier(sl)) { R.subloop = s; continue; }
                work.push_back(s);
            }
        }
        return R;
    };
    auto inLoopSucc = [&](llvm::Loop* L) -> llvm::BasicBlock* {
        for (llvm::BasicBlock* s : llvm::successors(L->getHeader()))
            if (L->contains(s)) return s;
        return nullptr;
    };

    std::function<void(llvm::BasicBlock*, llvm::Loop*, llvm::BasicBlock*)> walk =
        [&](llvm::BasicBlock* start, llvm::Loop* encLoop,
            llvm::BasicBlock* predBlock) {
        llvm::BasicBlock* cur = start;
        llvm::BasicBlock* pred = predBlock;
        while (cur) {
            // Sitting directly on a barrier-subloop header (a nested loop with no
            // separating preheader region): enter it as a subloop rather than
            // letting `collect` walk through and flatten it. Cajeta's structured
            // loops have a preheader so this is defensive, but it keeps the walk
            // from misregioning any nested shape (never miscompile).
            if (llvm::Loop* cl = LI.getLoopFor(cur))
                if (cl != encLoop && cl->getHeader() == cur && loopHasBarrier(cl)) {
                    walk(inLoopSucc(cl), cl, cur);
                    pred = cur;
                    cur = cl->getExitBlock();
                    continue;
                }
            Collected R = collect(cur, encLoop);
            llvm::BasicBlock* done = nullptr;
            if (R.barrier) done = R.barrier;
            else if (R.subloop) done = R.subloop;
            else if (R.reachedRet) done = wrapEnd;
            else if (encLoop && R.reachedLatch) done = encLoop->getLoopLatch();
            else unsupported("unstructured barrier control flow");

            if (hasReal(R.blocks))
                jobs.push_back({cur, R.blocks, pred, done, nullptr});

            if (R.subloop) {
                llvm::Loop* L = LI.getLoopFor(R.subloop);
                walk(inLoopSucc(L), L, L->getHeader());       // wrap loop body
                pred = L->getHeader();
                cur = L->getExitBlock();                      // region after loop
                continue;
            }
            if (R.barrier) {
                pred = R.barrier;
                cur = R.barrier->getSingleSuccessor();
                continue;
            }
            return;   // reached ret or the loop latch — end of this level
        }
    };
    walk(bodyEntry, /*encLoop=*/nullptr, /*predBlock=*/trueEntry);

    auto regionOf = [&](llvm::BasicBlock* bb) -> int {
        for (size_t i = 0; i < jobs.size(); ++i)
            for (llvm::BasicBlock* b : jobs[i].blocks)
                if (b == bb) return (int) i;
        return -1;
    };

    // --- 7. Context arrays for tid-tainted locals live across a barrier -----
    // (`allocas` gathered in step 4, before the taint fixpoint.)
    llvm::DenseMap<llvm::AllocaInst*, llvm::AllocaInst*> ctxArray;
    for (llvm::AllocaInst* a : allocas) {
        bool perWorkItem = false;
        int only = -2;
        bool multi = false;
        for (llvm::User* u : a->users()) {
            auto* i = llvm::dyn_cast<llvm::Instruction>(u);
            if (!i) continue;
            if (auto* st = llvm::dyn_cast<llvm::StoreInst>(i))
                if (tainted.count(st->getValueOperand())) perWorkItem = true;
            int rg = regionOf(i->getParent());
            if (rg >= 0) {
                if (only == -2) only = rg;
                else if (only != rg) multi = true;
            }
        }
        if (perWorkItem && multi) {
            auto* arr = eb.CreateAlloca(a->getAllocatedType(), 0, ntidX,
                                        a->getName() + ".ctx");
            arr->setAlignment(a->getAlign());
            ctxArray[a] = arr;
        }
    }

    // --- 8. Hoist remaining allocas to the true entry -----------------------
    for (llvm::AllocaInst* a : allocas) {
        if (ctxArray.count(a)) continue;
        a->moveBefore(placeholder);
    }

    // --- 9. Wrap each region in a counted work-item loop --------------------
    for (RegionJob& J : jobs) {
        auto* ph = llvm::BasicBlock::Create(ctx, "wi.ph", wrapper, J.entry);
        auto* head = llvm::BasicBlock::Create(ctx, "wi.head", wrapper, J.entry);
        auto* latch = llvm::BasicBlock::Create(ctx, "wi.latch", wrapper, J.entry);

        // pred → preheader (redirect the specific edge pred → entry)
        auto* pterm = J.predBlock->getTerminator();
        bool redirected = false;
        for (unsigned s = 0; s < pterm->getNumSuccessors(); ++s)
            if (pterm->getSuccessor(s) == J.entry) {
                pterm->setSuccessor(s, ph); redirected = true;
            }
        if (!redirected) unsupported("region predecessor edge not found");
        llvm::BranchInst::Create(head, ph);

        llvm::IRBuilder<> hb(head);
        auto* tid = hb.CreatePHI(i32, 2, "wi.tid");
        tid->addIncoming(llvm::ConstantInt::get(i32, 0), ph);
        hb.CreateCondBr(hb.CreateICmpSLT(tid, ntidX, "wi.cond"), J.entry,
                        J.doneTarget);
        J.iv = tid;

        llvm::IRBuilder<> lb(latch);
        tid->addIncoming(
            lb.CreateAdd(tid, llvm::ConstantInt::get(i32, 1), "wi.next"), latch);
        auto* latchBr = lb.CreateBr(head);
        if (workItemLatches) workItemLatches->push_back(latchBr);

        // region exit edge(s) (to doneTarget, or a ret) → latch
        for (llvm::BasicBlock* bb : J.blocks) {
            auto* term = bb->getTerminator();
            if (llvm::isa<llvm::ReturnInst>(term)) {
                term->eraseFromParent();
                llvm::BranchInst::Create(latch, bb);
            } else {
                for (unsigned s = 0; s < term->getNumSuccessors(); ++s)
                    if (term->getSuccessor(s) == J.doneTarget)
                        term->setSuccessor(s, latch);
            }
        }
    }

    // --- 10. Rewrite per region: tid placeholder + context-array indexing ---
    for (RegionJob& J : jobs) {
        for (llvm::BasicBlock* bb : J.blocks) {
            for (llvm::Instruction& in : *bb) {
                for (unsigned k = 0; k < in.getNumOperands(); ++k)
                    if (in.getOperand(k) == placeholder) in.setOperand(k, J.iv);
                llvm::Value* ptr = nullptr;
                unsigned ptrIdx = 0;
                if (auto* ld = llvm::dyn_cast<llvm::LoadInst>(&in)) {
                    ptr = ld->getPointerOperand();
                    ptrIdx = ld->getPointerOperandIndex();
                } else if (auto* st = llvm::dyn_cast<llvm::StoreInst>(&in)) {
                    ptr = st->getPointerOperand();
                    ptrIdx = st->getPointerOperandIndex();
                }
                if (!ptr) continue;
                auto f = ctxArray.find(llvm::dyn_cast<llvm::AllocaInst>(ptr));
                if (f == ctxArray.end()) continue;
                llvm::IRBuilder<> b(&in);
                in.setOperand(ptrIdx,
                              b.CreateInBoundsGEP(f->second->getAllocatedType(),
                                                  f->second, J.iv, "ctx.p"));
            }
        }
    }

    for (auto& kv : ctxArray) kv.first->eraseFromParent();
    placeholder->eraseFromParent();
    (void) hostModule;
}

} // namespace cpu
} // namespace xpu
} // namespace cajeta
