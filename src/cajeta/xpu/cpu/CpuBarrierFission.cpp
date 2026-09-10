// CPU work-item loop fission for workgroup barriers — see header.

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
#include "llvm/IR/ReplaceConstant.h"
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
    llvm::Value* ivX = nullptr;                 // tid.x (inner work-item-loop IV)
    llvm::Value* ivY = nullptr;                 // tid.y (mid loop IV)
    llvm::Value* ivZ = nullptr;                 // tid.z (outer loop IV)
    llvm::Value* linear = nullptr;              // tz*ntidY*ntidX + ty*ntidX + tx
};

// Least fixpoint of the per-work-item value set over SSA def-use AND memory
// round-trips: pre-mem2reg a store/load through an alloca slot breaks the SSA
// chain, so loads from any tainted slot are tainted too. Widening is safe.
void computeTaint(llvm::ArrayRef<llvm::Value*> seeds,
                  llvm::ArrayRef<llvm::AllocaInst*> slots,
                  llvm::SmallPtrSetImpl<llvm::Value*>& tainted) {
    llvm::SmallVector<llvm::Value*, 32> work(seeds.begin(), seeds.end());
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
                          const std::vector<llvm::Value*>& nctaid,
                          llvm::Module& hostModule,
                          std::vector<llvm::UncondBrInst*>* workItemLatches,
                          llvm::Value* dynSharedBytes) {
    llvm::LLVMContext& ctx = wrapper->getContext();
    llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
    llvm::Value* ntidX = ntid[0];
    llvm::Value* ntidY = ntid[1];
    llvm::Value* ntidZ = ntid[2];

    // --- 1. True-entry + the work-item-index placeholders (tid.x/y/z) -------
    // Placeholders for the tid coords until the per-region IVs exist; tid.x
    // must stay the contiguous inner IV (no `urem`, so SIMD survives).
    auto* trueEntry = llvm::BasicBlock::Create(ctx, "entry", wrapper);
    llvm::IRBuilder<> eb(trueEntry);
    auto* phX = llvm::cast<llvm::Instruction>(
        eb.CreateFreeze(llvm::PoisonValue::get(i32), "wiid.x.ph"));
    auto* phY = llvm::cast<llvm::Instruction>(
        eb.CreateFreeze(llvm::PoisonValue::get(i32), "wiid.y.ph"));
    auto* phZ = llvm::cast<llvm::Instruction>(
        eb.CreateFreeze(llvm::PoisonValue::get(i32), "wiid.z.ph"));
    // Stride and total work-items per block, for sizing context arrays.
    llvm::Value* ntidYX  = eb.CreateMul(ntidY, ntidX, "ntid.yx");
    llvm::Value* ntidAll = eb.CreateMul(ntidYX, ntidZ, "ntid.all");

    // --- 2. Clone the per-work-item kernel body in --------------------------
    llvm::ValueToValueMapTy vmap;
    for (unsigned i = 0; i < nReal; ++i)
        vmap[linked->getArg(i)] = wrapper->getArg(i);
    vmap[linked->getArg(nReal + 0)] = phX;   // tid.x
    vmap[linked->getArg(nReal + 1)] = phY;   // tid.y
    vmap[linked->getArg(nReal + 2)] = phZ;   // tid.z
    for (unsigned d = 0; d < 3; ++d) {
        vmap[linked->getArg(nReal + 3 + d)] = ctaid[d];
        vmap[linked->getArg(nReal + 6 + d)] = ntid[d];
        vmap[linked->getArg(nReal + 9 + d)] = nctaid[d];   // gridDim (Item 6 St.2)
    }
    llvm::SmallVector<llvm::ReturnInst*, 4> returns;
    llvm::CloneFunctionInto(wrapper, linked, vmap,
                            llvm::CloneFunctionChangeType::LocalChangesOnly,
                            returns);
    llvm::BasicBlock* bodyEntry = nullptr;
    for (auto& bb : *wrapper)
        if (&bb != trueEntry) { bodyEntry = &bb; break; }

    // The entry builder must keep inserting *before* this terminator.
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
    // The kernel's locals: taint carriers here, context arrays in step 7.
    llvm::SmallVector<llvm::AllocaInst*, 8> allocas;
    for (auto& in : *bodyEntry)
        if (auto* a = llvm::dyn_cast<llvm::AllocaInst>(&in)) allocas.push_back(a);
    llvm::SmallPtrSet<llvm::Value*, 32> tainted;
    llvm::Value* phSeeds[] = {phX, phY, phZ};
    computeTaint(phSeeds, allocas, tainted);

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
                if (auto* br = llvm::dyn_cast<llvm::CondBrInst>(xb->getTerminator()))
                    if (tainted.count(br->getCondition()))
                        unsupported("a barrier in a loop with a work-item-"
                                    "dependent trip count (must be uniform)");
        }
    }
    // Post-dominating the level entry IS the block-uniformity a barrier needs.
    llvm::PostDominatorTree PDT(*wrapper);
    for (llvm::BasicBlock* bar : boundarySet) {
        llvm::Loop* bl = LI.getLoopFor(bar);
        llvm::BasicBlock* levelEntry = bl ? inLoopSuccOf(bl) : bodyEntry;
        if (!PDT.dominates(bar, levelEntry))
            unsupported("a barrier under work-item-divergent control flow "
                        "(all work-items must reach every barrier)");
    }
    // The same rule one level out; the check above looks only INSIDE the loop.
    for (llvm::Loop* top : LI)
        for (llvm::Loop* L : llvm::depth_first(top)) {
            if (!loopHasBarrier(L)) continue;
            llvm::Loop* P = L->getParentLoop();
            llvm::BasicBlock* levelEntry = P ? inLoopSuccOf(P) : bodyEntry;
            if (!PDT.dominates(L->getHeader(), levelEntry))
                unsupported("a barrier loop under work-item-divergent control "
                            "flow (every work-item must enter a loop that holds "
                            "a barrier)");
        }
    // --- 5. Per-block shared memory -----------------------------------------
    // One module-level addrspace(3) global would race across blocks, so each
    // becomes a wrapper-local buffer. Discovery must see through ConstantExprs.
    llvm::SmallPtrSet<llvm::GlobalVariable*, 4> sharedGlobals;
    llvm::SmallVector<llvm::Value*, 16> work;
    llvm::SmallPtrSet<llvm::Value*, 16> seen;
    for (auto& bb : *wrapper)
        for (auto& in : bb)
            for (llvm::Value* op : in.operands())
                if (seen.insert(op).second) work.push_back(op);
    while (!work.empty()) {
        llvm::Value* v = work.pop_back_val();
        if (auto* gv = llvm::dyn_cast<llvm::GlobalVariable>(v)) {
            if (gv->getAddressSpace() == 3) sharedGlobals.insert(gv);
        } else if (auto* ce = llvm::dyn_cast<llvm::ConstantExpr>(v)) {
            for (llvm::Value* op : ce->operands())
                if (seen.insert(op).second) work.push_back(op);
        }
    }
    for (llvm::GlobalVariable* gv : sharedGlobals) {
        llvm::AllocaInst* buf;
        if (gv->hasInitializer()) {
            // Static `shared T[N]`: a per-block [N x T] stack buffer.
            buf = eb.CreateAlloca(gv->getValueType(), 0, nullptr,
                                  gv->getName() + ".blk");
        } else {
            // Dynamic `shared T[runtimeN]`: the launch's `sharedBytes:` count.
            if (!dynSharedBytes)
                unsupported("dynamic-sized shared memory needs a runtime byte "
                            "count (the launch's sharedBytes:)");
            llvm::Value* bytes = eb.CreateZExt(
                dynSharedBytes, llvm::Type::getInt64Ty(ctx), "dyn.shared.bytes");
            buf = eb.CreateAlloca(llvm::Type::getInt8Ty(ctx), 0, bytes,
                                  gv->getName() + ".dyn");
        }
        buf->setAlignment(llvm::Align(16));
        // The replacement is an Instruction, so RAUW through a ConstantExpr
        // user would install it inside a "constant" and produce malformed IR.
        // Expand constant users first; then every use is an operand.
        llvm::convertUsersOfConstantsToInstructions({gv});
        gv->replaceAllUsesWith(
            eb.CreateAddrSpaceCast(buf, llvm::PointerType::get(ctx, 3)));
        if (gv->use_empty()) gv->eraseFromParent();
    }

    // --- 6. Identify regions (loop-aware structured walk) -------------------
    // A uniform loop stays the scalar scaffold, its body region wrapped inside.
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
                // A latch is scaffold: never in a region, never a region start.
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

    // Every block starts at most ONE region: a CFG that comes back around to a
    // regioned block is unstructured, and without this guard the walk appends a
    // RegionJob per lap until it exhausts memory. Throw, never return silently.
    llvm::SmallPtrSet<llvm::BasicBlock*, 32> regioned;
    std::function<void(llvm::BasicBlock*, llvm::Loop*, llvm::BasicBlock*)> walk =
        [&](llvm::BasicBlock* start, llvm::Loop* encLoop,
            llvm::BasicBlock* predBlock) {
        llvm::BasicBlock* cur = start;
        llvm::BasicBlock* pred = predBlock;
        while (cur) {
            // A uniform latch after the last barrier ends this level with no
            // region job; a per-work-item one has no context, so decline it.
            if (encLoop && cur == encLoop->getLoopLatch()) {
                for (llvm::Instruction& in : *cur)
                    if (!in.isTerminator() && tainted.count(&in)) {
                        std::string where = cur->hasName()
                            ? " (" + cur->getName().str() + ")" : "";
                        unsupported("per-work-item code in the latch of a barrier "
                                    "loop, after its last barrier" + where);
                    }
                return;
            }
            if (!regioned.insert(cur).second)
                unsupported("unstructured barrier control flow (a block is "
                            "reached by more than one region path)");
            // A barrier-subloop header with no separating preheader: enter it
            // as a subloop rather than letting `collect` flatten it.
            if (llvm::Loop* cl = LI.getLoopFor(cur))
                if (cl != encLoop && cl->getHeader() == cur && loopHasBarrier(cl)) {
                    walk(inLoopSucc(cl), cl, cur);
                    pred = cur;
                    cur = cl->getExitBlock();
                    continue;
                }
            Collected R = collect(cur, encLoop);
            // Claim every block a region collects, not only its start: a block
            // two regions reach is left dangling by the second — a miscompile.
            for (llvm::BasicBlock* b : R.blocks)
                if (b != cur && !regioned.insert(b).second)
                    unsupported("unstructured barrier control flow (a block is "
                                "reached by more than one region path)");
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
            auto* arr = eb.CreateAlloca(a->getAllocatedType(), 0, ntidAll,
                                        a->getName() + ".ctx");
            arr->setAlignment(a->getAlign());
            ctxArray[a] = arr;
        }
    }

    // --- 8. Hoist remaining allocas to the true entry -----------------------
    for (llvm::AllocaInst* a : allocas) {
        if (ctxArray.count(a)) continue;
        a->moveBefore(phX->getIterator());
    }

    // --- 9. Wrap each region in a 3-D work-item loop nest -------------------
    // The inner tid.x loop is the vectorizable one; a 1-D block is one SIMD
    // loop with two single-trip loops around it.
    llvm::Constant* z0 = llvm::ConstantInt::get(i32, 0);
    llvm::Constant* one = llvm::ConstantInt::get(i32, 1);
    for (RegionJob& J : jobs) {
        auto* ph   = llvm::BasicBlock::Create(ctx, "wi.ph", wrapper, J.entry);
        auto* zHd  = llvm::BasicBlock::Create(ctx, "wi.z.head", wrapper, J.entry);
        auto* yHd  = llvm::BasicBlock::Create(ctx, "wi.y.head", wrapper, J.entry);
        auto* head = llvm::BasicBlock::Create(ctx, "wi.head", wrapper, J.entry);
        auto* xLat = llvm::BasicBlock::Create(ctx, "wi.latch", wrapper, J.entry);
        auto* yLat = llvm::BasicBlock::Create(ctx, "wi.y.latch", wrapper, J.entry);
        auto* zLat = llvm::BasicBlock::Create(ctx, "wi.z.latch", wrapper, J.entry);

        auto* pterm = J.predBlock->getTerminator();
        bool redirected = false;
        for (unsigned s = 0; s < pterm->getNumSuccessors(); ++s)
            if (pterm->getSuccessor(s) == J.entry) {
                pterm->setSuccessor(s, ph); redirected = true;
            }
        if (!redirected) unsupported("region predecessor edge not found");
        llvm::UncondBrInst::Create(zHd, ph);

        llvm::IRBuilder<> zb(zHd);                              // tid.z loop
        auto* tz = zb.CreatePHI(i32, 2, "wi.tz");
        tz->addIncoming(z0, ph);
        zb.CreateCondBr(zb.CreateICmpSLT(tz, ntidZ, "wi.z.cond"), yHd,
                        J.doneTarget);

        llvm::IRBuilder<> yb(yHd);                              // tid.y loop
        auto* ty = yb.CreatePHI(i32, 2, "wi.ty");
        ty->addIncoming(z0, zHd);
        yb.CreateCondBr(yb.CreateICmpSLT(ty, ntidY, "wi.y.cond"), head, zLat);

        llvm::IRBuilder<> hb(head);                             // tid.x loop (inner)
        auto* tx = hb.CreatePHI(i32, 2, "wi.tx");
        tx->addIncoming(z0, yHd);
        hb.CreateCondBr(hb.CreateICmpSLT(tx, ntidX, "wi.cond"), J.entry, yLat);
        J.ivX = tx; J.ivY = ty; J.ivZ = tz;

        llvm::IRBuilder<> xlb(xLat);                            // inner back-edge
        tx->addIncoming(xlb.CreateAdd(tx, one, "wi.next"), xLat);
        auto* latchBr = xlb.CreateBr(head);
        if (workItemLatches) workItemLatches->push_back(latchBr);

        llvm::IRBuilder<> ylb(yLat);
        ty->addIncoming(ylb.CreateAdd(ty, one, "wi.y.next"), yLat);
        ylb.CreateBr(yHd);

        llvm::IRBuilder<> zlb(zLat);
        tz->addIncoming(zlb.CreateAdd(tz, one, "wi.z.next"), zLat);
        zlb.CreateBr(zHd);

        // Region exit edges, a `ret` included, become the inner (x) latch.
        for (llvm::BasicBlock* bb : J.blocks) {
            auto* term = bb->getTerminator();
            if (llvm::isa<llvm::ReturnInst>(term)) {
                term->eraseFromParent();
                llvm::UncondBrInst::Create(xLat, bb);
            } else {
                for (unsigned s = 0; s < term->getNumSuccessors(); ++s)
                    if (term->getSuccessor(s) == J.doneTarget)
                        term->setSuccessor(s, xLat);
            }
        }

        // Linearized work-item index; its base hoists out of the x loop.
        llvm::IRBuilder<> rb(&*J.entry->getFirstInsertionPt());
        J.linear = rb.CreateAdd(
            rb.CreateAdd(rb.CreateMul(tz, ntidYX, "wi.zbase"),
                         rb.CreateMul(ty, ntidX, "wi.ybase")),
            tx, "wi.linear");
    }

    // --- 10. Rewrite per region: tid placeholders + context-array indexing --
    for (RegionJob& J : jobs) {
        for (llvm::BasicBlock* bb : J.blocks) {
            for (llvm::Instruction& in : *bb) {
                for (unsigned k = 0; k < in.getNumOperands(); ++k) {
                    llvm::Value* op = in.getOperand(k);
                    if (op == phX) in.setOperand(k, J.ivX);
                    else if (op == phY) in.setOperand(k, J.ivY);
                    else if (op == phZ) in.setOperand(k, J.ivZ);
                }
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
                                                  f->second, J.linear, "ctx.p"));
            }
        }
    }

    for (auto& kv : ctxArray) {
        // The redirect above rewrites only direct Load/Store inside J.blocks, so
        // erasing an alloca a GEP/bitcast user still names leaves dangling IR.
        if (!kv.first->use_empty())
            unsupported("a per-work-item local is accessed in a way barrier "
                        "fission can't redirect (a derived/GEP pointer, or a "
                        "use outside a per-work-item region)");
        kv.first->eraseFromParent();
    }
    phX->eraseFromParent();
    phY->eraseFromParent();
    phZ->eraseFromParent();
    (void) hostModule;
}

} // namespace cpu
} // namespace xpu
} // namespace cajeta
