#include "cajeta/jit/CajetaLazyEmitter.h"

#include "cajeta/compile/CajetaModule.h"
#include "cajeta/jit/JitModulePrep.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include "llvm/Transforms/Utils/ValueMapper.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <set>

namespace cajeta {

    namespace {

        // The shared snapshot tail: one definition cloned out of the live
        // module, prepared exactly as the eager delivery path prepares whole
        // modules (spec 4.4 — the passes hold over the lazy set because they
        // run on every snapshot), then bitcode-round-tripped into a context
        // ORC owns so the live module stays the front-end's to mutate.
        // 3.2.3 — the set of definitions a snapshot must carry: the kept
        // value plus every local-linkage global its body (transitively)
        // references. Local linkage cannot be declared across modules, so
        // those come along whole; everything else stays a declaration and
        // resolves through the generator. Foreign-module operands are left
        // to legalizeCrossModuleRefs, which runs on the clone.
        llvm::SmallPtrSet<const llvm::GlobalValue*, 32>
        reachableClosure(llvm::Module* live,
                         llvm::ArrayRef<const llvm::GlobalValue*> seeds) {
            llvm::SmallPtrSet<const llvm::GlobalValue*, 32> reached;
            llvm::SmallPtrSet<const llvm::Constant*, 32> seen;
            llvm::SmallVector<const llvm::GlobalValue*, 32> work;
            for (const llvm::GlobalValue* s : seeds) {
                if (reached.insert(s).second) work.push_back(s);
            }
            std::function<void(const llvm::Constant*)> scan =
                [&](const llvm::Constant* c) {
                    if (!seen.insert(c).second) return;
                    if (auto* gv = llvm::dyn_cast<llvm::GlobalValue>(c)) {
                        if (gv->getParent() == live && !gv->isDeclaration()
                            && gv->hasLocalLinkage()
                            && reached.insert(gv).second)
                            work.push_back(gv);
                        return;
                    }
                    for (const llvm::Use& u : c->operands())
                        if (auto* op = llvm::dyn_cast<llvm::Constant>(u.get()))
                            scan(op);
                };
            while (!work.empty()) {
                const llvm::GlobalValue* gv = work.pop_back_val();
                // Covers initializers, aliasees, and a function's
                // personality/prefix/prologue operands.
                for (const llvm::Use& u : gv->operands())
                    if (auto* op = llvm::dyn_cast<llvm::Constant>(u.get()))
                        scan(op);
                if (auto* fn = llvm::dyn_cast<llvm::Function>(gv))
                    for (const llvm::BasicBlock& bb : *fn)
                        for (const llvm::Instruction& inst : bb)
                            for (const llvm::Use& u : inst.operands())
                                if (auto* op =
                                        llvm::dyn_cast<llvm::Constant>(u.get()))
                                    scan(op);
            }
            return reached;
        }

