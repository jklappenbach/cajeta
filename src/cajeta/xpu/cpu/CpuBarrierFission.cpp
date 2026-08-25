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
    // 3-D blocks: each region's work-item loop is a tid.z→tid.y→tid.x nest, so
    // tid.x stays the contiguous inner IV (no `urem`, SIMD preserved). The three
    // placeholders stand in for the tid coords until the per-region IVs exist.
    auto* trueEntry = llvm::BasicBlock::Create(ctx, "entry", wrapper);
    llvm::IRBuilder<> eb(trueEntry);
    auto* phX = llvm::cast<llvm::Instruction>(
        eb.CreateFreeze(llvm::PoisonValue::get(i32), "wiid.x.ph"));
    auto* phY = llvm::cast<llvm::Instruction>(
        eb.CreateFreeze(llvm::PoisonValue::get(i32), "wiid.y.ph"));
    auto* phZ = llvm::cast<llvm::Instruction>(
        eb.CreateFreeze(llvm::PoisonValue::get(i32), "wiid.z.ph"));
    // y*x stride + total work-items per block, for context arrays sized by the
    // whole 3-D block and indexed by a linearized work-item index.
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
    // Discovery must see THROUGH ConstantExprs: a slice base (`arr[k]` with a
    // constant k, epilogue-verb SLICE args) folds to a ConstantExpr GEP over
    // the global, so the instruction operand is the CE, not the global — a
    // shared array referenced only that way would silently keep ONE module
    // global across all blocks (a cross-block race).
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
            // Dynamic `shared T[runtimeN]` (an external unsized addrspace(3)
            // global): alloca the launch's `sharedBytes:` count of bytes per
            // block. The kernel's typed GEPs index this byte buffer unchanged.
            if (!dynSharedBytes)
                unsupported("dynamic-sized shared memory needs a runtime byte "
                            "count (the launch's sharedBytes:)");
            llvm::Value* bytes = eb.CreateZExt(
                dynSharedBytes, llvm::Type::getInt64Ty(ctx), "dyn.shared.bytes");
            buf = eb.CreateAlloca(llvm::Type::getInt8Ty(ctx), 0, bytes,
                                  gv->getName() + ".dyn");
        }
        buf->setAlignment(llvm::Align(16));
        // The replacement is an INSTRUCTION, and gv may be used inside
        // ConstantExprs (the folded slice GEP above). RAUW through a constant
        // user re-uniques the constant with `cast<Constant>(New)` — in a
        // release build that blindly installs the Instruction pointer INSIDE
        // the "constant", i.e. malformed IR that InstCombine later walks into
        // a nondeterministic SIGSEGV (the XpuCoopEpilogue parallel-suite
        // crash). Expand every constant user to instructions first; then all
        // uses are instruction operands and the RAUW is well-defined.
        llvm::convertUsersOfConstantsToInstructions({gv});
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

    // Every block starts at most ONE region. `collect` already guards its own
    // BFS with a `seen` set; this walk had no equivalent, so a CFG that comes
    // back around to a block already regioned kept going — appending a
    // RegionJob per lap, forever. `samples/profile` built with
    // `--xpu-backend=cpu` reached a 67,108,864-element `jobs` vector and 116 GB
    // of RSS before the OOM killer took the compiler (kernel log:
    // "Out of memory: Killed process (cajeta) total-vm:176864816kB").
    //
    // A revisit means the barrier control flow is not the structured sequence
    // the region model assumes, which is precisely the case this pass is
    // supposed to DECLINE: `emitKernelRegistration` already wraps the call in
    // `try { fissionBarrierKernel(...) } catch (cajeta::Exception&)` and falls
    // back to a host stub. That fallback was simply unreachable for this shape,
    // because the walk never got around to throwing — it ran out of memory
    // first. Raising `unsupported` here is what connects the two.
    //
    // Deliberately NOT a silent `return`: dropping the region would leave the
    // kernel half-fissioned, and this pass's rule is never to miscompile.
    llvm::SmallPtrSet<llvm::BasicBlock*, 32> regioned;
    std::function<void(llvm::BasicBlock*, llvm::Loop*, llvm::BasicBlock*)> walk =
        [&](llvm::BasicBlock* start, llvm::Loop* encLoop,
            llvm::BasicBlock* predBlock) {
        llvm::BasicBlock* cur = start;
        llvm::BasicBlock* pred = predBlock;
        while (cur) {
            if (!regioned.insert(cur).second)
                unsupported("unstructured barrier control flow (a block is "
                            "reached by more than one region path)");
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
    // Per region: a tid.z → tid.y → tid.x nest. The inner tid.x loop is the
    // contiguous, vectorizable one (its latch is forced to VF=W for waves), so a
    // 1-D block (ntid.y/z == 1) is the same single SIMD loop as before; the z/y
    // loops are single-trip. The region body sits inside the x loop.
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

        // pred → preheader (redirect the specific edge pred → entry)
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

        // region exit edge(s) (to doneTarget, or a ret) → the inner (x) latch
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

        // Linearized work-item index for context-array accesses:
        // tz*ntidYX + ty*ntidX + tx. Built at the region entry (after its phis);
        // the tz*ntidYX + ty*ntidX base is loop-invariant in the x loop and
        // hoists out, leaving `base + tx` in the inner loop (SIMD-friendly).
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
                // Index the per-work-item context array by the linearized index.
                in.setOperand(ptrIdx,
                              b.CreateInBoundsGEP(f->second->getAllocatedType(),
                                                  f->second, J.linear, "ctx.p"));
            }
        }
    }

    for (auto& kv : ctxArray) {
        // H17: the redirect above only rewrites direct Load/Store whose pointer is
        // the alloca, and only inside J.blocks. A GEP/bitcast user, or a Load/Store
        // in a non-job (loop-structural) block, is left pointing at the alloca —
        // erasing it then leaves dangling IR (assert in debug, UAF/miscompile in
        // release). Reject cleanly instead, mirroring the use_empty() guard above.
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
