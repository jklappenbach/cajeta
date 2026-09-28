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

#include <algorithm>
#include <cstdio>
#include <cstdlib>
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
    llvm::BasicBlock* phBlock = nullptr;        // the work-item loop's preheader
};

// The loads and stores that ultimately address `base`, whether directly or
// through a GEP / bitcast chain. A distributed cooperative-matrix tile reaches
// its per-lane slot alloca ONLY through a GEP (distElemPtr), so its stores and
// loads never name the alloca directly -- the taint and the context-array steps
// below must follow the GEP or they take the tile's per-work-item accumulator
// for a uniform local and never materialize it across a barrier.
void collectMemUsers(llvm::Value* base,
                     llvm::SmallVectorImpl<llvm::LoadInst*>& loads,
                     llvm::SmallVectorImpl<llvm::StoreInst*>& stores) {
    for (llvm::User* u : base->users()) {
        if (auto* ld = llvm::dyn_cast<llvm::LoadInst>(u)) {
            if (ld->getPointerOperand() == base) loads.push_back(ld);
        } else if (auto* st = llvm::dyn_cast<llvm::StoreInst>(u)) {
            if (st->getPointerOperand() == base) stores.push_back(st);
        } else if (auto* g = llvm::dyn_cast<llvm::GetElementPtrInst>(u)) {
            if (g->getPointerOperand() == base) collectMemUsers(g, loads, stores);
        } else if (llvm::isa<llvm::BitCastInst>(u)) {
            collectMemUsers(u, loads, stores);
        }
    }
}

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
            llvm::SmallVector<llvm::LoadInst*, 8> loads;
            llvm::SmallVector<llvm::StoreInst*, 8> stores;
            collectMemUsers(a, loads, stores);        // direct AND via GEP
            bool perWorkItem = false;
            for (llvm::StoreInst* st : stores)
                if (tainted.count(st->getValueOperand())) {
                    perWorkItem = true; break;
                }
            if (!perWorkItem) continue;
            for (llvm::LoadInst* ld : loads)
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
                          llvm::Value* dynSharedBytes,
                          bool scaffoldUniformLoops) {
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
    // A wave op's RESULT is per work-item in principle (two waves receive
    // different reductions), and seeding the taint set with it was measured
    // 2026-09-26 to fix the uniform-constant accumulator (4.2.1.3) and to
    // BREAK four working shapes: XpuCpuBarrierExec.waveReduceWithBarrierBlockSum,
    // XpuCoopFromWords.lowersOnTheCpuDistributedTile,
    // XpuCoopEpilogue.waveVectorOfLaneLowersOnTheCpuDistributedTile and the
    // lane-strided-either-side-of-a-barrier probe, because a reduce result
    // staged through shared memory then became a context array and the
    // region shape changed under it. Reverted; the accumulator defect is
    // caught as a REFUSAL by the gate's vanished-wave-op check instead
    // (CpuRegistration), and the narrow taint is 4.2.1.3's remaining work.
    llvm::Value* seeds[] = {phX, phY, phZ};
    computeTaint(seeds, allocas, tainted);
    // Separately: values that carry a WAVE RESULT. Not part of the taint set
    // -- seeding taint with them was measured to change which locals become
    // context arrays and broke four working shapes -- but the re-arm in step 9
    // must leave them alone. A uniform local holding `Wave.reduceSumF32(...)`
    // is genuinely per-work-item (per wave), so re-arming it to the region's
    // entry value discards the reduce each work-item just computed: that is
    // what broke waveReduceWithBarrierBlockSum, XpuCoopFromWords,
    // XpuCoopEpilogue and the lane-strided-across-a-barrier probe
    // (measured 2026-09-26).
    // Allocas that must get a per-work-item context array whatever the taint
    // rule says: the scaffold flags below, and a wave-fed accumulator.
    llvm::SmallPtrSet<llvm::AllocaInst*, 8> forceCtx;
    llvm::SmallPtrSet<llvm::Value*, 32> waveDerived;
    {
        llvm::SmallVector<llvm::Value*, 8> wseeds;
        for (auto& bb : *wrapper)
            for (auto& in : bb)
                if (auto* c = llvm::dyn_cast<llvm::CallInst>(&in))
                    if (auto* cf = c->getCalledFunction()) {
                        llvm::StringRef n = cf->getName();
                        if (n.starts_with("__cajeta_xpu_wave_")
                            && n != "__cajeta_xpu_wave_width")
                            wseeds.push_back(c);
                    }
        if (!wseeds.empty()) computeTaint(wseeds, allocas, waveDerived);
    }
    // A local that is READ-MODIFY-WRITTEN with a wave result is per
    // work-item, and it must get a CONTEXT ARRAY rather than share one slot.
    //
    // `while (j < n) { acc = acc + Wave.reduceSumF32(1.0f); }`: each
    // work-item's own accumulator should end at trips * W. Sharing the slot
    // made it accumulate across the whole block — 2048 where 32 is right,
    // measured 2026-09-26 — and the per-region snapshot cannot fix that,
    // because a local carrying a wave result must NOT be re-read from the
    // region's entry value: each wave computed its own.
    //
    // Read-modify-written is the narrow condition, and it is what keeps this
    // from becoming the broad wave taint that was measured to break four
    // working shapes. `wsum = Wave.reduceSumF32(ss)` followed by
    // `part[lane / w] = wsum` is a wave result STORED ONCE, not accumulated:
    // it stays a plain local, and blockReduce, XpuCoopFromWords,
    // XpuCoopEpilogue and the lane-strided-across-a-barrier probe stay
    // green. (xpu-kernel-adaptor 4.2.1.3)
    for (llvm::AllocaInst* a : allocas) {
        llvm::SmallVector<llvm::LoadInst*, 8> loads;
        llvm::SmallVector<llvm::StoreInst*, 8> stores;
        collectMemUsers(a, loads, stores);
        llvm::SmallPtrSet<llvm::Value*, 8> selfLoads(loads.begin(), loads.end());
        for (llvm::StoreInst* st : stores) {
            llvm::Value* v = st->getValueOperand();
            if (!waveDerived.count(v)) continue;
            // Does the stored value depend on a load of THIS slot?
            llvm::SmallPtrSet<llvm::Value*, 16> seenV;
            llvm::SmallVector<llvm::Value*, 16> work{v};
            bool rmw = false;
            while (!work.empty() && !rmw) {
                llvm::Value* x = work.pop_back_val();
                if (!seenV.insert(x).second) continue;
                if (selfLoads.count(x)) { rmw = true; break; }
                if (auto* xi = llvm::dyn_cast<llvm::Instruction>(x))
                    for (llvm::Value* op : xi->operands()) work.push_back(op);
            }
            if (rmw) { forceCtx.insert(a); break; }
        }
    }

    // --- 4a/4b/4c. Scaffold ---------------------------------------------------
    //
    // Three things run ONCE for the block, outside the work-item loops, so the
    // work-item loop each region becomes is INNERMOST (which is the only kind
    // LoopVectorize widens, and a wave op left scalar is the width-1
    // identity): a branch the whole workgroup takes together (4a), a loop the
    // whole workgroup steps through together (4b), and -- since 2026-09-26 --
    // a loop or a branch that is per work-item, with the work-item PREDICATED
    // (4b, 4c). The last is what every wave mat-vec in cajeta-llm needs: the
    // lane-strided `b = lane >> 3; while (b < n) { ...; b += 4 }` under a
    // `row = globalIdX / 32; if (row < rows)` guard, with the wave reduce
    // after the loop (xpu-kernel-adaptor 4.2.1.1).
    //
    // Flags. A per-work-item flag lives in a context array (`forceCtx`: its
    // stores are constants, so the taint rule alone would hoist and share it)
    // and is read at the head of a region: the loop's `active` flag, cleared
    // by an exiting edge, and the arm's `mask`, the guard's condition. A
    // region inside a predicated arm runs only for the work-items whose mask
    // agrees with the arm (`blockPreds`, tested in wi.pre beside the activity
    // mask). A predicated loop runs while ANY work-item is active: the body
    // region ORs each work-item's flag into a uniform `any` slot the scaffold
    // header resets and the scaffold latch tests.
    //
    // Cuts. A divergent arm's regions have to start and end at scaffold
    // boundaries or the region before the guard would flow into them; a cut
    // is a boundary block that is not a barrier (`cutSet`), placed on the
    // edge into the arm, between the arms, and on the edge into the join.
    llvm::Type* i1 = llvm::Type::getInt1Ty(ctx);
    llvm::Type* i8b = llvm::Type::getInt8Ty(ctx);
    llvm::Constant* flag1 = llvm::ConstantInt::get(i8b, 1);
    llvm::Constant* flag0 = llvm::ConstantInt::get(i8b, 0);
    struct ArmPred { llvm::AllocaInst* mask; bool polarity; };
    llvm::DenseMap<llvm::BasicBlock*, llvm::SmallVector<ArmPred, 2>> blockPreds;
    llvm::SmallPtrSet<llvm::BasicBlock*, 8> cutSet;
    llvm::SmallPtrSet<llvm::BasicBlock*, 4> splitSet;
    llvm::SmallPtrSet<llvm::BasicBlock*, 8> uniformScaffoldHeaders;
    // The PREDICATED scaffold loops' headers, a subset of the above: the
    // region walk ranks a `ret` below such a loop's latch (step 8).
    llvm::SmallPtrSet<llvm::BasicBlock*, 8> predicatedScaffoldHeaders;
    // The block a scaffold loop continues at. NOT `Loop::getExitBlock()`,
    // which answers null as soon as the loop has two exit blocks -- and a
    // `return` inside the loop is a second exit block (a work-item leaving
    // the kernel), so the shape that motivated the whole transform has two.
    // With null the walk stopped, the code after the loop was never given a
    // region, and step 10 reported the tid-derived local it still used
    // (measured 2026-09-26 on the return-inside-a-scaffold-loop probe).
    llvm::DenseMap<llvm::BasicBlock*, llvm::BasicBlock*> scaffoldExit;
    auto makeFlag = [&](const llvm::Twine& name) {
        auto* a = eb.CreateAlloca(i8b, nullptr, name);
        allocas.push_back(a);
        forceCtx.insert(a);
        return a;
    };
    auto isTrivialRet = [](llvm::BasicBlock* b) {
        if (!llvm::isa<llvm::ReturnInst>(b->getTerminator())) return false;
        for (auto& in : *b)
            if (!in.isTerminator()) return false;
        return true;
    };
    auto blockHasWork = [](llvm::BasicBlock* b) {
        for (auto& in : *b)
            if (!in.isTerminator() && !llvm::isa<llvm::PHINode>(in)
                && !llvm::isa<llvm::AllocaInst>(in))
                return true;
        return false;
    };
    // When `to` does NOT post-dominate the level entry, the second question:
    // is the only way to miss `to` to LEAVE the kernel without doing anything
    // on the way out? That is the shape of every guard at the top of a
    // cajeta-llm kernel -- `if (row >= nH + nKv) { return; }`, or an `if`
    // around the whole body. A work-item that leaves needs no barrier and no
    // scaffold loop: step 9's activity mask keeps it out of every later
    // region. A path that computes something, skips and rejoins is real
    // divergence (4c predicates the barrier-free case; a barrier under it is
    // still declined).
    auto everyMissIsACleanExit = [&](llvm::BasicBlock* to,
                                     llvm::BasicBlock* levelEntry) {
        llvm::SmallPtrSet<llvm::BasicBlock*, 32> arrives;
        llvm::SmallVector<llvm::BasicBlock*, 16> bw{to};
        while (!bw.empty()) {
            llvm::BasicBlock* b = bw.pop_back_val();
            if (!arrives.insert(b).second) continue;
            for (llvm::BasicBlock* p : llvm::predecessors(b)) bw.push_back(p);
        }
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
    auto loopContainsBoundary = [&](llvm::Loop* L) {
        for (llvm::BasicBlock* b : boundarySet)
            if (L->contains(b)) return true;
        return false;
    };
    auto inLoopSuccOfL = [&](llvm::Loop* L) -> llvm::BasicBlock* {
        for (llvm::BasicBlock* s : llvm::successors(L->getHeader()))
            if (L->contains(s)) return s;
        return L->getHeader();
    };
    // The level a block belongs to: the deepest scaffold entry that dominates
    // it -- the kernel body, the enclosing scaffold loop's first body block,
    // a uniform split's arm, or a divergent arm's cut. "Every work-item
    // reaches X" is asked of the level, not of the whole kernel.
    auto levelEntryOf = [&](llvm::BasicBlock* b, llvm::Loop* parent,
                            llvm::DominatorTree& dt) -> llvm::BasicBlock* {
        llvm::BasicBlock* best = parent ? inLoopSuccOfL(parent) : bodyEntry;
        auto consider = [&](llvm::BasicBlock* arm) {
            if (!arm || !dt.dominates(arm, b)) return;
            if (dt.dominates(best, arm)) best = arm;
        };
        for (llvm::BasicBlock* ctl : splitSet)
            for (llvm::BasicBlock* arm : llvm::successors(ctl)) consider(arm);
        for (llvm::BasicBlock* cut : cutSet) consider(cut->getSingleSuccessor());
        return best;
    };

    // A loop 4b can scaffold, by shape alone (admission by level is asked
    // later, once the splits and cuts that define the levels exist): barrier
    // free, one latch that is not the header, one exit block that is not a
    // return (a `return` inside the loop is a work-item leaving the kernel,
    // which the region handles), every exiting branch conditional.
    // UNIFORM when every exiting condition is untainted and the only exiting
    // blocks are the header and the latch, the header-and-latch scaffold of
    // old; PREDICATED otherwise (a per-work-item trip count, or a break).
    struct LoopCand {
        llvm::BasicBlock* header; llvm::BasicBlock* latch; llvm::BasicBlock* exit;
        bool predicated;
        std::vector<llvm::BasicBlock*> blocks;
    };
    // CAJETA_XPU_DEBUG_WAVE names the loop and, when it is rejected, WHICH
    // test rejected it. "the reduce sat in a loop that was not scaffolded" is
    // the whole story behind several declines and the reason is never visible
    // from the emitted IR.
    const bool qdbg = std::getenv("CAJETA_XPU_DEBUG_WAVE") != nullptr;
    auto qname = [](llvm::BasicBlock* b) {
        return b->hasName() ? b->getName().str() : std::string("<bb>");
    };
    auto qualifyLoop = [&](llvm::Loop* L, LoopCand& out) -> bool {
        auto no = [&](const char* why) {
            if (qdbg)
                fprintf(stderr, "[wave-qual] %s: NOT scaffold — %s\n",
                        qname(L->getHeader()).c_str(), why);
            return false;
        };
        if (!scaffoldUniformLoops) return false;
        if (loopContainsBoundary(L)) return no("it holds a barrier or a region cut");
        llvm::BasicBlock* latch = L->getLoopLatch();
        if (!latch) return no("no single latch");
        if (latch == L->getHeader()) return no("one-block loop (latch is the header)");
        // A bare `return` reached from inside the loop is a WORK-ITEM
        // leaving the kernel, not the loop's exit, so it is skipped —
        // UNLESS it is the only exit there is, in which case the loop's
        // normal exit simply is the function's return. Skipping it
        // unconditionally rejected every loop that ENDS a kernel, and the
        // shared-memory reduction tail of the Id family is exactly that
        // shape: `while (r < rpw) { …reduce…; r = r + 1; }` as the last
        // statement. Four kernels lost their reduce to a work-item loop
        // that could not be innermost because of it (measured 2026-09-26
        // through [wave-qual], xpu-kernel-adaptor 4.2.1.7).
        llvm::SmallVector<llvm::BasicBlock*, 4> exits;
        L->getUniqueExitBlocks(exits);
        llvm::BasicBlock* exitBB = nullptr;
        for (llvm::BasicBlock* x : exits) {
            if (isTrivialRet(x)) continue;
            if (exitBB && exitBB != x)
                return no("more than one non-return exit block");
            exitBB = x;
        }
        if (!exitBB) {
            if (exits.size() != 1)
                return no("no exit but several bare returns");
            exitBB = exits[0];          // the loop ends the kernel
        }
        bool uniform = true;
        llvm::SmallVector<llvm::BasicBlock*, 4> exiting;
        L->getExitingBlocks(exiting);
        for (llvm::BasicBlock* xb : exiting) {
            auto* br = llvm::dyn_cast<llvm::CondBrInst>(xb->getTerminator());
            if (!br) return no("an exiting block does not end in a conditional branch");
            if (tainted.count(br->getCondition())) uniform = false;
            if (xb != L->getHeader() && xb != latch) uniform = false;
        }
        // A collective inside a PREDICATED loop would run with some lanes
        // switched off, and the cpu backend gives that no meaning: a
        // cooperative mma, a shuffle, a reduce all assume the whole wave
        // converged. Declined here, the loop stays inside a work-item region
        // and the gate refuses its wave op as left scalar, which is what
        // XpuCpuDistCoopVerb.waveOpLeftScalarIsRefused pins. That kernel
        // used to be refused only by accident: before a predicated body's
        // locals became per work-item, the shared slots kept its shuffle from
        // widening (measured 2026-09-27, when making them per work-item let
        // it register).
        if (!uniform)
            for (llvm::BasicBlock* bb : L->blocks())
                for (auto& in : *bb)
                    if (auto* c = llvm::dyn_cast<llvm::CallInst>(&in))
                        if (auto* cf = c->getCalledFunction()) {
                            llvm::StringRef n = cf->getName();
                            if (n.starts_with("__cajeta_xpu_wave_")
                                && n != "__cajeta_xpu_wave_width")
                                return no("a wave or cooperative op inside a per-work-item loop");
                        }
        if (qdbg)
            fprintf(stderr, "[wave-qual] %s: candidate, %s\n",
                    qname(L->getHeader()).c_str(),
                    uniform ? "uniform" : "PREDICATED");
        out = {L->getHeader(), latch, exitBB, !uniform,
               std::vector<llvm::BasicBlock*>(L->block_begin(), L->block_end())};
        return true;
    };
    auto candidateHeaders = [&](llvm::LoopInfo& li) {
        llvm::SmallPtrSet<llvm::BasicBlock*, 8> heads;
        for (llvm::Loop* top : li)
            for (llvm::Loop* L : llvm::depth_first(top)) {
                LoopCand c;
                if (qualifyLoop(L, c)) heads.insert(c.header);
            }
        return heads;
    };
    auto isLoopStructuralIn = [&](llvm::LoopInfo& li, llvm::BasicBlock* b) {
        llvm::Loop* L = li.getLoopFor(b);
        if (!L) return false;
        return b == L->getHeader() || b == L->getLoopLatch() || L->isLoopExiting(b);
    };
    // Reachable from `from` without first arriving at `via` or `join`.
    auto scopeOf = [&](llvm::BasicBlock* from, llvm::BasicBlock* via,
                       llvm::BasicBlock* join) {
        llvm::SmallPtrSet<llvm::BasicBlock*, 32> seen;
        llvm::SmallVector<llvm::BasicBlock*, 16> work{from};
        while (!work.empty()) {
            llvm::BasicBlock* b = work.pop_back_val();
            if (b == via || b == join || !seen.insert(b).second) continue;
            for (llvm::BasicBlock* sb : llvm::successors(b)) work.push_back(sb);
        }
        return seen;
    };

    // --- 4a. A branch the whole workgroup takes together is SCAFFOLD --------
    // `qkPrepKernel`'s `if (norm != 0) { …two barriers… }` has real work after
    // the join, so nobody leaves and the activity mask cannot help. What makes
    // it safe is the CONDITION: `norm` is a kernel parameter, so it is not in
    // the taint set, so every work-item of the block takes the same side. A
    // branch like that can run ONCE, outside the work-item loops, with each arm
    // regioned on its own — exactly what a barrier loop's header and latch
    // already do. The same holds for `if (r0 < rows) { lane-strided loop }`,
    // the wave mat-vec guard on `Workgroup.x()`: a scaffold loop under a
    // branch needs the branch to be scaffold too, or the region holding the
    // guard would collect both the way in and the way around.
    //
    // This is uniformity by PROVENANCE, which is the only kind available here.
    // A guard on `globalIdX() / 256` is uniform too when the block is 256 wide,
    // but that fact lives in the launch, not in the kernel; 4c predicates it.
    //
    // The condition travels through a one-element slot rather than an SSA edge.
    // It is computed inside the region's work-item loop (every work-item
    // computing the same value) and read by the scaffold branch after the loop
    // has exited, and a value defined in a loop does not dominate the block the
    // loop exits to. Every work-item storing the same bit makes the slot
    // trivially correct, which a context array indexed per work-item would also
    // be, only bigger.
    {
        // A loop's OWN control flow is already scaffold, and its exiting
        // branch trivially has a barrier on the in-loop side and none on the
        // way out. Splitting it tears the loop apart: measured 2026-09-20,
        // when `while (stride > 0)` became a "uniform split" and every shape
        // the activity mask had just fixed went back to being declined.
        llvm::DominatorTree preDT(*wrapper);
        llvm::LoopInfo preLI(preDT);
        llvm::PostDominatorTree prePDT(*wrapper);
        llvm::SmallPtrSet<llvm::BasicBlock*, 8> heads = candidateHeaders(preLI);
        auto ipdomOf = [&](llvm::BasicBlock* b) -> llvm::BasicBlock* {
            if (auto* nd = prePDT.getNode(b))
                if (auto* d = nd->getIDom()) return d->getBlock();
            return nullptr;
        };
        // A barrier, or a loop 4b will scaffold, INSIDE the branch's own
        // scope -- reachable from this successor without first arriving at
        // the join. One after the join belongs to the level, not to the
        // branch, and scaffolding for it would cut the work-item loops finer
        // for nothing.
        auto reachesScaffold = [&](llvm::BasicBlock* from, llvm::BasicBlock* via,
                                   llvm::BasicBlock* join) {
            for (llvm::BasicBlock* b : scopeOf(from, via, join))
                if (boundarySet.count(b) || heads.count(b)) return true;
            return false;
        };
        llvm::SmallVector<llvm::BranchInst*, 4> cands;
        for (auto& bb : *wrapper) {
            auto* br = llvm::dyn_cast<llvm::BranchInst>(bb.getTerminator());
            if (!br || !br->isConditional()) continue;
            if (tainted.count(br->getCondition())) continue;
            if (isLoopStructuralIn(preLI, &bb)) continue;
            // At least one side must hold scaffold of its own. BOTH sides
            // holding one is the case that matters most, not one to exclude:
            // `iq3xxsQ8WaveGateUpGluKernel`'s outer guard picks between two
            // separate barrier chains, and requiring "exactly one side"
            // silently left it a decline while every simpler shape passed.
            // A uniform branch with nothing scaffold in its scope is ordinary
            // control flow and belongs inside the work-item loop, where it
            // costs nothing.
            llvm::BasicBlock* join = ipdomOf(&bb);
            if (!reachesScaffold(br->getSuccessor(0), &bb, join)
                && !reachesScaffold(br->getSuccessor(1), &bb, join))
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

    // --- 4c. A per-work-item branch around a scaffold loop: a PREDICATED arm
    // `row = KernelThread.globalIdX() / 32; if (row < rows) { b = lane >> 3;
    // while (b < n) { ... } }` then the reduce. The guard is per work-item
    // by provenance, the loop under it has to be scaffold (4b), and a
    // scaffold loop under a divergent branch has no meaning unless the
    // branch itself is spread over the block: the guard's condition is
    // stored per work-item as a MASK, the arm runs as its own chain of
    // regions between two cuts, and each of those regions runs only for the
    // work-items whose mask says so. Both arms run (the second under the
    // mask's negation), one after the other, then the join. A barrier in
    // either arm is still declined below: a barrier under divergence has no
    // point at which the block is together, whatever the mask says.
    //
    // NOT ENABLED. Measured 2026-09-26 on cajeta-llm's cpu census: this fires
    // at over a thousand sites in one build, no test requires it (4b's
    // predicated LOOPS carry every shape the 53 declined kernels use), and
    // with it on, four Id kernels that HAD lowered stopped lowering -- their
    // reduce ends up inside an unscaffolded loop. A transform with that blast
    // radius and no test demanding it does not ship. The arm shapes that
    // genuinely need it are 4.2.1.3's remaining work, with a test each.
    if (scaffoldUniformLoops) {
        llvm::DominatorTree cDT(*wrapper);
        llvm::LoopInfo cLI(cDT);
        llvm::SmallPtrSet<llvm::BasicBlock*, 8> heads = candidateHeaders(cLI);
        struct Cand { llvm::BranchInst* br; unsigned depth; };
        std::vector<Cand> cands;
        for (auto& bb : *wrapper) {
            auto* br = llvm::dyn_cast<llvm::BranchInst>(bb.getTerminator());
            if (!br || !br->isConditional()) continue;
            if (!tainted.count(br->getCondition())) continue;
            if (isLoopStructuralIn(cLI, &bb)) continue;
            cands.push_back({br, cDT.getNode(&bb)->getLevel()});
        }
        // Outer guards first: an inner guard's predicate stacks on the outer's.
        std::sort(cands.begin(), cands.end(),
                  [](const Cand& a, const Cand& b) { return a.depth < b.depth; });
        for (Cand& c : cands) {
            llvm::BranchInst* br = c.br;
            llvm::BasicBlock* ctl = br->getParent();
            llvm::PostDominatorTree pdt(*wrapper);     // fresh: earlier cuts moved edges
            llvm::BasicBlock* join = nullptr;
            if (auto* nd = pdt.getNode(ctl))
                if (auto* d = nd->getIDom()) join = d->getBlock();
            if (!join || join == ctl) continue;
            llvm::BasicBlock* T = br->getSuccessor(0);
            llvm::BasicBlock* F = br->getSuccessor(1);
            if (T == join) { std::swap(T, F); }        // an `if (!c)` shape
            if (T == join) continue;
            llvm::SmallPtrSet<llvm::BasicBlock*, 32> scopeT = scopeOf(T, ctl, join);
            llvm::SmallPtrSet<llvm::BasicBlock*, 32> scopeF =
                F == join ? llvm::SmallPtrSet<llvm::BasicBlock*, 32>()
                          : scopeOf(F, ctl, join);
            bool holdsLoop = false, holdsBarrier = false;
            // Only a loop 4b CANNOT admit at its natural level needs the arm
            // spread over the block. A guard whose miss path does no work --
            // `row = globalIdX / 32; if (row < rows) { lane-strided loop }`,
            // the Id family's shape -- is already admitted by
            // everyMissIsACleanExit, so 4b scaffolds the loop with the guard
            // left as ordinary control flow inside the region, and the whole
            // kernel lowers. Predicating it as well moved the guard into the
            // work-item loop's entry test, which made LoopVectorize scalarize
            // the reduce after the join: four Id kernels that had lowered
            // stopped lowering (measured 2026-09-26 on cajeta-llm's cpu
            // census, q4kQ8IdMatVecKernel and three siblings). So this is the
            // last resort, not the first.
            bool needsArm = false;
            for (auto* sc : {&scopeT, &scopeF})
                for (llvm::BasicBlock* b : *sc) {
                    if (boundarySet.count(b)) holdsBarrier = true;
                    if (!heads.count(b)) continue;
                    holdsLoop = true;
                    llvm::Loop* L = cLI.getLoopFor(b);
                    llvm::BasicBlock* lvl =
                        levelEntryOf(b, L ? L->getParentLoop() : nullptr, cDT);
                    if (!pdt.dominates(b, lvl) && !everyMissIsACleanExit(b, lvl))
                        needsArm = true;
                }
            if (!holdsLoop || holdsBarrier || !needsArm) continue;
            for (llvm::BasicBlock* b : scopeT)
                if (scopeF.count(b))
                    unsupported("a per-work-item branch whose arms share a block "
                                "before their join");
            if (llvm::isa<llvm::PHINode>(join->front()))
                unsupported("a per-work-item branch around a loop whose join "
                            "carries a PHI");
            const bool tRet = isTrivialRet(T);
            const bool fArm = F != join;
            const bool fRet = fArm && isTrivialRet(F);
            if (tRet && (!fArm || fRet)) continue;      // nothing to predicate
            llvm::AllocaInst* mask = makeFlag("arm.mask");
            {
                llvm::IRBuilder<> mb(br);
                mb.CreateStore(mb.CreateZExt(br->getCondition(), i8b, "arm.c"), mask);
            }
            auto cutTo = [&](llvm::BasicBlock* target) {
                auto* cut = llvm::BasicBlock::Create(ctx, "wi.cut", wrapper, target);
                llvm::UncondBrInst::Create(target, cut);
                boundarySet.insert(cut);
                cutSet.insert(cut);
                return cut;
            };
            auto redirectExits = [&](llvm::SmallPtrSetImpl<llvm::BasicBlock*>& scope,
                                     llvm::BasicBlock* to) {
                for (llvm::BasicBlock* b : scope) {
                    auto* t = b->getTerminator();
                    for (unsigned sx = 0; sx < t->getNumSuccessors(); ++sx)
                        if (t->getSuccessor(sx) == join) t->setSuccessor(sx, to);
                }
            };
            llvm::BasicBlock* cutJ = cutTo(join);
            if (!tRet && (!fArm || !fRet)) {
                // Both arms real, or no else: the branch runs into the first
                // arm for every work-item; the arms select by mask.
                llvm::BasicBlock* cutT = cutTo(T);
                llvm::BasicBlock* after = fArm ? cutTo(F) : cutJ;
                redirectExits(scopeT, after);
                if (fArm) redirectExits(scopeF, cutJ);
                br->eraseFromParent();
                llvm::UncondBrInst::Create(cutT, ctl);
            } else if (tRet) {
                // The true arm returns: keep the branch, cut into the else arm.
                llvm::BasicBlock* cutF = cutTo(F);
                br->setSuccessor(1, cutF);
                redirectExits(scopeF, cutJ);
            } else {
                // The else arm returns: keep the branch, cut into the true arm.
                llvm::BasicBlock* cutT = cutTo(T);
                br->setSuccessor(0, cutT);
                redirectExits(scopeT, cutJ);
            }
            if (!tRet)
                for (llvm::BasicBlock* b : scopeT) blockPreds[b].push_back({mask, true});
            if (fArm && !fRet)
                for (llvm::BasicBlock* b : scopeF) blockPreds[b].push_back({mask, false});
        }
    }

    // --- 4b. A workgroup-uniform loop is scaffold too (wave kernels) ---------
    // The cross-lane wave ops (shuffle, reduce, ballot, scan) are scalar stubs
    // that LoopVectorize widens to the wave width, and LoopVectorize widens
    // INNERMOST loops only. A loop with no barrier in it used to be collected
    // whole into its region, so the region's work-item loop wrapped it as an
    // OUTER loop, stayed scalar, and every wave op inside ran the width-1
    // stub: a shuffle became the identity and q6kWmmaDeqMw4Kernel's mma read
    // this lane's own A element for every k (measured 2026-09-23: 512 scalar
    // shuffle calls survived in the `j` loop's region, one vector body in the
    // whole kernel). A loop the whole workgroup steps through together is
    // exactly a barrier loop without the barrier: its header and latch run
    // once for the block, its body is a region, and per-work-item values that
    // cross it ride the context arrays step 7 already builds.
    //
    // UNIFORM loop: qualification by provenance, as in 4a: every exiting
    // condition is untainted and the exits are the header's and the latch's.
    // The latch block usually ends the loop body -- `... j = j + 1; }` -- so
    // its uniform tail (the counter update, over uniform locals only) is
    // peeled into a block of its own to be the scaffold latch, and the work
    // before it stays in the body region.
    //
    // PREDICATED loop (2026-09-26): an exiting condition is per work-item
    // (`while (b < n)` with `b = lane >> 3`), or an exit is a `break`. The
    // loop becomes
    //
    //     P:      ...; active[wi] = 1                  (region before the loop)
    //     HS:     any = 0; br GATE                     (scaffold header)
    //     GATE:   if (active[wi]) goto H else SKIP     (region: the body)
    //     H..B:   the original body; an exiting edge → LEAVE, the back-edge → SKIP
    //     LEAVE:  active[wi] = 0; br SKIP
    //     SKIP:   any |= active[wi]; br BS
    //     BS:     if (any) goto HS else X              (scaffold latch)
    //
    // so the scaffold loop runs while any work-item is active and each
    // work-item runs exactly the trips it would have. The OR through the
    // uniform slot is an or-reduction over the work-item loop after mem2reg,
    // which LoopVectorize widens. The body is a region like any other.
    //
    // Either way, every work-item must enter the loop (admission by LEVEL:
    // the header post-dominates the level entry, or the only way to miss it
    // is to leave the kernel having done nothing -- the guard at the top of
    // a cajeta-llm kernel, `if (blk < blocks) { ... }` around the whole
    // body). A loop that does not qualify is left as it was; a wave op left
    // scalar inside it is refused after vectorization instead.
    if (scaffoldUniformLoops && std::getenv("CAJETA_XPU_DEBUG_WAVE"))
        fprintf(stderr, "[wave-fission] %s: scaffolding uniform loops\n",
                wrapper->getName().str().c_str());
    if (scaffoldUniformLoops) {
        llvm::DominatorTree uDT(*wrapper);
        llvm::LoopInfo uLI(uDT);
        llvm::PostDominatorTree uPDT(*wrapper);
        // May the scaffold latch run this once for the block? Only if it
        // touches no per-work-item value and no memory but uniform locals.
        auto scaffoldSafe = [&](llvm::Instruction& in) -> bool {
            if (in.isTerminator()) return true;
            if (tainted.count(&in)) return false;
            if (auto* st = llvm::dyn_cast<llvm::StoreInst>(&in))
                return llvm::isa<llvm::AllocaInst>(st->getPointerOperand())
                    && !tainted.count(st->getValueOperand());
            if (auto* ld = llvm::dyn_cast<llvm::LoadInst>(&in))
                return llvm::isa<llvm::AllocaInst>(ld->getPointerOperand());
            if (llvm::isa<llvm::CallBase>(in) || in.mayReadOrWriteMemory())
                return false;
            for (llvm::Value* op : in.operands())
                if (tainted.count(op)) return false;
            return true;
        };
        struct Qualified {
            LoopCand c;
            llvm::Instruction* splitAt;     // uniform: the latch's uniform tail
            llvm::SmallVector<llvm::BasicBlock*, 4> outsidePreds;
        };
        std::vector<Qualified> qual;
        for (llvm::Loop* top : uLI)
            for (llvm::Loop* L : llvm::depth_first(top)) {
                LoopCand c;
                if (!qualifyLoop(L, c)) continue;
                llvm::BasicBlock* levelEntry =
                    levelEntryOf(c.header, L->getParentLoop(), uDT);
                if (!uPDT.dominates(c.header, levelEntry)
                    && !everyMissIsACleanExit(c.header, levelEntry)) {
                    if (qdbg)
                        fprintf(stderr, "[wave-qual] %s: NOT scaffold — not every "
                                "work-item reaches it from level '%s'\n",
                                qname(c.header).c_str(), qname(levelEntry).c_str());
                    continue;
                }
                Qualified Q{c, nullptr, {}};
                if (!c.predicated) {
                    // The uniform tail of the latch, scanned back from its branch.
                    Q.splitAt = c.latch->getTerminator();
                    for (llvm::Instruction* in = Q.splitAt->getPrevNode(); in;
                         in = in->getPrevNode()) {
                        if (!scaffoldSafe(*in)) break;
                        Q.splitAt = in;
                    }
                    // …and every operand of the peeled run must be defined
                    // OUTSIDE the loop body, or inside the run itself. What is
                    // peeled becomes the scaffold LATCH, which runs once per
                    // trip outside the work-item loops, while the body runs
                    // inside them: an operand left in the body does not
                    // dominate its use in the latch any more.
                    //
                    // `ws = ws + w[k]; k = k + 1;` is the shape. The buffer
                    // load `w[k]` is not scaffold-safe so the backward scan
                    // stops there, but the `fadd` that consumes it IS
                    // (untainted operands, no memory effects), so the run
                    // began at the fadd and carried it into the latch, away
                    // from both of its operands. The verifier's words:
                    // "Instruction does not dominate all uses!" twice over,
                    // once per operand. Measured 2026-09-26 on
                    // moeTopK / routerTopK / rmsnormRouterTopK and two
                    // siblings — a latent defect in this peel that cause A
                    // exposed by making these loops scaffold at all, since
                    // they had declined earlier for another reason and never
                    // reached it.
                    //
                    // The run shrinks from the front until it holds. Advancing
                    // the start only removes constraints, so this terminates.
                    {
                        llvm::SmallPtrSet<llvm::BasicBlock*, 16> inLoop(
                            c.blocks.begin(), c.blocks.end());
                        bool moved = true;
                        while (moved && Q.splitAt != c.latch->getTerminator()) {
                            moved = false;
                            for (llvm::Instruction* in = Q.splitAt; in;
                                 in = in->getNextNode()) {
                                bool bad = false;
                                for (llvm::Value* op : in->operands()) {
                                    auto* oi = llvm::dyn_cast<llvm::Instruction>(op);
                                    if (!oi || !inLoop.count(oi->getParent())) continue;
                                    // Inside the peeled run is fine; anywhere
                                    // else in the loop body is not.
                                    if (oi->getParent() == c.latch
                                        && !oi->comesBefore(Q.splitAt)) continue;
                                    bad = true;
                                    break;
                                }
                                if (bad) {
                                    Q.splitAt = in->getNextNode();
                                    moved = true;
                                    break;
                                }
                                if (in->isTerminator()) break;
                            }
                        }
                    }
                } else {
                    if (llvm::isa<llvm::PHINode>(c.header->front()))
                        unsupported("a per-work-item loop whose header carries a PHI");
                    if (llvm::isa<llvm::PHINode>(c.exit->front()))
                        unsupported("a per-work-item loop whose exit carries a PHI");
                    // A value defined in the body and used AFTER the loop stops
                    // dominating its use once the gate can bypass the body:
                    // the path out becomes `gate -> skip -> BS -> exit`, which
                    // never passes through the body at all. Emitting that is
                    // invalid IR, and it does not fail the verifier here -- it
                    // crashed LLVM's LiveVariables analysis during codegen of
                    // the cajeta-llm test binary (fault addr 0x18, measured
                    // 2026-09-26), which is the worst way to learn it. Leave
                    // such a loop alone: the kernel lowers as it did before, or
                    // the gate refuses it by name.
                    // EVERY block of the loop, the header included: on the
                    // bypass path the header does not run either, so a value
                    // defined there and used after the loop breaks dominance
                    // just as a body-defined one does. Excluding the header is
                    // what let five router and top-k kernels through to emit
                    // "Instruction does not dominate all uses!" (measured
                    // 2026-09-26 by the post-fission verifier).
                    bool liveOut = false;
                    for (llvm::BasicBlock* b : c.blocks) {
                        for (llvm::Instruction& in : *b) {
                            for (llvm::User* u : in.users()) {
                                auto* ui = llvm::dyn_cast<llvm::Instruction>(u);
                                if (!ui) continue;
                                if (!L->contains(ui->getParent())) {
                                    liveOut = true; break;
                                }
                            }
                            if (liveOut) break;
                        }
                        if (liveOut) break;
                    }
                    if (liveOut) continue;
                    for (llvm::BasicBlock* pb : llvm::predecessors(c.header))
                        if (!L->contains(pb)) Q.outsidePreds.push_back(pb);
                }
                qual.push_back(std::move(Q));
            }
        // Every loop that became scaffold, outermost first (the qualification
        // walk is depth-first), for the uniform-local snapshot pass below.
        struct Scaffolded {
            llvm::BasicBlock* header;               // the SCAFFOLD header
            std::vector<llvm::BasicBlock*> body;    // blocks that run per work-item
        };
        std::vector<Scaffolded> scaffolded;
        for (Qualified& Q : qual) {
            if (Q.c.predicated) continue;
            // The latch must hold uniform code only, so it always becomes a
            // block of its own: the uniform tail when there is one, else just
            // the back-edge branch. Nested uniform loops can share one latch
            // block (`for i { for j { ... } i = i + 1 }` in one block), so a
            // later split takes the block its split point lives in NOW, and
            // a point already at a block head needs no split at all.
            llvm::BasicBlock* bb = Q.splitAt->getParent();
            if (Q.splitAt != &bb->front()) llvm::SplitBlock(bb, Q.splitAt);
            uniformScaffoldHeaders.insert(Q.c.header);
            scaffoldExit[Q.c.header] = Q.c.exit;
            scaffolded.push_back({Q.c.header, Q.c.blocks});
        }
        for (Qualified& Q : qual) {
            if (!Q.c.predicated) continue;
            llvm::BasicBlock* H = Q.c.header;
            llvm::BasicBlock* B = Q.c.latch;
            llvm::BasicBlock* X = Q.c.exit;
            llvm::SmallPtrSet<llvm::BasicBlock*, 32> body(Q.c.blocks.begin(),
                                                         Q.c.blocks.end());
            // A preheader of its own, in the region before the loop, to arm
            // the flag per work-item: the edge in may come from scaffold (a
            // barrier boundary, a split) or from more than one block.
            llvm::BasicBlock* P = llvm::SplitBlockPredecessors(
                H, Q.outsidePreds, ".act", static_cast<llvm::DominatorTree*>(nullptr));
            llvm::AllocaInst* active = makeFlag(H->getName() + ".active");
            auto* any = eb.CreateAlloca(i8b, nullptr, H->getName() + ".any");
            new llvm::StoreInst(flag1, active, P->getTerminator()->getIterator());
            auto* HS    = llvm::BasicBlock::Create(ctx, H->getName() + ".hdr", wrapper, H);
            auto* gate  = llvm::BasicBlock::Create(ctx, H->getName() + ".gate", wrapper, H);
            auto* leave = llvm::BasicBlock::Create(ctx, H->getName() + ".leave", wrapper, X);
            auto* skip  = llvm::BasicBlock::Create(ctx, H->getName() + ".skip", wrapper, X);
            auto* BS    = llvm::BasicBlock::Create(ctx, H->getName() + ".latch", wrapper, X);
            {
                llvm::IRBuilder<> b(HS);
                b.CreateStore(flag0, any);
                b.CreateBr(gate);
            }
            {
                llvm::IRBuilder<> b(gate);
                llvm::Value* a = b.CreateLoad(i8b, active, "wi.active");
                tainted.insert(a);
                b.CreateCondBr(b.CreateICmpNE(a, flag0, "wi.active.c"), H, skip);
            }
            {
                llvm::IRBuilder<> b(leave);
                b.CreateStore(flag0, active);
                b.CreateBr(skip);
            }
            {
                llvm::IRBuilder<> b(skip);
                llvm::Value* a = b.CreateLoad(i8b, active, "wi.active.end");
                tainted.insert(a);
                llvm::Value* v = b.CreateLoad(i8b, any, "wi.any");
                b.CreateStore(b.CreateOr(v, a, "wi.any.or"), any);
                b.CreateBr(BS);
            }
            {
                llvm::IRBuilder<> b(BS);
                llvm::Value* v = b.CreateLoad(i8b, any, "wi.any.v");
                b.CreateCondBr(b.CreateICmpNE(v, flag0, "wi.any.c"), HS, X);
            }
            // P feeds the scaffold header; every exiting edge of the body
            // leaves, and the back-edge continues.
            {
                auto* t = P->getTerminator();
                for (unsigned sx = 0; sx < t->getNumSuccessors(); ++sx)
                    if (t->getSuccessor(sx) == H) t->setSuccessor(sx, HS);
            }
            // EVERY edge out of the body clears this work-item's `active`
            // flag, not only the edge to the loop's exit. The scaffold loop
            // runs while ANY work-item is active, so a work-item that leaves
            // by some other door with its flag still set keeps the loop alive
            // forever. An early `return` is that other door: it goes to a bare
            // return block, which 4b deliberately does not count as the exit.
            // On 2026-09-27 that spun one cajeta-llm test for THREE HOURS on
            // all 32 cores. Termination then rested on a second mechanism —
            // step 9's activity mask taking the returning work-item out of
            // later trips — and that mask is built only when a region can be
            // left early, which a mis-ranked region walk had stopped being
            // true. Clearing the flag on the way out makes the loop terminate
            // on its own account, whatever the mask does.
            // A local STORED in a predicated body is per work-item, whatever
            // its data flow says. Work-items leave the loop at different
            // trips, so once one has left, the others' updates are not its
            // updates. The taint rule sees only data flow, and `j = j + 1`
            // or `acc = acc + 1.0f` carry no work-item id, so they stayed one
            // shared slot the work-item loop threaded from one work-item to
            // the next: every work-item after the first read the previous
            // one's `j`, the returning work-item never saw its trip, and the
            // return probe answered 40 where 35 is right (measured
            // 2026-09-27).
            //
            // Only CARRIED state, though: a local the body stores and that is
            // read either before this trip's store reaches it (the previous
            // trip's value, `j` and `acc`) or after the loop. A temporary
            // written and read within one trip is private to the work-item
            // already, and making it a context array too turned every such
            // temporary into a gather and a scatter: the cajeta-llm test
            // binary took 47 minutes to compile instead of about ten, and
            // q4kWmmaDeqMw8Kernel ran for over 20 minutes in one test
            // (measured 2026-09-27).
            {
                llvm::DominatorTree bDT(*wrapper);
                for (llvm::AllocaInst* a : allocas) {
                    llvm::SmallVector<llvm::LoadInst*, 8> ls;
                    llvm::SmallVector<llvm::StoreInst*, 8> ss;
                    collectMemUsers(a, ls, ss);
                    llvm::SmallVector<llvm::StoreInst*, 4> inBody;
                    for (llvm::StoreInst* st : ss)
                        if (body.count(st->getParent())) inBody.push_back(st);
                    if (inBody.empty()) continue;
                    bool carried = false;
                    for (llvm::LoadInst* ld : ls) {
                        if (!body.count(ld->getParent())) { carried = true; break; }
                        bool fresh = false;
                        for (llvm::StoreInst* st : inBody)
                            if (bDT.dominates(st, ld)) { fresh = true; break; }
                        if (!fresh) { carried = true; break; }
                    }
                    if (carried) forceCtx.insert(a);
                }
            }
            llvm::DenseMap<llvm::BasicBlock*, llvm::BasicBlock*> retLeave;
            auto leaveTo = [&](llvm::BasicBlock* dest) -> llvm::BasicBlock* {
                auto f = retLeave.find(dest);
                if (f != retLeave.end()) return f->second;
                auto* nb = llvm::BasicBlock::Create(
                    ctx, H->getName() + ".leave.ret", wrapper, dest);
                llvm::IRBuilder<> b(nb);
                b.CreateStore(flag0, active);
                b.CreateBr(dest);
                retLeave[dest] = nb;
                return nb;
            };
            for (llvm::BasicBlock* bb : body) {
                auto* t = bb->getTerminator();
                for (unsigned sx = 0; sx < t->getNumSuccessors(); ++sx) {
                    llvm::BasicBlock* dst = t->getSuccessor(sx);
                    if (dst == H) t->setSuccessor(sx, skip);
                    else if (dst == X) t->setSuccessor(sx, leave);
                    else if (!body.count(dst)) t->setSuccessor(sx, leaveTo(dst));
                }
            }
            (void) B;
            uniformScaffoldHeaders.insert(HS);
            predicatedScaffoldHeaders.insert(HS);
            scaffoldExit[HS] = X;
            std::vector<llvm::BasicBlock*> pbody(Q.c.blocks.begin(),
                                                 Q.c.blocks.end());
            pbody.push_back(gate);
            pbody.push_back(leave);
            pbody.push_back(skip);
            scaffolded.push_back({HS, std::move(pbody)});
        }

    }

    llvm::DominatorTree DT(*wrapper);
    llvm::LoopInfo LI(DT);

    auto loopHasBarrier = [&](llvm::Loop* L) {
        for (llvm::BasicBlock* b : boundarySet)
            if (L->contains(b)) return true;
        return false;
    };
    // Scaffold: a barrier loop, or a workgroup-uniform loop step 4b admitted.
    auto isScaffoldLoop = [&](llvm::Loop* L) {
        return loopHasBarrier(L) || uniformScaffoldHeaders.count(L->getHeader());
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
        if (cutSet.count(bar)) continue;          // a region cut, not a barrier
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
            if (!isScaffoldLoop(L)) continue;
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
        //
        // Only the WRAPPER's uses are redirected. The kernel `linked` was
        // cloned in, still names the global, and must stay untouched: when
        // a fission attempt is declined and retried on a fresh wrapper
        // (CpuRegistration), a `linked` whose operands had been pointed at
        // the dead wrapper's instructions crashed the second clone
        // (measured 2026-09-23, q4kWmmaIdMw8Kernel). The global outlives
        // `linked`; the caller sweeps it once nothing names it.
        llvm::convertUsersOfConstantsToInstructions({gv});
        llvm::Value* rep =
            eb.CreateAddrSpaceCast(buf, llvm::PointerType::get(ctx, 3));
        for (llvm::Use& u : llvm::make_early_inc_range(gv->uses()))
            if (auto* ui = llvm::dyn_cast<llvm::Instruction>(u.getUser()))
                if (ui->getFunction() == wrapper) u.set(rep);
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
                    && isScaffoldLoop(sl)) { R.subloop = s; continue; }
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
    // Where this level continues after a scaffold loop.
    auto scaffoldExitOf = [&](llvm::Loop* L) -> llvm::BasicBlock* {
        auto f = scaffoldExit.find(L->getHeader());
        if (f != scaffoldExit.end()) return f->second;
        return L->getExitBlock();
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
                if (cl != encLoop && cl->getHeader() == cur && isScaffoldLoop(cl)) {
                    walk(inLoopSucc(cl), cl, cur, nullptr);
                    pred = cur;
                    cur = scaffoldExitOf(cl);
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
            // A `ret` inside a PREDICATED scaffold loop's body ends one
            // WORK-ITEM, never the level. When the region also reaches that
            // loop's latch, the block goes on from the latch, and the
            // work-item that returned is taken out by step 9's activity mask.
            // Ranking the `ret` first there pointed the region at the function
            // exit and, since step 8b builds the mask only for a region whose
            // continuation is NOT the function exit, left no mask at all:
            // aReturnInsideAScaffoldLoopEndsThatWorkItem answered 80 for 75 at
            // width 16 (xpu-kernel-adaptor 4.2.1.9, 2026-09-27).
            //
            // ONLY there. Everywhere else the `ret` keeps its rank above the
            // latch, as it was: no other loop shape asked for the change,
            // and a `ret` reached from a plain barrier loop's body has always
            // meant "this work-item is done with the kernel".
            else if (R.reachedRet
                     && !(encLoop && R.reachedLatch
                          && predicatedScaffoldHeaders.count(encLoop->getHeader())))
                done = wrapEnd;
            else if (encLoop && R.reachedLatch) done = encLoop->getLoopLatch();
            else if (R.reachedStop) done = stopAt;
            else unsupported("unstructured barrier control flow");

            if (hasReal(R.blocks))
                jobs.push_back({cur, R.blocks, pred, done, nullptr});

            if (R.subloop) {
                llvm::Loop* L = LI.getLoopFor(R.subloop);
                walk(inLoopSucc(L), L, L->getHeader(), nullptr);  // loop body
                pred = L->getHeader();
                cur = scaffoldExitOf(L);                      // region after loop
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
        // Follow GEPs: a distributed coop tile's per-lane slot is reached only
        // through distElemPtr's GEP, so its stores/loads -- and the regions they
        // sit in -- are invisible to a direct-user scan, and the accumulator
        // that MUST cross the barrier looks like a uniform single-region local.
        llvm::SmallVector<llvm::LoadInst*, 8> loads;
        llvm::SmallVector<llvm::StoreInst*, 8> stores;
        collectMemUsers(a, loads, stores);
        bool perWorkItem = false;
        int only = -2;
        bool multi = false;
        auto noteRegion = [&](llvm::Instruction* i) {
            int rg = regionOf(i->getParent());
            if (rg >= 0) {
                if (only == -2) only = rg;
                else if (only != rg) multi = true;
            }
        };
        for (llvm::StoreInst* st : stores) {
            if (tainted.count(st->getValueOperand())) perWorkItem = true;
            noteRegion(st);
        }
        for (llvm::LoadInst* ld : loads) noteRegion(ld);
        if ((perWorkItem && multi) || forceCtx.count(a)) {
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
    llvm::Type* i8 = i8b;
    llvm::Constant* alive1 = flag1;
    llvm::Constant* alive0 = flag0;
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
        J.phBlock = ph;
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

        // Linearized work-item index, and the mask tests, in `pre`: the
        // activity mask, then each predicated arm this region sits in (4c).
        llvm::IRBuilder<> rb(pre);
        J.linear = rb.CreateAdd(
            rb.CreateAdd(rb.CreateMul(tz, ntidYX, "wi.zbase"),
                         rb.CreateMul(ty, ntidX, "wi.ybase")),
            tx, "wi.linear");
        llvm::Value* admit = nullptr;
        if (aliveArr) {
            llvm::Value* ap = rb.CreateInBoundsGEP(i8, aliveArr, J.linear,
                                                   "wi.alive.p");
            admit = rb.CreateICmpNE(rb.CreateLoad(i8, ap, "wi.alive.v"), alive0,
                                    "wi.alive.c");
        }
        auto predsIt = blockPreds.find(J.entry);
        if (predsIt != blockPreds.end())
            for (const ArmPred& ap : predsIt->second) {
                auto f = ctxArray.find(ap.mask);
                if (f == ctxArray.end())
                    unsupported("a divergent arm's mask has no context array");
                llvm::Value* mp = rb.CreateInBoundsGEP(i8, f->second, J.linear,
                                                       "wi.mask.p");
                llvm::Value* m = rb.CreateICmpNE(rb.CreateLoad(i8, mp, "wi.mask.v"),
                                                 alive0, "wi.mask.c");
                if (!ap.polarity) m = rb.CreateNot(m, "wi.mask.n");
                admit = admit ? rb.CreateAnd(admit, m, "wi.admit") : m;
            }
        if (admit) rb.CreateCondBr(admit, J.entry, xLat);
        else rb.CreateBr(J.entry);
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

    // --- 9b. Uniform locals a region UPDATES -------------------------------
    // A region runs its body once per work-item, so `j = j + 1` -- or any
    // workgroup-uniform read-modify-write -- would advance the slot ntid times
    // per region execution. Snapshot the slot ONCE per region execution (in the
    // work-item loop's preheader, outside the loop) and point the trip-start
    // READ at the snapshot: every work-item then computes from the value the
    // region was entered with and stores the same result, which makes the store
    // idempotent.
    //
    // Redirect the READ; never write the slot back. A re-arming store in the
    // work-item loop looks equivalent and is not: a work-item that SKIPS the
    // update -- `if (t == 0) { total = total + partials[i]; }`, every kernel
    // with a lane-0 tail -- would still re-arm the slot, so the last work-item
    // through the region would overwrite the value work-item 0 computed and the
    // update would be lost. Measured 2026-09-26: blockReduce answered 3968 for
    // 32640 that way, and XpuCoopFromWords, XpuCoopEpilogue and the
    // lane-strided-across-a-barrier probe failed with it.
    //
    // Per REGION, not per loop trip. Taken in the header of an enclosing
    // scaffold loop instead, an outer loop's snapshot rewinds an inner loop's
    // updates -- a uniform accumulator in nested scaffold loops answered 4
    // where 16 is right (measured 2026-09-25 at 072d0bed,
    // xpu-kernel-adaptor 4.2.1.3) -- and a region that sits in no loop at all
    // gets no snapshot, so its uniform local accumulates across the block.
    {
        llvm::DominatorTree rDT(*wrapper);
        llvm::PostDominatorTree rPDT(*wrapper);
        for (RegionJob& J : jobs) {
            if (!J.phBlock) continue;
            llvm::SmallPtrSet<llvm::BasicBlock*, 16> inRegion(J.blocks.begin(),
                                                              J.blocks.end());
            for (llvm::AllocaInst* a : allocas) {
                if (ctxArray.count(a)) continue;
                llvm::SmallVector<llvm::LoadInst*, 8> allLoads;
                llvm::SmallVector<llvm::StoreInst*, 8> allStores;
                collectMemUsers(a, allLoads, allStores);
                llvm::SmallVector<llvm::StoreInst*, 4> stores;
                llvm::SmallVector<llvm::LoadInst*, 8> loads;
                bool waveFed = false;
                for (llvm::StoreInst* st : allStores) {
                    if (!inRegion.count(st->getParent())) continue;
                    if (waveDerived.count(st->getValueOperand())) waveFed = true;
                    stores.push_back(st);
                }
                for (llvm::LoadInst* ld : allLoads)
                    if (inRegion.count(ld->getParent())) loads.push_back(ld);
                if (stores.empty() || loads.empty()) continue;
                // The update must be UNCONDITIONAL within the region. A
                // conditional one -- `if (t == 0) { total = total + p[i]; }`,
                // the lane-0 tail -- must keep reading the slot: the
                // work-items that skip it would otherwise hand the next one a
                // region-entry value that erases what the updating work-item
                // computed. Measured 2026-09-26 as three coop and wave-vector
                // suites on cpu.
                bool always = false;
                for (llvm::StoreInst* st : stores)
                    if (rPDT.dominates(st->getParent(), J.entry)) {
                        always = true; break;
                    }
                if (!always) continue;
                // A local holding a WAVE RESULT is per work-item in fact (each
                // wave computes its own), so the region-entry value is not what
                // its reads want.
                if (waveFed || waveDerived.count(a)) continue;
                auto* shadow = eb.CreateAlloca(a->getAllocatedType(), nullptr,
                                               a->getName() + ".region");
                shadow->setAlignment(a->getAlign());
                llvm::IRBuilder<> pb(J.phBlock->getTerminator());
                pb.CreateStore(pb.CreateLoad(a->getAllocatedType(), a,
                                            a->getName() + ".snap"),
                               shadow);
                // A load a region store DOMINATES sees this work-item's own
                // value and keeps reading the slot; a load no store dominates
                // is the trip-start read and takes the snapshot.
                for (llvm::LoadInst* ld : loads) {
                    bool afterStore = false;
                    for (llvm::StoreInst* st : stores)
                        if (rDT.dominates(st, ld)) { afterStore = true; break; }
                    if (!afterStore) ld->setOperand(0, shadow);
                }
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
                } else if (auto* g = llvm::dyn_cast<llvm::GetElementPtrInst>(&in)) {
                    // A coop tile's per-lane access is a GEP off the slot alloca;
                    // redirect the GEP's base to this work-item's context slice,
                    // and the load/store hanging off the GEP follows for free.
                    ptr = g->getPointerOperand();
                    ptrIdx = g->getPointerOperandIndex();
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
