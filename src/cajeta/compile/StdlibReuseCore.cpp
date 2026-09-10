// See StdlibReuseCore.h; comments here sit with the machinery they guard.

#include "cajeta/compile/StdlibReuseCore.h"

#include <vector>

#include <llvm/IR/GlobalAlias.h>
#include <llvm/IR/GlobalIFunc.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Module.h>

#include "cajeta/compile/Compiler.h"
#include "cajeta/error/Diagnostics.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/xref/XrefIndex.h"

namespace cajeta {

    StdlibReuseCore& StdlibReuseCore::instance() {
        static StdlibReuseCore core;
        return core;
    }

    void StdlibReuseCore::ensurePrimed() {
        if (isPrimed) return;
        Compiler::setSharedContext(&sharedContext);
        // Template members are recordable only AS the stdlib parses.
        xref::resetCapture();
        xref::setCaptureEnabled(true);
        prime = std::make_unique<Compiler>();
        prime->ensureStdlibModule();   // front-end: parse + prototype layout
        xref::setCaptureEnabled(false);
        stdlibModule = CajetaModule::getStdlibModule();
        captureBaselines();
        isPrimed = true;
    }

    void StdlibReuseCore::ensureCodegenLayer(
            const std::function<void(Compiler&)>& layer) {
        ensurePrimed();
        if (isCodegenLayered) return;
        // A front-end-only user may have run requests; return to pristine first.
        restoreBaseline();
        Compiler::setSharedContext(&sharedContext);
        layer(*prime);
        captureBaselines();
        isCodegenLayered = true;
    }

    void StdlibReuseCore::captureBaselines() {
        CajetaType::captureBaseline();
        CajetaModule::captureBaseline();
        baselineStructures = stdlibModule->getStructures();
        // Snapshot the module-bound bindings a reusing run must be reset out of.
        for (auto& [canon, klass] : baselineStructures)
            if (klass) klass->captureReuseBaseline();
        captureLlvmBaseline();
        xref::captureBaseline();
    }

    // --- the stdlib llvm::Module's own baseline -----------------------------
    // Snapshots the persistent stdlib module's global values, which the restores
    // above do not touch though a session adds declarations to them.
    void StdlibReuseCore::captureLlvmBaseline() {
        baselineLlvmValues.clear();
        llvm::Module* m = stdlibModule ? stdlibModule->getLlvmModule() : nullptr;
        if (!m) return;
        for (llvm::GlobalValue& gv : m->global_values())
            baselineLlvmValues.insert(&gv);
    }

    // Every GlobalValue a constant expression reaches, however nested.
    static void collectReferencedGlobals(llvm::Constant* c,
                                         std::set<llvm::GlobalValue*>& out) {
        if (!c) return;
        if (auto* gv = llvm::dyn_cast<llvm::GlobalValue>(c)) { out.insert(gv); return; }
        for (llvm::Value* op : c->operands())
            if (auto* opc = llvm::dyn_cast<llvm::Constant>(op))
                collectReferencedGlobals(opc, out);
    }

    // The global values REFERRING to `v`, however deeply nested.
    static void collectReferringGlobals(llvm::Value* v,
                                        std::set<llvm::GlobalValue*>& out) {
        for (llvm::User* u : v->users()) {
            if (auto* inst = llvm::dyn_cast<llvm::Instruction>(u)) {
                if (inst->getFunction()) out.insert(inst->getFunction());
            } else if (auto* gv = llvm::dyn_cast<llvm::GlobalValue>(u)) {
                out.insert(gv);
            } else if (llvm::isa<llvm::Constant>(u)) {
                collectReferringGlobals(u, out);
            }
        }
    }

