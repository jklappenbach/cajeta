#include "cajeta/jit/JitModulePrep.h"

#include <functional>

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Metadata.h"
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
            // Thread-locality MUST carry over: a TLS definition exports only
            // `__emutls_v.X`, so a plain declaration's direct load never resolves.
            return new llvm::GlobalVariable(
                *m, g->getValueType(), g->isConstant(),
                llvm::GlobalValue::ExternalLinkage, nullptr, g->getName(),
                nullptr, g->getThreadLocalMode(), g->getAddressSpace());
        }
        return nullptr;
    };

    // Constants and metadata are uniqued per context, so foreign references must be
    // REBUILT through ValueMapper; mutating in place corrupts the bitcode writer.
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
    llvm::SmallPtrSet<const llvm::Metadata*, 32> mdSeen;
    std::function<void(const llvm::Metadata*)> scanMD =
        [&](const llvm::Metadata* md) {
            if (!md || !mdSeen.insert(md).second) return;
            if (auto* vam = llvm::dyn_cast<llvm::ValueAsMetadata>(md)) {
                if (auto* c = llvm::dyn_cast_or_null<llvm::Constant>(
                        vam->getValue()))
                    scan(const_cast<llvm::Constant*>(c));
                return;
            }
            if (auto* node = llvm::dyn_cast<llvm::MDNode>(md))
                for (const llvm::MDOperand& op : node->operands())
                    scanMD(op.get());
        };
    llvm::SmallVector<std::pair<unsigned, llvm::MDNode*>, 8> mds;
    for (auto& F : *m) {
        mds.clear();
        F.getAllMetadata(mds);
        for (auto& kv : mds) scanMD(kv.second);
        for (auto& BB : F)
            for (auto& I : BB) {
                for (unsigned i = 0; i < I.getNumOperands(); ++i) {
                    if (auto* c = llvm::dyn_cast<llvm::Constant>(I.getOperand(i)))
                        scan(c);
                    else if (auto* mav = llvm::dyn_cast<llvm::MetadataAsValue>(
                                 I.getOperand(i)))
                        scanMD(mav->getMetadata());
                }
                mds.clear();
                I.getAllMetadata(mds);
                for (auto& kv : mds) scanMD(kv.second);
            }
    }
    for (auto& g : m->globals()) {
        if (g.hasInitializer()) scan(g.getInitializer());
        mds.clear();
        g.getAllMetadata(mds);
        for (auto& kv : mds) scanMD(kv.second);
    }
    for (auto& nmd : m->named_metadata())
        for (const llvm::MDNode* node : nmd.operands()) scanMD(node);
    if (vm.empty()) return;

    // No RF_ReuseAndMutateDistinctMDs: distinct MDNodes are shared with the live module.
    constexpr auto flags = llvm::RF_IgnoreMissingLocals;
    for (auto& F : *m) {
        mds.clear();
        F.getAllMetadata(mds);
        for (auto& kv : mds) {
            llvm::Metadata* mapped = llvm::MapMetadata(kv.second, vm, flags);
            if (mapped != kv.second)
                F.setMetadata(kv.first, llvm::cast<llvm::MDNode>(mapped));
        }
        for (auto& BB : F)
            for (auto& I : BB)
                llvm::RemapInstruction(&I, vm, flags);
    }
    for (auto& g : m->globals()) {
        if (g.hasInitializer())
            g.setInitializer(llvm::cast<llvm::Constant>(
                llvm::MapValue(g.getInitializer(), vm, flags)));
        mds.clear();
        g.getAllMetadata(mds);
        for (auto& kv : mds) {
            llvm::Metadata* mapped = llvm::MapMetadata(kv.second, vm, flags);
            if (mapped != kv.second)
                g.setMetadata(kv.first, llvm::cast<llvm::MDNode>(mapped));
        }
    }
    for (auto& nmd : m->named_metadata())
        for (unsigned i = 0; i < nmd.getNumOperands(); ++i) {
            llvm::Metadata* mapped =
                llvm::MapMetadata(nmd.getOperand(i), vm, flags);
            if (mapped != nmd.getOperand(i))
                nmd.setOperand(i, llvm::cast<llvm::MDNode>(mapped));
        }
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
