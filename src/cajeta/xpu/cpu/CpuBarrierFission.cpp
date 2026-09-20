// CPU work-item loop fission for workgroup barriers — see header.

#include "CpuBarrierFission.h"

#include "cajeta/error/Exception.h"

#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/CFG.h"
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

    // --- 3b. One `ret` per bypass edge --------------------------------------
    // A guard at the top of a kernel — `if (row >= nH + nKv) { return; }` in
    // qkNormRowsF32, or the `if (t0 < rows && i0 < outDim)` that wraps the
    // whole body of the WMMA GEMMs — sends the work-items it turns away to the
    // SAME exit block the normal path ends at. The region walk then reaches
    // that block twice, once from the guard and once from the end of the
    // barrier chain, and declines the kernel as unstructured. That is how the
    // guarded barrier loop came to be declined by name, and accepting it
    // without this split is what regioned the join twice and crashed RAGreedy
    // on 2026-09-06.
    //
    // An exit block holding no work is not a join in any useful sense, only a
    // shared `ret`. Give each incoming edge its own and the walk sees a
    // straight chain with an early exit hanging off it. An exit block that
    // DOES hold work is left alone: duplicating it would duplicate the work,
    // and the relaxed reachability check below refuses that shape anyway.
    {
        std::vector<llvm::BasicBlock*> exits;
        for (auto& bb : *wrapper)
            if (llvm::isa<llvm::ReturnInst>(bb.getTerminator()))
                exits.push_back(&bb);
        for (llvm::BasicBlock* ex : exits) {
            bool trivial = true;
            for (auto& in : *ex)
                if (!in.isTerminator()) { trivial = false; break; }
            if (!trivial) continue;
            llvm::SmallVector<llvm::BasicBlock*, 4> preds;
            llvm::SmallPtrSet<llvm::BasicBlock*, 4> seenPred;
            for (llvm::BasicBlock* pb : llvm::predecessors(ex))
                if (seenPred.insert(pb).second) preds.push_back(pb);
            if (preds.size() < 2) continue;
            for (size_t i = 1; i < preds.size(); ++i) {
                auto* nb = llvm::BasicBlock::Create(ctx, "wi.exit", wrapper);
                llvm::ReturnInst::Create(ctx, nb);
                auto* t = preds[i]->getTerminator();
                for (unsigned sx = 0; sx < t->getNumSuccessors(); ++sx)
                    if (t->getSuccessor(sx) == ex) t->setSuccessor(sx, nb);
            }
        }
    }

    // --- 4. Analyses + uniformity guardrails --------------------------------
    // The kernel's locals: taint carriers here, context arrays in step 7.
    llvm::SmallVector<llvm::AllocaInst*, 8> allocas;
    for (auto& in : *bodyEntry)
        if (auto* a = llvm::dyn_cast<llvm::AllocaInst>(&in)) allocas.push_back(a);
    llvm::SmallPtrSet<llvm::Value*, 32> tainted;
    llvm::Value* phSeeds[] = {phX, phY, phZ};
    computeTaint(phSeeds, allocas, tainted);

    // --- 4a. A branch the whole workgroup takes together is SCAFFOLD --------
    // `qkPrepKernel`'s `if (norm != 0) { …two barriers… }` has real work after
    // the join, so nobody leaves and the activity mask cannot help. What makes
    // it safe is the CONDITION: `norm` is a kernel parameter, so it is not in
    // the taint set, so every work-item of the block takes the same side. A
    // branch like that can run ONCE, outside the work-item loops, with each arm
    // regioned on its own — exactly what a barrier loop's header and latch
    // already do.
    //
    // This is uniformity by PROVENANCE, which is the only kind available here.
    // A guard on `globalIdX() / 256` is uniform too when the block is 256 wide,
    // but that fact lives in the launch, not in the kernel; the kernels that
    // wanted it say `Workgroup.x()` instead, which needs no proof.
    //
    // The condition travels through a one-element slot rather than an SSA edge.
    // It is computed inside the region's work-item loop (every work-item
    // computing the same value) and read by the scaffold branch after the loop
    // has exited, and a value defined in a loop does not dominate the block the
    // loop exits to. Every work-item storing the same bit makes the slot
    // trivially correct, which a context array indexed per work-item would also
    // be, only bigger.
    llvm::SmallPtrSet<llvm::BasicBlock*, 4> splitSet;
    {
        llvm::Type* i1 = llvm::Type::getInt1Ty(ctx);
        // A loop's OWN control flow is already scaffold, and its exiting
        // branch trivially has a barrier on the in-loop side and none on the
        // way out. Splitting it tears the loop apart: measured 2026-09-20,
        // when `while (stride > 0)` became a "uniform split" and every shape
        // the activity mask had just fixed went back to being declined.
        llvm::DominatorTree preDT(*wrapper);
        llvm::LoopInfo preLI(preDT);
        llvm::PostDominatorTree prePDT(*wrapper);
        auto ipdomOf = [&](llvm::BasicBlock* b) -> llvm::BasicBlock* {
            if (auto* nd = prePDT.getNode(b))
                if (auto* d = nd->getIDom()) return d->getBlock();
            return nullptr;
        };
        auto isLoopStructural = [&](llvm::BasicBlock* b) {
            llvm::Loop* L = preLI.getLoopFor(b);
            if (!L) return false;
            return b == L->getHeader() || b == L->getLoopLatch()
                || L->isLoopExiting(b);
        };
        // A barrier INSIDE the branch's own scope — reachable from this
        // successor without first arriving at the join. A barrier after the
        // join belongs to the level, not to the branch, and scaffolding for
        // it would cut the work-item loops finer for nothing.
        auto reachesBarrier = [&](llvm::BasicBlock* from, llvm::BasicBlock* via,
                                  llvm::BasicBlock* join) {
            llvm::SmallPtrSet<llvm::BasicBlock*, 32> seen;
            llvm::SmallVector<llvm::BasicBlock*, 16> work{from};
            while (!work.empty()) {
                llvm::BasicBlock* b = work.pop_back_val();
                if (b == via || b == join || !seen.insert(b).second) continue;
                if (boundarySet.count(b)) return true;
                for (llvm::BasicBlock* sb : llvm::successors(b)) work.push_back(sb);
            }
            return false;
        };
        llvm::SmallVector<llvm::BranchInst*, 4> cands;
        for (auto& bb : *wrapper) {
            auto* br = llvm::dyn_cast<llvm::BranchInst>(bb.getTerminator());
            if (!br || !br->isConditional()) continue;
            if (tainted.count(br->getCondition())) continue;
            if (isLoopStructural(&bb)) continue;
            // At least one side must hold a barrier of its own. BOTH sides
            // holding one is the case that matters most, not one to exclude:
            // `iq3xxsQ8WaveGateUpGluKernel`'s outer guard picks between two
            // separate barrier chains, and requiring "exactly one side"
            // silently left it a decline while every simpler shape passed.
            // A uniform branch with no barrier in its own scope is ordinary
            // control flow and belongs inside the work-item loop, where it
            // costs nothing.
            llvm::BasicBlock* join = ipdomOf(&bb);
            if (!reachesBarrier(br->getSuccessor(0), &bb, join)
                && !reachesBarrier(br->getSuccessor(1), &bb, join))
                continue;
            cands.push_back(br);
        }
        for (llvm::BranchInst* br : cands) {
            llvm::BasicBlock* home = br->getParent();
            llvm::Value* cond = br->getCondition();
            auto* slot = eb.CreateAlloca(i1, nullptr, "uni.cond");
            llvm::BasicBlock* ctl = llvm::SplitBlock(home, br);
            new llvm::StoreInst(cond, slot, home->getTerminator()->getIterator());
            llvm::IRBuilder<> cb(br);
            br->setCondition(cb.CreateLoad(i1, slot, "uni.cond.v"));
            splitSet.insert(ctl);
        }
    }

    llvm::DominatorTree DT(*wrapper);
    llvm::LoopInfo LI(DT);

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

    // ...and when it does NOT hold, the second question: is the only way to
    // miss `to` to LEAVE the kernel without doing anything on the way out?
    //
    // That is the shape of every guard at the top of a cajeta-llm kernel —
    // `if (row >= nH + nKv) { return; }`, or an `if` around the whole body.
    // The guard is workgroup-uniform in fact (`globalIdX() / 256` with a
    // 256-wide block IS the workgroup id) but nothing here can prove it: the
    // block bound is a launch parameter, and `globalIdX()` is seeded from the
    // tid placeholder, so the condition is tainted and the barrier stops
    // post-dominating. Proving uniformity instead would take either an
    // annotation on every such kernel or a block size baked into this pass —
    // per-kernel maintenance, or a hard-coded per-device number.
    //
    // A work-item that leaves needs no barrier: step 9's activity mask keeps
    // it out of every later region, and a barrier is a point in the program
    // rather than a rendezvous of a particular set of work-items. So the
    // bypass is admitted, on one condition — it must do NO work. A path that
    // computes something, skips the barrier and rejoins is real divergence,
    // has no point at which the group is together, and is still declined.
    auto blockHasWork = [](llvm::BasicBlock* b) {
        for (auto& in : *b)
            if (!in.isTerminator() && !llvm::isa<llvm::PHINode>(in)
                && !llvm::isa<llvm::AllocaInst>(in))
                return true;
        return false;
    };
    auto everyMissIsACleanExit = [&](llvm::BasicBlock* to,
                                     llvm::BasicBlock* levelEntry) {
        // Backward from `to`: everything that can still arrive.
        llvm::SmallPtrSet<llvm::BasicBlock*, 32> arrives;
        llvm::SmallVector<llvm::BasicBlock*, 16> bw{to};
        while (!bw.empty()) {
            llvm::BasicBlock* b = bw.pop_back_val();
            if (!arrives.insert(b).second) continue;
            for (llvm::BasicBlock* p : llvm::predecessors(b)) bw.push_back(p);
        }
        // Forward from the level entry, stopping AT `to`: the span in which a
        // work-item can still be turned away.
        llvm::SmallPtrSet<llvm::BasicBlock*, 32> span;
        llvm::SmallVector<llvm::BasicBlock*, 16> fw{levelEntry};
        while (!fw.empty()) {
            llvm::BasicBlock* b = fw.pop_back_val();
            if (b == to || !span.insert(b).second) continue;
            for (llvm::BasicBlock* sb : llvm::successors(b)) fw.push_back(sb);
        }
        for (llvm::BasicBlock* b : span)
            if (!arrives.count(b) && blockHasWork(b)) return false;
        return true;
    };

    // A uniform split's arm is a level of its own: inside it, "all work-items
    // must reach the barrier" means all of them that took this arm, and they
    // all did, because the branch was uniform.
    auto deepestArmEntry = [&](llvm::BasicBlock* b) -> llvm::BasicBlock* {
        llvm::BasicBlock* best = nullptr;
        for (llvm::BasicBlock* ctl : splitSet)
            for (llvm::BasicBlock* arm : llvm::successors(ctl)) {
                if (!DT.dominates(arm, b)) continue;
                if (!best || DT.dominates(best, arm)) best = arm;
            }
        return best;
    };

    for (llvm::BasicBlock* bar : boundarySet) {
        llvm::Loop* bl = LI.getLoopFor(bar);
        llvm::BasicBlock* levelEntry = bl ? inLoopSuccOf(bl) : bodyEntry;
        if (!bl)
            if (llvm::BasicBlock* arm = deepestArmEntry(bar)) levelEntry = arm;
        if (!PDT.dominates(bar, levelEntry)
            && !everyMissIsACleanExit(bar, levelEntry))
            unsupported("a barrier under work-item-divergent control flow "
                        "(all work-items must reach every barrier, or leave "
                        "without doing anything on the way out)");
    }
    // The same rule one level out; the check above looks only INSIDE the loop.
    for (llvm::Loop* top : LI)
        for (llvm::Loop* L : llvm::depth_first(top)) {
            if (!loopHasBarrier(L)) continue;
            llvm::Loop* P = L->getParentLoop();
            llvm::BasicBlock* levelEntry = P ? inLoopSuccOf(P) : bodyEntry;
            if (!P)
                if (llvm::BasicBlock* arm = deepestArmEntry(L->getHeader()))
                    levelEntry = arm;
            if (!PDT.dominates(L->getHeader(), levelEntry)
                && !everyMissIsACleanExit(L->getHeader(), levelEntry))
                unsupported("a barrier loop under work-item-divergent control "
                            "flow (every work-item must enter a loop that holds "
                            "a barrier, or leave without doing anything on the "
                            "way out)");
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
        llvm::BasicBlock* split = nullptr;    // a uniform branch, run as scaffold
        bool reachedRet = false;
        bool reachedLatch = false;
        bool reachedStop = false;             // hit this level's join
    };
    auto collect = [&](llvm::BasicBlock* start, llvm::Loop* encLoop,
                       llvm::BasicBlock* stopAt) {
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
                if (s == stopAt) { R.reachedStop = true; continue; }
                if (boundarySet.count(s)) { R.barrier = s; continue; }
                if (splitSet.count(s)) { R.split = s; continue; }
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
    std::function<void(llvm::BasicBlock*, llvm::Loop*, llvm::BasicBlock*,
                       llvm::BasicBlock*)> walk =
        [&](llvm::BasicBlock* start, llvm::Loop* encLoop,
            llvm::BasicBlock* predBlock, llvm::BasicBlock* stopAt) {
        llvm::BasicBlock* cur = start;
        llvm::BasicBlock* pred = predBlock;
        while (cur) {
            if (cur == stopAt) return;
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
                    walk(inLoopSucc(cl), cl, cur, nullptr);
                    pred = cur;
                    cur = cl->getExitBlock();
                    continue;
                }
            Collected R = collect(cur, encLoop, stopAt);
            // Claim every block a region collects, not only its start: a block
            // two regions reach is left dangling by the second — a miscompile.
            for (llvm::BasicBlock* b : R.blocks)
                if (b != cur && !regioned.insert(b).second)
                    unsupported("unstructured barrier control flow (a block is "
                                "reached by more than one region path)");
            if (R.barrier && R.split)
                unsupported("a barrier and a workgroup-uniform branch are "
                            "reachable from the same region, so there is no "
                            "single point at which this level continues");
            llvm::BasicBlock* done = nullptr;
            if (R.barrier) done = R.barrier;
            else if (R.split) done = R.split;
            else if (R.subloop) done = R.subloop;
            else if (R.reachedRet) done = wrapEnd;
            else if (encLoop && R.reachedLatch) done = encLoop->getLoopLatch();
            else if (R.reachedStop) done = stopAt;
            else unsupported("unstructured barrier control flow");

            if (hasReal(R.blocks))
                jobs.push_back({cur, R.blocks, pred, done, nullptr});

            if (R.subloop) {
                llvm::Loop* L = LI.getLoopFor(R.subloop);
                walk(inLoopSucc(L), L, L->getHeader(), nullptr);  // loop body
                pred = L->getHeader();
                cur = L->getExitBlock();                      // region after loop
                continue;
            }
            if (R.barrier) {
                pred = R.barrier;
                cur = R.barrier->getSingleSuccessor();
                continue;
            }
            if (R.split) {
                // Scaffold: the branch runs once for the whole block. Each arm
                // is its own chain of regions, bounded by the join — the
                // branch's immediate post-dominator. When there is none (both
                // arms end the kernel, iq3xxsQ8WaveGateUpGluKernel's shape)
                // this level simply ends with the two arms.
                llvm::BasicBlock* join = nullptr;
                if (auto* node = PDT.getNode(R.split))
                    if (auto* idom = node->getIDom()) join = idom->getBlock();
                if (join == R.split) join = nullptr;
                for (llvm::BasicBlock* arm : llvm::successors(R.split))
                    if (arm != join) walk(arm, encLoop, R.split, join);
                if (!join) return;
                pred = R.split;
                cur = join;
                continue;
            }
            return;   // reached ret, the loop latch, or this level's join
        }
    };
    walk(bodyEntry, /*encLoop=*/nullptr, /*predBlock=*/trueEntry,
         /*stopAt=*/nullptr);

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

    // --- 8b. The work-item activity mask ------------------------------------
    // One byte per work-item, live for the whole kernel. A region already
    // turns a `ret` into "end this work-item's iteration" (step 9 below), but
    // before this mask the SAME work-item then ran every LATER region too, on
    // state it had never initialized — reading a row index it had just been
    // told was out of range, and writing the output row at it.
    //
    // Only materialized when a region can be left early: a kernel whose only
    // `ret` is the last region's natural end pays nothing, which is every
    // kernel that lowered before this change.
    llvm::Type* i8 = llvm::Type::getInt8Ty(ctx);
    llvm::Constant* alive1 = llvm::ConstantInt::get(i8, 1);
    llvm::Constant* alive0 = llvm::ConstantInt::get(i8, 0);
    bool canLeaveEarly = false;
    for (RegionJob& J : jobs) {
        if (J.doneTarget == wrapEnd) continue;
        for (llvm::BasicBlock* b : J.blocks)
            if (llvm::isa<llvm::ReturnInst>(b->getTerminator())) {
                canLeaveEarly = true; break;
            }
        if (canLeaveEarly) break;
    }
    llvm::AllocaInst* aliveArr = nullptr;
    if (canLeaveEarly) {
        aliveArr = eb.CreateAlloca(i8, 0, ntidAll, "wi.alive");
        aliveArr->setAlignment(llvm::Align(1));
        eb.CreateMemSet(aliveArr, alive1,
                        eb.CreateZExt(ntidAll, llvm::Type::getInt64Ty(ctx)),
                        llvm::Align(1));
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

        // EVERY edge from outside the region, not only J.predBlock's. A
        // region that starts at the join of a uniform split has one incoming
        // edge per arm, and an arm left pointing straight at J.entry would
        // skip the work-item loop entirely — a miscompile, not a diagnostic.
        llvm::SmallPtrSet<llvm::BasicBlock*, 8> inRegion(J.blocks.begin(),
                                                         J.blocks.end());
        llvm::SmallVector<llvm::BasicBlock*, 4> outside;
        for (llvm::BasicBlock* pb : llvm::predecessors(J.entry))
            if (!inRegion.count(pb)) outside.push_back(pb);
        unsigned redirected = 0;
        for (llvm::BasicBlock* pb : outside) {
            auto* pterm = pb->getTerminator();
            for (unsigned sx = 0; sx < pterm->getNumSuccessors(); ++sx)
                if (pterm->getSuccessor(sx) == J.entry) {
                    pterm->setSuccessor(sx, ph); ++redirected;
                }
            J.entry->replacePhiUsesWith(pb, ph);
        }
        if (!redirected) unsupported("region predecessor edge not found");
        // Two arms merging into one `ph` would collapse a PHI's two incoming
        // values onto one edge. Locals live in allocas here, so this does not
        // arise in practice; say so by name rather than emit invalid IR.
        if (redirected > 1 && llvm::isa<llvm::PHINode>(J.entry->front()))
            unsupported("a region entered from more than one branch arm still "
                        "carries a PHI, which the work-item loop preheader "
                        "cannot merge");
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

        // The linear index and the mask test live in their own block between
        // the loop head and the region: the index has to dominate every
        // context-array access in the region, and a work-item that left has
        // to skip the region entirely rather than enter and branch out of it.
        auto* pre  = llvm::BasicBlock::Create(ctx, "wi.pre", wrapper, J.entry);

        llvm::IRBuilder<> hb(head);                             // tid.x loop (inner)
        auto* tx = hb.CreatePHI(i32, 2, "wi.tx");
        tx->addIncoming(z0, yHd);
        hb.CreateCondBr(hb.CreateICmpSLT(tx, ntidX, "wi.cond"), pre, yLat);
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

        // Linearized work-item index, and the mask test, in `pre`.
        llvm::IRBuilder<> rb(pre);
        J.linear = rb.CreateAdd(
            rb.CreateAdd(rb.CreateMul(tz, ntidYX, "wi.zbase"),
                         rb.CreateMul(ty, ntidX, "wi.ybase")),
            tx, "wi.linear");
        if (aliveArr) {
            llvm::Value* ap = rb.CreateInBoundsGEP(i8, aliveArr, J.linear,
                                                   "wi.alive.p");
            rb.CreateCondBr(
                rb.CreateICmpNE(rb.CreateLoad(i8, ap, "wi.alive.v"), alive0,
                                "wi.alive.c"),
                J.entry, xLat);
        } else {
            rb.CreateBr(J.entry);
        }
        J.entry->replacePhiUsesWith(ph, pre);

        // Region exit edges, a `ret` included, become the inner (x) latch. A
        // `ret` also clears the mask: this work-item is done for the kernel,
        // not only for this region.
        for (llvm::BasicBlock* bb : J.blocks) {
            auto* term = bb->getTerminator();
            if (llvm::isa<llvm::ReturnInst>(term)) {
                if (aliveArr) {
                    llvm::IRBuilder<> tb(term);
                    tb.CreateStore(alive0,
                                   tb.CreateInBoundsGEP(i8, aliveArr, J.linear,
                                                        "wi.alive.off"));
                }
                term->eraseFromParent();
                llvm::UncondBrInst::Create(xLat, bb);
            } else {
                for (unsigned s = 0; s < term->getNumSuccessors(); ++s)
                    if (term->getSuccessor(s) == J.doneTarget)
                        term->setSuccessor(s, xLat);
            }
        }

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
        if (!kv.first->use_empty()) {
            std::string who = kv.first->hasName()
                ? " ('" + kv.first->getName().str() + "')" : "";
            std::string where;
            if (auto* u = llvm::dyn_cast<llvm::Instruction>(*kv.first->user_begin()))
                if (u->getParent()->hasName())
                    where = ", still used in block '"
                          + u->getParent()->getName().str() + "'";
            // A stranded local almost always means a BLOCK was stranded:
            // the walk never gave it a region, so step 10 never rewrote its
            // accesses. Naming the blocks turns "somewhere in the CFG" into
            // a place to look.
            std::string orphans;
            unsigned nOrphan = 0;
            for (auto& bb : *wrapper) {
                if (regioned.count(&bb) || boundarySet.count(&bb)) continue;
                if (splitSet.count(&bb) || &bb == trueEntry || &bb == wrapEnd)
                    continue;
                if (!hasReal({&bb})) continue;
                // Step 9's own loop nest (wi.ph / wi.*.head / wi.*.latch /
                // wi.pre / wi.exit) is scaffold by construction, never a
                // region, and listing it would bury the real answer.
                if (bb.getName().starts_with("wi.")) continue;
                if (++nOrphan <= 6)
                    orphans += (orphans.empty() ? " " : ", ")
                             + (bb.hasName() ? bb.getName().str() : "<unnamed>");
            }
            if (nOrphan)
                orphans = "; " + std::to_string(nOrphan)
                        + " block(s) with work were never given a region:"
                        + orphans;
            unsupported("a per-work-item local" + who + " is accessed in a way "
                        "barrier fission can't redirect (a derived/GEP pointer, "
                        "or a use outside a per-work-item region)" + where
                        + orphans);
        }
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
