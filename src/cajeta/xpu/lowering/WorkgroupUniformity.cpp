#include "WorkgroupUniformity.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/Transforms/Utils/Cloning.h"

namespace cajeta {
namespace xpu {

namespace {

constexpr const char* kVaryingMd = "cajeta.varying";
constexpr const char* kWorkgroupReduceMd = "cajeta.wgreduce";
constexpr int kInlineRounds = 16;

void tag(llvm::Instruction* i, const char* kind) {
    i->setMetadata(kind, llvm::MDNode::get(i->getContext(), {}));
}

bool isAtomic(const llvm::Instruction& i) {
    if (llvm::isa<llvm::AtomicRMWInst>(i) || llvm::isa<llvm::AtomicCmpXchgInst>(i)) return true;
    if (auto* c = llvm::dyn_cast<llvm::CallBase>(&i))
        if (auto* f = c->getCalledFunction())
            return f->getName().contains_insensitive("atomic");
    return false;
}

// Whether `f` or a helper it calls holds a tagged Workgroup.reduce barrier.
bool reachesWorkgroupReduce(llvm::Function& f, llvm::SmallPtrSetImpl<llvm::Function*>& seen) {
    if (!seen.insert(&f).second) return false;
    for (auto& bb : f)
        for (auto& i : bb) {
            if (i.getMetadata(kWorkgroupReduceMd)) return true;
            if (auto* c = llvm::dyn_cast<llvm::CallBase>(&i))
                if (auto* cf = c->getCalledFunction(); cf && !cf->isDeclaration())
                    if (reachesWorkgroupReduce(*cf, seen)) return true;
        }
    return false;
}

void inlineHelpers(llvm::Function& f) {
    for (int round = 0; round < kInlineRounds; ++round) {
        llvm::SmallVector<llvm::CallBase*, 16> calls;
        for (auto& bb : f)
            for (auto& i : bb)
                if (auto* c = llvm::dyn_cast<llvm::CallBase>(&i))
                    if (auto* cf = c->getCalledFunction();
                        cf && !cf->isDeclaration() && cf != &f)
                        calls.push_back(c);
        bool inlined = false;
        for (llvm::CallBase* c : calls) {
            llvm::InlineFunctionInfo info;
            inlined |= llvm::InlineFunction(*c, info).isSuccess();
        }
        if (!inlined) return;
    }
}

bool underDivergence(llvm::Function& f) {
    llvm::SmallVector<llvm::Instruction*, 4> sites;
    llvm::SmallVector<llvm::Value*, 64> work;
    llvm::DenseMap<llvm::AllocaInst*, llvm::SmallVector<llvm::LoadInst*, 4>> loadsOf;
    auto slotOf = [](llvm::Value* ptr) {
        return llvm::dyn_cast<llvm::AllocaInst>(llvm::getUnderlyingObject(ptr));
    };
    for (auto& bb : f)
        for (auto& i : bb) {
            if (i.getMetadata(kWorkgroupReduceMd)) sites.push_back(&i);
            if (i.getMetadata(kVaryingMd) || isAtomic(i)) work.push_back(&i);
            if (auto* ld = llvm::dyn_cast<llvm::LoadInst>(&i))
                if (auto* a = slotOf(ld->getPointerOperand())) loadsOf[a].push_back(ld);
        }
    if (sites.empty()) return false;
    for (llvm::Argument& a : f.args())
        if (a.hasAttribute(kVaryingMd)) work.push_back(&a);

    llvm::PostDominatorTree pdt(f);
    llvm::SmallPtrSet<llvm::Value*, 32> tainted;
    llvm::SmallPtrSet<llvm::AllocaInst*, 16> taintedSlots;
    llvm::SmallPtrSet<llvm::Instruction*, 16> divergent;
    llvm::SmallPtrSet<llvm::BasicBlock*, 32> controlled;
    auto taintSlot = [&](llvm::Value* ptr) {
        if (auto* a = slotOf(ptr); a && taintedSlots.insert(a).second)
            for (llvm::LoadInst* ld : loadsOf[a]) work.push_back(ld);
    };
    for (;;) {
        while (!work.empty()) {
            llvm::Value* v = work.pop_back_val();
            if (!tainted.insert(v).second) continue;
            for (llvm::User* u : v->users()) {
                if (auto* st = llvm::dyn_cast<llvm::StoreInst>(u)) taintSlot(st->getPointerOperand());
                else if (auto* i = llvm::dyn_cast<llvm::Instruction>(u)) work.push_back(i);
            }
        }
        bool grew = false;
        for (auto& bb : f) {
            llvm::Instruction* term = bb.getTerminator();
            llvm::Value* cond = nullptr;
            if (auto* br = llvm::dyn_cast_or_null<llvm::BranchInst>(term); br && br->isConditional())
                cond = br->getCondition();
            else if (auto* sw = llvm::dyn_cast_or_null<llvm::SwitchInst>(term))
                cond = sw->getCondition();
            if (!cond || !tainted.count(cond) || !divergent.insert(term).second) continue;
            grew = true;
            llvm::BasicBlock* join = nullptr;
            if (auto* node = pdt.getNode(&bb))
                if (auto* idom = node->getIDom()) join = idom->getBlock();
            llvm::SmallVector<llvm::BasicBlock*, 16> todo(llvm::succ_begin(&bb), llvm::succ_end(&bb));
            llvm::SmallPtrSet<llvm::BasicBlock*, 32> region;
            while (!todo.empty()) {
                llvm::BasicBlock* b = todo.pop_back_val();
                if (b == join || !region.insert(b).second) continue;
                for (llvm::BasicBlock* s : llvm::successors(b)) todo.push_back(s);
            }
            for (llvm::BasicBlock* b : region) {
                controlled.insert(b);
                for (auto& i : *b) {
                    if (auto* st = llvm::dyn_cast<llvm::StoreInst>(&i)) taintSlot(st->getPointerOperand());
                    else if (llvm::isa<llvm::PHINode>(i)) work.push_back(&i);
                }
            }
            if (join)
                for (llvm::PHINode& phi : join->phis()) work.push_back(&phi);
        }
        if (!grew && work.empty()) break;
    }
    for (llvm::Instruction* s : sites)
        if (controlled.count(s->getParent())) return true;
    return false;
}

} // namespace

void markVarying(llvm::Value* v) {
    if (auto* i = llvm::dyn_cast_or_null<llvm::Instruction>(v)) tag(i, kVaryingMd);
    else if (auto* a = llvm::dyn_cast_or_null<llvm::Argument>(v))
        a->addAttr(llvm::Attribute::get(a->getContext(), kVaryingMd));
}

void markWorkgroupReduce(llvm::Instruction* barrier) {
    if (barrier) tag(barrier, kWorkgroupReduceMd);
}

bool workgroupReduceUnderDivergence(llvm::Function& kernel) {
    llvm::SmallPtrSet<llvm::Function*, 8> seen;
    if (!reachesWorkgroupReduce(kernel, seen)) return false;
    llvm::ValueToValueMapTy vmap;
    llvm::Function* copy = llvm::CloneFunction(&kernel, vmap);
    inlineHelpers(*copy);
    bool refused = underDivergence(*copy);
    copy->eraseFromParent();
    return refused;
}

} // namespace xpu
} // namespace cajeta