        // The closure extractor shared by per-symbol snapshots and the init
        // delta (4.2.4). `ctorsToRebuild`, when non-null, names the ctor
        // functions (with priorities) the clone's llvm.global_ctors must
        // carry — the ONLY case a snapshot may carry ctors, since a
        // per-body snapshot re-running registration would be wrong
        // (spec 2.2).
        llvm::Expected<llvm::orc::ThreadSafeModule>
        extractClosure(llvm::Module* live,
                       llvm::ArrayRef<const llvm::GlobalValue*> seeds,
                       const std::string& ident,
                       const std::vector<std::pair<uint32_t,
                           const llvm::Function*>>* ctorsToRebuild) {
            const bool probe = std::getenv("CAJETA_SNAPSHOT_TIMING") != nullptr;
            auto now = [] { return std::chrono::steady_clock::now(); };
            auto us = [](auto a, auto b) {
                return std::chrono::duration_cast<std::chrono::microseconds>(
                           b - a).count();
            };
            auto t0 = now();
            auto reached = reachableClosure(live, seeds);

            // Local-linkage CONSTANTS may be duplicated per snapshot;
            // MUTABLE internal globals may not — a runtime static (the
            // script-result buffer, the exception TLS head) cloned into two
            // snapshots splits its state: set() writes one copy, get() reads
            // the other, and a throw pushes a frame the catch never walks.
            // Promote such variables in the LIVE module to external linkage
            // under a module-qualified name and drop them from the clone
            // set: every snapshot then declares them, and the generator
            // serves the single definition like any other live global.
            {
                llvm::SmallVector<llvm::GlobalVariable*, 8> promote;
                for (const llvm::GlobalValue* gv : reached) {
                    auto* g = llvm::dyn_cast<llvm::GlobalVariable>(gv);
                    if (!g || g->isConstant() || !g->hasLocalLinkage())
                        continue;
                    if (llvm::is_contained(seeds, gv)) continue;
                    promote.push_back(const_cast<llvm::GlobalVariable*>(g));
                }
                for (llvm::GlobalVariable* g : promote) {
                    std::string uniq = g->getName().str() + ".__lazyp.";
                    for (char c : live->getModuleIdentifier())
                        uniq += (std::isalnum((unsigned char) c) ? c : '_');
                    g->setName(uniq);
                    g->setLinkage(llvm::GlobalValue::ExternalLinkage);
                    g->setDSOLocal(false);
                    reached.erase(g);
                }
            }
            auto t1 = now();

            // A fresh module, NOT CloneModule: CloneModule walks the whole
            // live module creating a declaration for every one of its ~11k
            // globals — 26 of the 34 ms a snapshot used to cost, all of it
            // deleted again by the strip below. Build shells for the reached
            // set only and clone just those bodies. References to anything
            // else self-map into the live module and are rewritten to local
            // declarations by legalizeCrossModuleRefs — the same net that
            // already catches cross-module operands.
            auto clone = std::make_unique<llvm::Module>(
                ident, live->getContext());
            clone->setTargetTriple(live->getTargetTriple());
            clone->setDataLayout(live->getDataLayout());
            // Module flags must come along ("Debug Info Version" above all):
            // cloned bodies carry attached debug metadata, and the bitcode
            // reader strips DI from a module whose version flag is absent
            // ("ignoring debug info with an invalid version (0)").
            {
                llvm::SmallVector<llvm::Module::ModuleFlagEntry, 8> flags;
                live->getModuleFlagsMetadata(flags);
                for (auto& f : flags)
                    clone->addModuleFlag(f.Behavior, f.Key->getString(),
                                         f.Val);
            }

            llvm::ValueToValueMapTy vmap;
            for (const llvm::GlobalValue* gv : reached) {
                if (auto* f = llvm::dyn_cast<llvm::Function>(gv)) {
                    auto* nf = llvm::Function::Create(
                        f->getFunctionType(), f->getLinkage(),
                        f->getAddressSpace(), f->getName(), clone.get());
                    nf->copyAttributesFrom(f);
                    // Comdats are section-linker semantics; ORC dedups weak
                    // symbols by linkage alone, and a comdat would need its
                    // own fresh-module recreation. Shed them.
                    nf->setComdat(nullptr);
                    vmap[f] = nf;
                } else if (auto* g =
                               llvm::dyn_cast<llvm::GlobalVariable>(gv)) {
                    auto* ng = new llvm::GlobalVariable(
                        *clone, g->getValueType(), g->isConstant(),
                        g->getLinkage(), nullptr, g->getName(), nullptr,
                        g->getThreadLocalMode(), g->getAddressSpace(),
                        g->isExternallyInitialized());
                    ng->copyAttributesFrom(g);
                    ng->setComdat(nullptr);
                    vmap[g] = ng;
                } else {
                    // No alias/ifunc has ever appeared in a closure; fail
                    // loudly rather than half-copy one.
                    return llvm::createStringError(
                        llvm::inconvertibleErrorCode(),
                        "lazy emit: unsupported global kind '%s' in the "
                        "closure of '%s'", gv->getName().str().c_str(),
                        ident.c_str());
                }
            }
            for (const llvm::GlobalValue* gv : reached) {
                if (auto* f = llvm::dyn_cast<llvm::Function>(gv)) {
                    auto* nf = llvm::cast<llvm::Function>(vmap[f]);
                    auto argIt = nf->arg_begin();
                    for (const llvm::Argument& arg : f->args()) {
                        argIt->setName(arg.getName());
                        vmap[&arg] = &*argIt++;
                    }
                    llvm::SmallVector<llvm::ReturnInst*, 4> returns;
                    llvm::CloneFunctionInto(
                        nf, f, vmap,
                        llvm::CloneFunctionChangeType::DifferentModule,
                        returns);
                } else if (auto* g =
                               llvm::dyn_cast<llvm::GlobalVariable>(gv)) {
                    if (g->hasInitializer()) {
                        llvm::cast<llvm::GlobalVariable>(vmap[g])
                            ->setInitializer(llvm::cast<llvm::Constant>(
                                llvm::MapValue(g->getInitializer(), vmap)));
                    }
                }
            }
            // The init delta's reason to exist: a global_ctors array naming
            // exactly the not-yet-delivered ctors, cloned above as seeds.
            if (ctorsToRebuild) {
                for (auto& [prio, fn] : *ctorsToRebuild) {
                    llvm::appendToGlobalCtors(
                        *clone, llvm::cast<llvm::Function>(vmap[fn]), prio);
                }
            }
            auto t2 = now();

            // Instantiation emission can leave operands pointing at globals
            // homed in ANOTHER live module; the bitcode writer cannot express
            // that, so rewrite them to local declarations first (3.2.2).
            cajeta::jit::legalizeCrossModuleRefs(clone.get());
            // ODR: an eagerly delivered module or another snapshot may carry
            // the same instantiation symbol; weak_odr lets ORC pick one.
            cajeta::jit::demoteInstantiationsToWeakODR(clone.get());

            // Safety net: only declarations the kept closure actually uses
            // may reach bitcode (3.2.3). The fresh-module build creates none
            // unused; this keeps the property if a later pass grows one.
            for (auto it = clone->begin(); it != clone->end();) {
                llvm::Function& f = *it++;
                if (f.isDeclaration() && f.use_empty()) f.eraseFromParent();
            }
            for (auto it = clone->global_begin();
                 it != clone->global_end();) {
                llvm::GlobalVariable& g = *it++;
                if (g.isDeclaration() && g.use_empty()) g.eraseFromParent();
            }

            // Every declaration must lose dso_local: a dso_local external is
            // addressed with a direct 32-bit
            // PC-relative fixup — valid inside the eager world's single large
            // module, but a snapshot's externals live in OTHER snapshots that
            // JITLink may place >2 GB away ("out of range of Delta32 fixup",
            // seen on __cajeta_session_guard_frame at delivery #558).
            // Clearing it routes external addressing through the GOT, which
            // JITLink synthesizes with no range limit.
            for (llvm::GlobalValue& gv : clone->global_values()) {
                if (gv.isDeclaration()) gv.setDSOLocal(false);
            }

            // Dylib-init work is the EAGER remainder (spec 2.2): a per-body
            // snapshot that carried llvm.global_ctors would re-run class
            // registration on every delivered body. Only the init delta may
            // carry them — and then only the ones it just rebuilt.
            if (!ctorsToRebuild) {
                if (auto* ctors = clone->getNamedGlobal("llvm.global_ctors"))
                    ctors->eraseFromParent();
                if (auto* dtors = clone->getNamedGlobal("llvm.global_dtors"))
                    dtors->eraseFromParent();
            }
            auto t3 = now();

            llvm::SmallVector<char, 0> buf;
            {
                llvm::raw_svector_ostream os(buf);
                llvm::WriteBitcodeToFile(*clone, os);
            }
            auto memBuffer = llvm::MemoryBuffer::getMemBufferCopy(
                llvm::StringRef(buf.data(), buf.size()),
                clone->getModuleIdentifier());
            auto tsCtx = std::make_unique<llvm::LLVMContext>();
            llvm::orc::ThreadSafeContext tsContext(std::move(tsCtx));
            auto parsed = tsContext.withContextDo([&](llvm::LLVMContext* ctx) {
                return llvm::parseBitcodeFile(memBuffer->getMemBufferRef(),
                                              *ctx);
            });
            if (!parsed) return parsed.takeError();
            if (probe) {
                auto t4 = now();
                std::fprintf(stderr,
                             "[snap] closure %lld us, clone %lld us, prep %lld "
                             "us, bitcode %lld us, %zu kept, %zu kb\n",
                             (long long) us(t0, t1), (long long) us(t1, t2),
                             (long long) us(t2, t3), (long long) us(t3, t4),
                             reached.size(), buf.size() / 1024);
            }
            return llvm::orc::ThreadSafeModule(std::move(*parsed),
                                               std::move(tsContext));
        }