    // Prunes an appending global to entries whose referents all survive; the
    // pruned array retypes it, so the global is replaced under the same name.
    void StdlibReuseCore::pruneAppendingGlobal(
            llvm::Module& m, const char* name,
            const std::set<llvm::GlobalValue*>& surviving) {
        llvm::GlobalVariable* gv = m.getNamedGlobal(name);
        if (!gv || !gv->hasInitializer()) return;
        // The caller erases a session-added list itself; freeing it here too
        // leaves that caller dereferencing a freed pointer.
        if (!baselineLlvmValues.count(gv)) return;
        auto* init = llvm::dyn_cast<llvm::ConstantArray>(gv->getInitializer());
        if (!init) return;                           // empty (zeroinitializer)

        std::vector<llvm::Constant*> kept;
        for (unsigned i = 0; i < init->getNumOperands(); ++i) {
            llvm::Constant* entry = init->getOperand(i);
            std::set<llvm::GlobalValue*> referenced;
            collectReferencedGlobals(entry, referenced);
            bool allSurvive = true;
            for (llvm::GlobalValue* ref : referenced)
                if (!surviving.count(ref)) { allSurvive = false; break; }
            if (allSurvive) kept.push_back(entry);
        }
        if (kept.size() == init->getNumOperands()) return;   // nothing to drop

        auto* elemTy = init->getType()->getElementType();
        auto* newTy = llvm::ArrayType::get(elemTy, kept.size());
        auto* replacement = new llvm::GlobalVariable(
            m, newTy, gv->isConstant(), gv->getLinkage(),
            llvm::ConstantArray::get(newTy, kept));
        replacement->copyAttributesFrom(gv);
        gv->setInitializer(nullptr);
        baselineLlvmValues.erase(gv);
        gv->eraseFromParent();
        replacement->setName(name);
        baselineLlvmValues.insert(replacement);
    }

    // Gives back what the session added, by reachability rather than authorship:
    // an added value the BASELINE still refers to has been adopted and stays.
    void StdlibReuseCore::restoreLlvmBaseline() {
        llvm::Module* m = stdlibModule ? stdlibModule->getLlvmModule() : nullptr;
        if (!m) return;
        std::vector<llvm::GlobalValue*> added;
        for (llvm::GlobalValue& gv : m->global_values())
            if (!baselineLlvmValues.count(&gv)) added.push_back(&gv);
        if (added.empty()) return;

        // A prior session's constants stay uniqued, so nothing looks unused yet.
        for (llvm::GlobalValue* gv : added) gv->removeDeadConstantUsers();

        // The appending globals are deliberately NOT roots below: they are
        // lists, so a session's own entry would make its values look adopted.
        std::set<llvm::GlobalValue*> addedSet(added.begin(), added.end());
        std::set<llvm::GlobalValue*> appending;
        for (const char* name : {"llvm.global_ctors", "llvm.global_dtors",
                                 "llvm.used", "llvm.compiler.used"})
            if (auto* gv = m->getNamedGlobal(name)) appending.insert(gv);

        std::set<llvm::GlobalValue*> adopted;
        bool grew = true;
        while (grew) {
            grew = false;
            for (llvm::GlobalValue* gv : added) {
                if (adopted.count(gv)) continue;
                std::set<llvm::GlobalValue*> referrers;
                collectReferringGlobals(gv, referrers);
                for (llvm::GlobalValue* r : referrers) {
                    if (r == gv || appending.count(r)) continue;
                    if (!addedSet.count(r) || adopted.count(r)) {
                        adopted.insert(gv);
                        grew = true;
                        break;
                    }
                }
            }
        }

        std::set<llvm::GlobalValue*> surviving = baselineLlvmValues;
        surviving.insert(adopted.begin(), adopted.end());
        for (const char* name : {"llvm.global_ctors", "llvm.global_dtors",
                                 "llvm.used", "llvm.compiler.used"})
            pruneAppendingGlobal(*m, name, surviving);

        std::vector<llvm::GlobalValue*> doomed;
        for (llvm::GlobalValue* gv : added)
            if (!adopted.count(gv)) doomed.push_back(gv);

        for (llvm::GlobalValue* gv : doomed) {
            if (auto* fn = llvm::dyn_cast<llvm::Function>(gv)) {
                if (!fn->isDeclaration()) fn->deleteBody();
            } else if (auto* var = llvm::dyn_cast<llvm::GlobalVariable>(gv)) {
                if (var->hasInitializer()) var->setInitializer(nullptr);
            } else if (auto* alias = llvm::dyn_cast<llvm::GlobalAlias>(gv)) {
                alias->setAliasee(nullptr);
            } else if (auto* ifunc = llvm::dyn_cast<llvm::GlobalIFunc>(gv)) {
                ifunc->setResolver(nullptr);
            }
        }
        // To fixpoint: a doomed value can hold the last use of another.
        size_t stillUsed = 0;
        bool progress = true;
        while (progress) {
            progress = false;
            stillUsed = 0;
            for (llvm::GlobalValue*& gv : doomed) {
                if (!gv) continue;
                gv->removeDeadConstantUsers();
                if (!gv->use_empty()) { ++stillUsed; continue; }
                gv->eraseFromParent();
                gv = nullptr;
                progress = true;
            }
        }
        if (stillUsed > 0) {
            // Unreachable by construction, and reported rather than force-erased
            // because a dangling use is a verifier failure at launch.
            std::ostringstream msg;
            msg << "cajeta: resident stdlib kept " << stillUsed
                << " session-added symbol(s) that could not be released\n";
            cajeta::logLine("warn", msg.str());
        }
    }

