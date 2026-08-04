// See StdlibReuseCore.h. The prime/restore sequences here are the ones the
// JIT test harness proved under the full suite (StdlibReuseCache); comments
// on the non-obvious steps live with the machinery they guard.

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
        // Arm xref capture for the stdlib parse: template members are only
        // recordable AS the stdlib parses, and a warm lint that asks for the
        // xref stream needs them in its restored logs exactly as a fresh
        // process (which parses the stdlib with capture armed) would have
        // them. Costs a few vectors of records when no one ever asks.
        xref::resetCapture();
        xref::setCaptureEnabled(true);
        // First Compiler under the shared context primes the global type
        // tables (resetGlobals + init) in that context.
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
        // Front-end-only users (warm lint) may have run requests already;
        // return to the pristine post-front-end state before layering.
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
        // Snapshot each baseline stdlib class's module-bound llvm bindings
        // (drop/vtable/RTTI/static-field globals + method functions) so
        // restoreBaseline can reset any that a reusing run lazily generated
        // into its own per-run module — the cross-module-reference leak.
        for (auto& [canon, klass] : baselineStructures)
            if (klass) klass->captureReuseBaseline();
        captureLlvmBaseline();
        xref::captureBaseline();
    }

    // --- the stdlib llvm::Module's own baseline (spec §4.1) ----------------
    //
    // Everything above restores the COMPILER's world — type registry, module
    // structures, per-class llvm bindings. None of it touches the persistent
    // stdlib llvm::Module, which a session can and does add to: a stdlib
    // template instantiated over a USER type emits into that user's module
    // (TemplateInstantiator's emitOwner rule), and the stdlib module is then
    // given `external global` DECLARATIONS of the instantiation's symbols so
    // its own references resolve at link time.
    //
    // Those declarations outlived the session that justified them. The next
    // session's user module defines nothing by those names, so materialization
    // failed with `Symbols not found: cajeta.collection.ArrayList<demo.Prog>
    // #ClassObject` — the leak behind ResidentWorld's suite-order failure.
    //
    // Snapshot the module's global values at capture time, then let
    // restoreLlvmBaseline decide what a session may take back. Not everything
    // it added is its own: see the reachability rule there.
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

    // The global values that REFER to `v`, walking through constant
    // expressions and aggregates to whatever named thing ultimately holds
    // them: the function whose body contains an instruction, or the global
    // whose initializer contains a constant.
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

    // An appending global (llvm.global_ctors and friends) is a LIST each
    // session appends its own entries to, not an ordinary reference. It comes
    // from the baseline, so the erase pass can't touch it — and every entry it
    // holds would otherwise look like a live baseline reference to the session
    // value it names. Prune it to entries whose referents all survive.
    //
    // The pruned array has a different type, so this replaces the global and
    // hands its identity (name, and its place in the baseline set) to the
    // replacement.
    void StdlibReuseCore::pruneAppendingGlobal(
            llvm::Module& m, const char* name,
            const std::set<llvm::GlobalValue*>& surviving) {
        llvm::GlobalVariable* gv = m.getNamedGlobal(name);
        if (!gv || !gv->hasInitializer()) return;
        // A session-added list is erased wholesale by the caller's normal
        // path. Replacing it here as well would leave the caller holding a
        // freed pointer — which it then dereferences (SIGSEGV in
        // removeDeadConstantUsers, seen the first time this ran under the
        // full ResidentWorld suite).
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

    void StdlibReuseCore::restoreLlvmBaseline() {
        llvm::Module* m = stdlibModule ? stdlibModule->getLlvmModule() : nullptr;
        if (!m) return;
        std::vector<llvm::GlobalValue*> added;
        for (llvm::GlobalValue& gv : m->global_values())
            if (!baselineLlvmValues.count(&gv)) added.push_back(&gv);
        if (added.empty()) return;

        // Constants outlive the code that built them: the previous session's
        // `{i32 65535, ptr @ctor, ptr null}` global_ctors entry stays uniqued
        // in the shared LLVMContext, and still counts as a use of @ctor, long
        // after the module holding it is gone. Drop those first or nothing
        // added ever looks unused.
        for (llvm::GlobalValue* gv : added) gv->removeDeadConstantUsers();

        // Not everything a session added is the session's to take back. The
        // drop-thunk backfill, for one, generates thunks for BASELINE stdlib
        // classes and writes them into those classes' baseline vtables: the
        // stdlib legitimately owns them now, and erasing them would leave the
        // vtable pointing at nothing.
        //
        // So the rule is reachability, not authorship: an added value that the
        // BASELINE still refers to (directly, or through another such value)
        // has been adopted and stays intact. Everything else is the session's
        // own accretion — its user-typed instantiations, their #ClassObject
        // declarations, the constructors registering them — and goes.
        //
        // The appending globals are deliberately NOT roots here: they are
        // lists, and treating a session's own entry in one as a baseline
        // reference would make every session value look adopted.
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
        // Erase to fixpoint: one doomed value can hold the last use of
        // another through a constant expression, which only drops when the
        // holder goes.
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
            // Unreachable by construction (a still-used value would have been
            // adopted). Report rather than force-erase: a dangling use is a
            // verifier failure at launch, and the cause belongs in the log.
            std::ostringstream msg;
            msg << "cajeta: resident stdlib kept " << stillUsed
                << " session-added symbol(s) that could not be released\n";
            cajeta::logLine("warn", msg.str());
        }
    }

    void StdlibReuseCore::restoreBaseline() {
        if (!isPrimed) return;
        // Advance the reuse generation so per-template method-instantiation
        // caches (held on persistent stdlib Methods) invalidate stale entries
        // bound to the previous run's now-freed user emit module.
        CajetaModule::bumpReuseEpoch();
        CajetaType::restoreBaseline();
        CajetaModule::restoreBaseline();   // re-pins the stdlib singleton
        stdlibModule->getStructures() = baselineStructures;
        // Lazy stdlib: the baseline was captured before any on-demand package
        // (cajeta.math) was parsed, and the restore above drops those types.
        // Clear the lazy bookkeeping too, so a later run importing the package
        // re-parses it instead of skipping it as "already parsed".
        Compiler::resetLazyStdlibState();
        // Drop every method-template instantiation a PRIOR run registered on
        // a persistent stdlib class. Such a class outlives the per-run user
        // module its instantiations were codegen'd into; left registered, the
        // next run's resolveMethod finds the stale entry and skips re-emitting
        // the body into ITS module. Collect-then-remove to avoid mutating the
        // maps mid-iteration.
        for (auto& [canonical, klass] : stdlibModule->getStructures()) {
            if (!klass) continue;
            std::vector<MethodPtr> stale;
            for (auto& m : klass->getMethodList()) {
                if (m && m->isMethodTemplateInstantiation()) stale.push_back(m);
            }
            for (auto& m : stale) klass->removeMethod(m);
            // Reset this class's module-bound llvm bindings that a reusing
            // run generated into its own per-run module, so the next run
            // regenerates into its module rather than referencing a freed one.
            klass->restoreReuseBaseline();
        }
        // ...and the same discipline one level down, in the IR itself: drop
        // whatever the previous session added to the persistent stdlib module.
        // Runs LAST, after the per-class bindings above are reset, so nothing
        // still points at a value about to be erased.
        restoreLlvmBaseline();
    }

    void StdlibReuseCore::captureContextBaseline() {
        if (!isPrimed) return;
        CajetaType::captureContextBaseline();
        CajetaModule::captureContextBaseline();
        // The stdlib module gains classes only when a swept sibling pulled a
        // lazy package (cajeta.math) into it; snapshot the post-sweep set so a
        // warm restore reinstates them (the pristine baselineStructures does
        // not). Siblings themselves are EXTERNAL modules — their classes live
        // in the canonical/structure maps captured above, not here.
        contextStructures = stdlibModule->getStructures();
        contextLazyState = Compiler::captureLazyStdlibState();
        hasContextBaseline = true;
    }

    void StdlibReuseCore::restoreContextBaseline() {
        if (!hasContextBaseline) return;
        // Lint never codegens, so there are no per-template method
        // instantiations or per-class llvm bindings to unwind (unlike
        // restoreBaseline) — this is a pure reinstatement of the captured
        // registries. Reassigning the maps drops the previous request's target
        // (its entries were never in the snapshot).
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