        llvm::Expected<llvm::orc::ThreadSafeModule>
        snapshotOne(llvm::Module* live, const llvm::GlobalValue* keep,
                    const std::string& sym) {
            const llvm::GlobalValue* seeds[] = {keep};
            return extractClosure(live, seeds, "lazy:" + sym, nullptr);
        }

    } // namespace

    llvm::Expected<llvm::orc::ThreadSafeModule>
    emitMethodModule(const MethodPtr& method) {
        if (!method) {
            return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                           "lazy emit: null method");
        }
        const std::string sym = method->getLlvmSymbolName();

        // The same two calls the eager fixpoint makes, for one method.
        method->getLlvmFunctionType();
        method->generateCode();

        // The EMIT module, not the home module: a user-typed specialization
        // redirects its emission to the unit being compiled
        // (stdlib-reuse policy), so ArrayList<cell.Foo>::count's body lands
        // in the CELL's module while getModule() still names the stdlib —
        // and the snapshot must be cut where the body actually is.
        CajetaModulePtr parent = method->getEmitModule();
        llvm::Module* live = parent ? parent->getLlvmModule() : nullptr;
        if (!live) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(),
                "lazy emit: '%s' has no parent llvm::Module", sym.c_str());
        }
        llvm::Function* fn = live->getFunction(sym);
        if (!fn || fn->isDeclaration()) {
            // spec 5.3 — distinguishable from an ordinary missing symbol: the
            // method was indexed but generateCode() left no definition.
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(),
                "lazy emit: generateCode() left no body for '%s'", sym.c_str());
        }
        return snapshotOne(live, fn, sym);
    }

    llvm::Expected<llvm::orc::ThreadSafeModule>
    snapshotLiveDefinition(llvm::GlobalValue* gv) {
        if (!gv || gv->isDeclaration()) {
            return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                           "lazy emit: no live definition");
        }
        return snapshotOne(gv->getParent(), gv, gv->getName().str());
    }

    namespace {
        // Every defined llvm.global_ctors entry of `live`, in array order.
        std::vector<std::pair<uint32_t, const llvm::Function*>>
        definedCtors(llvm::Module* live) {
            std::vector<std::pair<uint32_t, const llvm::Function*>> out;
            auto* ga = live->getNamedGlobal("llvm.global_ctors");
            if (!ga || !ga->hasInitializer()) return out;
            auto* arr = llvm::dyn_cast<llvm::ConstantArray>(
                ga->getInitializer());
            if (!arr) return out;
            for (const llvm::Use& u : arr->operands()) {
                auto* entry = llvm::dyn_cast<llvm::ConstantStruct>(u.get());
                if (!entry || entry->getNumOperands() < 2) continue;
                auto* prio = llvm::dyn_cast<llvm::ConstantInt>(
                    entry->getOperand(0));
                auto* fn = llvm::dyn_cast<llvm::Function>(
                    entry->getOperand(1)->stripPointerCasts());
                if (!prio || !fn || fn->isDeclaration()) continue;
                out.emplace_back((uint32_t) prio->getZExtValue(), fn);
            }
            return out;
        }
    } // namespace

    void recordDeliveredCtors(llvm::Module* live,
                              std::set<std::string>& deliveredCtors) {
        for (auto& [prio, fn] : definedCtors(live))
            deliveredCtors.insert(fn->getName().str());
    }

    llvm::Expected<llvm::orc::ThreadSafeModule>
    extractInitDelta(llvm::Module* live,
                     std::set<std::string>& deliveredCtors) {
        std::vector<std::pair<uint32_t, const llvm::Function*>> newCtors;
        for (auto& [prio, fn] : definedCtors(live)) {
            if (deliveredCtors.count(fn->getName().str())) continue;
            newCtors.emplace_back(prio, fn);
        }
        // Nothing new since the last delivery: an empty (false) TSM, which
        // callers must treat as "deliver nothing", not as an error.
        if (newCtors.empty()) return llvm::orc::ThreadSafeModule();

        std::vector<const llvm::GlobalValue*> seeds;
        seeds.reserve(newCtors.size());
        for (auto& [prio, fn] : newCtors) seeds.push_back(fn);
        auto tsm = extractClosure(
            live, seeds,
            "lazy-init:" + live->getModuleIdentifier(), &newCtors);
        if (!tsm) return tsm;
        for (auto& [prio, fn] : newCtors)
            deliveredCtors.insert(fn->getName().str());
        return tsm;
    }

} // namespace cajeta
