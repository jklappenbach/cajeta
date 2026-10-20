#include "SetjmpLocals.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>

#include <set>
#include <utility>
#include <vector>

namespace cajeta {

namespace {

bool callsNamed(const llvm::Instruction& i, llvm::StringRef name) {
    auto* c = llvm::dyn_cast<llvm::CallBase>(&i);
    auto* f = c ? c->getCalledFunction() : nullptr;
    return f && f->getName() == name;
}

bool returnsTwice(const llvm::Instruction& i) {
    auto* c = llvm::dyn_cast<llvm::CallBase>(&i);
    return c && c->hasFnAttr(llvm::Attribute::ReturnsTwice);
}

/** The alloca `p` addresses, through GEPs; null for any other pointer. */
llvm::AllocaInst* rootAlloca(llvm::Value* p) {
    while (auto* g = llvm::dyn_cast<llvm::GetElementPtrInst>(p)) p = g->getPointerOperand();
    return llvm::dyn_cast<llvm::AllocaInst>(p);
}

/** Allocas `i` may write: a store's target, a memory intrinsic's destination, or any
 *  alloca handed to a call. */
void writes(llvm::Instruction& i, std::set<llvm::AllocaInst*>& out) {
    if (auto* s = llvm::dyn_cast<llvm::StoreInst>(&i)) {
        if (auto* a = rootAlloca(s->getPointerOperand())) out.insert(a);
    } else if (auto* c = llvm::dyn_cast<llvm::CallBase>(&i)) {
        if (llvm::isa<llvm::LifetimeIntrinsic>(c) || llvm::isa<llvm::DbgInfoIntrinsic>(c)) return;
        for (llvm::Value* arg : c->args())
            if (arg->getType()->isPointerTy())
                if (auto* a = rootAlloca(arg)) out.insert(a);
    }
}

/** Allocas `i` may read: a load's source, or any alloca handed to a call. */
void reads(llvm::Instruction& i, std::set<llvm::AllocaInst*>& out) {
    if (auto* l = llvm::dyn_cast<llvm::LoadInst>(&i)) {
        if (auto* a = rootAlloca(l->getPointerOperand())) out.insert(a);
    } else if (llvm::isa<llvm::CallBase>(&i)) {
        writes(i, out);
    }
}

/** The successors a returns_twice call branches to on its first (zero) and second
 *  (nonzero) return; both null when its result does not feed an `== 0` / `!= 0` branch. */
std::pair<llvm::BasicBlock*, llvm::BasicBlock*> arms(llvm::CallBase* call) {
    for (llvm::User* u : call->users()) {
        auto* cmp = llvm::dyn_cast<llvm::ICmpInst>(u);
        auto* zero = cmp ? llvm::dyn_cast<llvm::ConstantInt>(cmp->getOperand(1)) : nullptr;
        if (!zero || !zero->isZero() || !cmp->isEquality()) continue;
        for (llvm::User* cu : cmp->users()) {
            auto* br = llvm::dyn_cast<llvm::BranchInst>(cu);
            if (!br || !br->isConditional()) continue;
            bool ne = cmp->getPredicate() == llvm::ICmpInst::ICMP_NE;
            return ne ? std::make_pair(br->getSuccessor(1), br->getSuccessor(0))
                      : std::make_pair(br->getSuccessor(0), br->getSuccessor(1));
        }
    }
    return {nullptr, nullptr};
}

/** Allocas written from `start` until the try frame open at it is popped; nested
 *  push/pop pairs are counted so an inner try's pop does not end the walk. */
std::set<llvm::AllocaInst*> bodyWrites(llvm::Instruction* start) {
    std::set<llvm::AllocaInst*> out;
    std::set<std::pair<llvm::BasicBlock*, int>> seen;
    std::vector<std::pair<llvm::Instruction*, int>> work{{start, 0}};
    while (!work.empty()) {
        auto [first, depth] = work.back();
        work.pop_back();
        llvm::BasicBlock* bb = first->getParent();
        bool popped = false;
        for (auto it = first->getIterator(); it != bb->end(); ++it) {
            writes(*it, out);
            if (callsNamed(*it, "__cajeta_exc_push")) ++depth;
            if (callsNamed(*it, "__cajeta_exc_pop") && --depth < 0) { popped = true; break; }
        }
        if (popped) continue;
        for (llvm::BasicBlock* s : llvm::successors(bb))
            if (seen.insert({s, depth}).second) work.push_back({&s->front(), depth});
    }
    return out;
}

/** Allocas read anywhere reachable from `from`. */
std::set<llvm::AllocaInst*> readsFrom(llvm::BasicBlock* from) {
    std::set<llvm::AllocaInst*> out;
    std::set<llvm::BasicBlock*> seen{from};
    std::vector<llvm::BasicBlock*> work{from};
    while (!work.empty()) {
        llvm::BasicBlock* bb = work.back();
        work.pop_back();
        for (llvm::Instruction& i : *bb) reads(i, out);
        for (llvm::BasicBlock* s : llvm::successors(bb))
            if (seen.insert(s).second) work.push_back(s);
    }
    return out;
}

void markVolatile(llvm::Value* p) {
    for (llvm::User* u : p->users()) {
        if (auto* l = llvm::dyn_cast<llvm::LoadInst>(u)) l->setVolatile(true);
        else if (auto* s = llvm::dyn_cast<llvm::StoreInst>(u)) { if (s->getPointerOperand() == p) s->setVolatile(true); }
        else if (auto* g = llvm::dyn_cast<llvm::GetElementPtrInst>(u)) markVolatile(g);
        else if (auto* mi = llvm::dyn_cast<llvm::MemIntrinsic>(u))
            mi->setVolatile(llvm::ConstantInt::getTrue(p->getContext()));
    }
}

void pinFunction(llvm::Function& f) {
    std::set<llvm::AllocaInst*> pin, frames;
    for (llvm::BasicBlock& bb : f)
        for (llvm::Instruction& i : bb) {
            if (!returnsTwice(i)) continue;
            auto* call = llvm::cast<llvm::CallBase>(&i);
            for (llvm::Value* arg : call->args())
                if (auto* a = rootAlloca(arg)) frames.insert(a);
            auto [body, second] = arms(call);
            std::set<llvm::AllocaInst*> w = bodyWrites(body ? &body->front() : call->getNextNode());
            std::set<llvm::AllocaInst*> r = readsFrom(second ? second : call->getParent());
            for (llvm::AllocaInst* a : w)
                if (r.count(a)) pin.insert(a);
        }
    for (llvm::AllocaInst* a : pin) {
        if (frames.count(a)) continue;
        markVolatile(a);
        if (!a->hasName()) a->setName("setjmp.pinned");
    }
}

}  // namespace

void pinSetjmpLocals(llvm::Module& m) {
    for (llvm::Function& f : m) {
        if (f.isDeclaration()) continue;
        bool any = false;
        for (llvm::BasicBlock& bb : f) {
            for (llvm::Instruction& i : bb)
                if (returnsTwice(i)) { any = true; break; }
            if (any) break;
        }
        if (any) pinFunction(f);
    }
}

}  // namespace cajeta