    void StdlibReuseCore::restoreBaseline() {
        if (!isPrimed) return;
        // Advancing the generation invalidates per-template instantiation caches
        // bound to the previous run's now-freed emit module.
        CajetaModule::bumpReuseEpoch();
        CajetaType::restoreBaseline();
        CajetaModule::restoreBaseline();   // re-pins the stdlib singleton
        stdlibModule->getStructures() = baselineStructures;
        // The restore above dropped any lazily parsed package's types, so the
        // lazy bookkeeping must go too or a later import skips re-parsing it.
        Compiler::resetLazyStdlibState();
        // A prior run's method-template instantiations must be dropped from the
        // persistent classes, or the next run's resolveMethod finds the stale
        // entry and never emits the body into ITS module.
        for (auto& [canonical, klass] : stdlibModule->getStructures()) {
            if (!klass) continue;
            std::vector<MethodPtr> stale;
            for (auto& m : klass->getMethodList()) {
                if (m && m->isMethodTemplateInstantiation()) stale.push_back(m);
            }
            for (auto& m : stale) klass->removeMethod(m);
            // Reset bindings a reusing run generated into its own module, so
            // the next run regenerates rather than referencing a freed one.
            klass->restoreReuseBaseline();
        }
        // LAST, after the per-class bindings above are reset, so nothing still
        // points at a value about to be erased.
        restoreLlvmBaseline();
    }

    void StdlibReuseCore::captureContextBaseline() {
        if (!isPrimed) return;
        CajetaType::captureContextBaseline();
        CajetaModule::captureContextBaseline();
        // The post-sweep set, which pristine baselineStructures does not hold:
        // the module gains classes when a swept sibling pulls in a lazy package.
        contextStructures = stdlibModule->getStructures();
        contextLazyState = Compiler::captureLazyStdlibState();
        hasContextBaseline = true;
    }

    void StdlibReuseCore::restoreContextBaseline() {
        if (!hasContextBaseline) return;
        // Lint never codegens, so unlike restoreBaseline this is a pure
        // reinstatement: reassigning the maps drops the last request's target.
        CajetaType::restoreContextBaseline();
        CajetaModule::restoreContextBaseline();
        stdlibModule->getStructures() = contextStructures;
        Compiler::restoreLazyStdlibState(contextLazyState);
    }

    void StdlibReuseCore::invalidateContextBaseline() {
        hasContextBaseline = false;
        contextStructures.clear();
        contextLazyState = Compiler::LazyStdlibState{};
        CajetaType::invalidateContextBaseline();
        CajetaModule::invalidateContextBaseline();
    }

} // namespace cajeta
