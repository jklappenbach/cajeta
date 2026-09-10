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

        /// The definitions a snapshot must carry: the seeds plus every
        /// local-linkage global they transitively reference, since local
        /// linkage cannot be declared across modules.
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

        /// Clones the seeds' closure into a fresh module and round-trips it
        /// through bitcode into a context ORC owns. `ctorsToRebuild`, when
        /// non-null, is the ONLY case in which a snapshot may carry ctors.
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

            // Local-linkage CONSTANTS may be duplicated per snapshot, but a
            // MUTABLE internal global cloned twice splits its state. Promote
            // those in the LIVE module so every snapshot declares the one.
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

            // A fresh module, NOT CloneModule, which would declare every
            // global of the live module only for the strip below to delete it.
            auto clone = std::make_unique<llvm::Module>(
                ident, live->getContext());
            clone->setTargetTriple(live->getTargetTriple());
            clone->setDataLayout(live->getDataLayout());
            // Module flags must come along: the bitcode reader strips debug
            // info from a module whose "Debug Info Version" flag is absent.
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
                    // ORC dedups weak symbols by linkage alone.
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
            if (ctorsToRebuild) {
                for (auto& [prio, fn] : *ctorsToRebuild) {
                    llvm::appendToGlobalCtors(
                        *clone, llvm::cast<llvm::Function>(vmap[fn]), prio);
                }
            }
            auto t2 = now();

            // The bitcode writer cannot express an operand homed elsewhere.
            cajeta::jit::legalizeCrossModuleRefs(clone.get());
            cajeta::jit::demoteInstantiationsToWeakODR(clone.get());

            for (auto it = clone->begin(); it != clone->end();) {
                llvm::Function& f = *it++;
                if (f.isDeclaration() && f.use_empty()) f.eraseFromParent();
            }
            for (auto it = clone->global_begin();
                 it != clone->global_end();) {
                llvm::GlobalVariable& g = *it++;
                if (g.isDeclaration() && g.use_empty()) g.eraseFromParent();
            }

            // A dso_local external is addressed by a direct 32-bit PC-relative
            // fixup, but a snapshot's externals live in other snapshots JITLink
            // may place >2 GB away. Clearing it routes them through the GOT.
            for (llvm::GlobalValue& gv : clone->global_values()) {
                if (gv.isDeclaration()) gv.setDSOLocal(false);
            }

            // Carrying ctors here would re-run registration per delivered body.
            if (!ctorsToRebuild) {
                if (auto* ctors = clone->getNamedGlobal("llvm.global_ctors"))
                    ctors->eraseFromParent();
                if (auto* dtors = clone->getNamedGlobal("llvm.global_dtors"))
                    dtors->eraseFromParent();
            }
            auto t3 = now();

            // Walks the clone as the bitcode writer will, holder before operands.
            if (std::getenv("CAJETA_SNAPSHOT_VALIDATE")) {
                llvm::SmallPtrSet<const llvm::Constant*, 32> seenv;
                std::function<void(const llvm::Constant*)> scanv =
                    [&](const llvm::Constant* c) {
                        if (!seenv.insert(c).second) return;
                        unsigned id = c->getValueID();
                        if (id > 90)
                            std::fprintf(stderr,
                                         "[val]   BAD id=%u at %p\n", id,
                                         (const void*) c);
                        if (auto* g = llvm::dyn_cast<llvm::GlobalValue>(c)) {
                            if (g->getParent() != clone.get())
                                std::fprintf(stderr,
                                             "[val]   FOREIGN global %s "
                                             "(parent %p)\n",
                                             g->getName().str().c_str(),
                                             (const void*) g->getParent());
                            return;
                        }
                        for (const llvm::Use& u : c->operands())
                            if (auto* op =
                                    llvm::dyn_cast<llvm::Constant>(u.get()))
                                scanv(op);
                    };
                for (llvm::GlobalVariable& g : clone->globals()) {
                    std::fprintf(stderr, "[val] global %s\n",
                                 g.getName().str().c_str());
                    if (g.hasInitializer()) scanv(g.getInitializer());
                }
                llvm::SmallPtrSet<const llvm::Metadata*, 32> seenmd;
                std::function<void(const llvm::Metadata*)> scanmd =
                    [&](const llvm::Metadata* md) {
                        if (!md || !seenmd.insert(md).second) return;
                        if (auto* vam =
                                llvm::dyn_cast<llvm::ValueAsMetadata>(md)) {
                            const llvm::Value* v = vam->getValue();
                            std::fprintf(stderr, "[val]   VAM %p -> %p\n",
                                         (const void*) vam, (const void*) v);
                            if (v)
                                if (auto* c = llvm::dyn_cast<llvm::Constant>(v))
                                    scanv(c);
                            return;
                        }
                        if (auto* node = llvm::dyn_cast<llvm::MDNode>(md))
                            for (const llvm::MDOperand& op : node->operands())
                                scanmd(op.get());
                    };
                for (llvm::Function& f : *clone) {
                    std::fprintf(stderr, "[val] fn %s\n",
                                 f.getName().str().c_str());
                    llvm::SmallVector<
                        std::pair<unsigned, llvm::MDNode*>, 8> fmds;
                    f.getAllMetadata(fmds);
                    for (auto& [kind, node] : fmds) scanmd(node);
                    for (llvm::BasicBlock& bb : f)
                        for (llvm::Instruction& inst : bb) {
                            for (const llvm::Use& u : inst.operands())
                                if (auto* op = llvm::dyn_cast<llvm::Constant>(
                                        u.get()))
                                    scanv(op);
                            llvm::SmallVector<
                                std::pair<unsigned, llvm::MDNode*>, 8> imds;
                            inst.getAllMetadata(imds);
                            for (auto& [kind, node] : imds) scanmd(node);
                        }
                }
                for (llvm::GlobalVariable& g : clone->globals()) {
                    llvm::SmallVector<
                        std::pair<unsigned, llvm::MDNode*>, 8> gmds;
                    g.getAllMetadata(gmds);
                    std::fprintf(stderr, "[val] global-md %s\n",
                                 g.getName().str().c_str());
                    for (auto& [kind, node] : gmds) scanmd(node);
                }
                for (const llvm::NamedMDNode& nmd : clone->named_metadata()) {
                    std::fprintf(stderr, "[val] named-md %s\n",
                                 nmd.getName().str().c_str());
                    for (const llvm::MDNode* node : nmd.operands())
                        scanmd(node);
                }
                std::fprintf(stderr, "[val] %s walk clean\n", ident.c_str());
            }

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

        /// One-seed snapshot, named `lazy:<sym>` and carrying no ctors.
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

        method->getLlvmFunctionType();
        method->generateCode();

        // The EMIT module, not the home module: a user-typed specialization
        // emits into the unit being compiled, so cut the snapshot there.
        CajetaModulePtr parent = method->getEmitModule();
        llvm::Module* live = parent ? parent->getLlvmModule() : nullptr;
        if (!live) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(),
                "lazy emit: '%s' has no parent llvm::Module", sym.c_str());
        }
        llvm::Function* fn = live->getFunction(sym);
        if (!fn || fn->isDeclaration()) {
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
        /// Every defined llvm.global_ctors entry of `live`, in array order.
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
        // An empty (false) TSM means "deliver nothing", not an error.
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
