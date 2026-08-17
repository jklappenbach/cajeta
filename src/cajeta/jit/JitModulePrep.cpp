#include "cajeta/jit/JitModulePrep.h"

#include <functional>

#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/Utils/ValueMapper.h"

namespace cajeta::jit {

void legalizeCrossModuleRefs(llvm::Module* m) {
    auto localDecl = [m](llvm::GlobalValue* gv) -> llvm::Value* {
        if (auto* fn = llvm::dyn_cast<llvm::Function>(gv)) {
            return m->getOrInsertFunction(fn->getName(),
                                          fn->getFunctionType()).getCallee();
        }
        if (auto* g = llvm::dyn_cast<llvm::GlobalVariable>(gv)) {
            if (auto* existing = m->getGlobalVariable(g->getName(), true))
                return existing;
            // Thread-locality MUST carry over: under emulated TLS a plain
            // declaration compiles to a direct load of `X`, but a TLS
            // definition only ever exports `__emutls_v.X` — the lookup then
            // fails ("Symbols not found") and the state silently splits.
            return new llvm::GlobalVariable(
                *m, g->getValueType(), g->isConstant(),
                llvm::GlobalValue::ExternalLinkage, nullptr, g->getName(),
                nullptr, g->getThreadLocalMode(), g->getAddressSpace());
        }
        return nullptr;
    };

    // Collect every foreign GlobalValue reachable from this module's
    // instructions and global initializers. Constants are uniqued per
    // context (shared across modules), so replacement must REBUILD constant
    // trees via ValueMapper rather than mutate in place.
    llvm::ValueToValueMapTy vm;
    std::function<void(llvm::Constant*)> scan = [&](llvm::Constant* c) {
        if (auto* gv = llvm::dyn_cast<llvm::GlobalValue>(c)) {
            if (gv->getParent() != m && !vm.count(gv))
                if (llvm::Value* repl = localDecl(gv)) vm[gv] = repl;
            return;
        }
        for (unsigned i = 0; i < c->getNumOperands(); ++i)
            if (auto* op = llvm::dyn_cast<llvm::Constant>(c->getOperand(i)))
                scan(op);
    };
    for (auto& F : *m)
        for (auto& BB : F)
            for (auto& I : BB)
                for (unsigned i = 0; i < I.getNumOperands(); ++i)
                    if (auto* c = llvm::dyn_cast<llvm::Constant>(I.getOperand(i)))
                        scan(c);
    for (auto& g : m->globals())
        if (g.hasInitializer()) scan(g.getInitializer());
    if (vm.empty()) return;

    for (auto& F : *m)
        for (auto& BB : F)
            for (auto& I : BB)
                llvm::RemapInstruction(&I, vm,
                    llvm::RF_IgnoreMissingLocals | llvm::RF_ReuseAndMutateDistinctMDs);
    for (auto& g : m->globals())
        if (g.hasInitializer())
            g.setInitializer(llvm::cast<llvm::Constant>(
                llvm::MapValue(g.getInitializer(), vm,
                               llvm::RF_IgnoreMissingLocals)));
}

int demoteInstantiationsToWeakODR(llvm::Module* m) {
    int demoted = 0;
    for (auto& F : *m)
        if (!F.isDeclaration() && F.getName().contains("<")
                && F.getLinkage() == llvm::GlobalValue::ExternalLinkage) {
            F.setLinkage(llvm::GlobalValue::WeakODRLinkage);
            F.setComdat(nullptr);
            ++demoted;
        }
    for (auto& g : m->globals())
        if (g.hasInitializer() && g.getName().contains("<")
                && g.getLinkage() == llvm::GlobalValue::ExternalLinkage) {
            g.setLinkage(llvm::GlobalValue::WeakODRLinkage);
            g.setComdat(nullptr);
            ++demoted;
        }
    return demoted;
}

}  // namespace cajeta::jit
