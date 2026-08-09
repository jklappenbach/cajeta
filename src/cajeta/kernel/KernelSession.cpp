#include "cajeta/kernel/KernelSession.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <vector>

#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/compile/DropBackfill.h"
#include "cajeta/method/Method.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/type/CajetaType.h"
#include "cajeta/compile/SessionState.h"
#include "cajeta/compile/StdlibReuseCore.h"
#include "cajeta/error/Exception.h"
#include "cajeta/compile/ScriptUnitSynthesis.h"
#include "cajeta/jit/JitModulePrep.h"

namespace cajeta::kernel {

namespace {

    // Sanitize a cell name into a filename stem. The stem becomes the script
    // unit's implicit class name (script-units 3.2), so it must be a legal
    // identifier: "In[3]" -> "In_3_".
    std::string stemFor(const std::string& cellName) {
        std::string out;
        out.reserve(cellName.size());
        for (char c : cellName) {
            out.push_back((std::isalnum(static_cast<unsigned char>(c)) || c == '_')
                              ? c : '_');
        }
        if (out.empty()) out = "cell";
        if (std::isdigit(static_cast<unsigned char>(out[0]))) {
            out.insert(out.begin(), '_');
        }
        return out;
    }

    void ensureTargetsInitialized() {
        static bool done = false;
        if (done) return;
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
        llvm::InitializeNativeTargetAsmParser();
        done = true;
    }

}  // namespace

struct KernelSession::Impl {
    std::unique_ptr<llvm::orc::LLJIT> jit;
    llvm::orc::JITDylib* bootstrapJD = nullptr;
    // Oldest cell first. Lookup walks it BACKWARDS so a redefined name
    // resolves to the newest definition (script-units 5.2 last-write-wins).
    std::vector<llvm::orc::JITDylib*> cellJDs;

    // ONE Compiler for the session: cell N must see the types, methods, and
    // instantiations cells 1..N-1 registered, which lives in the Compiler's
    // type world, not in the JIT.
    std::unique_ptr<Compiler> compiler;
    // The script-units U4 ownership table, carried across cell compiles.
    SessionState sessionState;

    std::filesystem::path scratchRoot;
    SessionStats stats;
    int execCount = 0;
    bool shutdownDone = false;

    // Modules already delivered to the JIT, by IR-module pointer. A cell's
    // codegen can emit into the stdlib module (template instantiations), so
    // "what is new this cell" is decided by identity, not by list position.
    std::set<llvm::Module*> delivered;
    // Modules belonging to a cell that FAILED to compile. The Compiler keeps
    // them in its module list with half-built methods, so a later cell's
    // codegen fixpoint would re-run generateCode over them and re-throw the
    // dead cell's error — poisoning every subsequent cell. Skipped forever
    // (script-units 5.5: a failed cell leaves the session unchanged).
    std::set<llvm::Module*> poisoned;
    // Globals DEFINED by an already-delivered cell. Statics are session-
    // lived: the declaring cell owns the storage and later cells must
    // REFERENCE it, never emit a fresh zero-initialized copy that their own
    // (first-searched) dylib would then resolve to.
    std::set<std::string> definedGlobals;

    // Set the per-cell link order EXPLICITLY. createJITDylib seeds the order
    // with the process-symbol main dylib FIRST; leaving that in place makes
    // user code bind runtime symbols (__cajeta_exc_push, the TLS accessors)
    // to the process's NATIVE runtime while stdlib code inside the JIT uses
    // the JIT copy — two __cajeta_main_exc_top slots, and a throw crossing
    // the seam is never caught. Order: self, newest prior cells, bootstrap,
    // then the process dylib as the last resort.
    void applyLinkOrder(llvm::orc::JITDylib& jd) {
        std::vector<llvm::orc::JITDylibSearchOrder::value_type> order;
        const auto exported =
            llvm::orc::JITDylibLookupFlags::MatchExportedSymbolsOnly;
        for (auto it = cellJDs.rbegin(); it != cellJDs.rend(); ++it) {
            order.emplace_back(*it, exported);
        }
        order.emplace_back(bootstrapJD, exported);
        order.emplace_back(&jit->getMainJITDylib(), exported);
        jd.setLinkOrder(std::move(order), /*LinkAgainstThisJITDylibFirst=*/true);
    }
};

KernelSession::KernelSession() : impl_(new Impl) {}

KernelSession::~KernelSession() {
    if (impl_) shutdown();
}

std::unique_ptr<KernelSession> KernelSession::create(std::string* error) {
    ensureTargetsInitialized();
    auto setErr = [&](const std::string& m) {
        if (error) *error = m;
        return nullptr;
    };

    std::unique_ptr<KernelSession> s(new KernelSession);
    Impl& impl = *s->impl_;

    auto jitOrErr = llvm::orc::LLJITBuilder().create();
    if (!jitOrErr) {
        return setErr("LLJIT create failed: "
                      + llvm::toString(jitOrErr.takeError()));
    }
    impl.jit = std::move(*jitOrErr);

    // Process symbols on the main dylib — the native runtime the JIT'd code
    // calls into (and the last-resort resolver for every cell).
    auto& mainJD = impl.jit->getMainJITDylib();
    auto generator = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
        impl.jit->getDataLayout().getGlobalPrefix());
    if (!generator) {
        return setErr("process-symbol generator failed: "
                      + llvm::toString(generator.takeError()));
    }
    mainJD.addGenerator(std::move(*generator));

    auto bootstrapOrErr = impl.jit->createJITDylib("CajetaBootstrap");
    if (!bootstrapOrErr) {
        return setErr("bootstrap dylib create failed: "
                      + llvm::toString(bootstrapOrErr.takeError()));
    }
    impl.bootstrapJD = &*bootstrapOrErr;
    impl.bootstrapJD->addToLinkOrder(mainJD);

    // Session-lived Compiler over the primed stdlib. The reuse core is
    // single-threaded and its baselines are thread_local, so this must be the
    // thread that owns the session (header contract).
    auto& core = StdlibReuseCore::instance();
    try {
        core.ensurePrimed();
        core.restoreBaseline();
        Compiler::setSharedContext(core.context());
        impl.compiler = std::make_unique<Compiler>();
        impl.compiler->setMode(CompilerMode::Debug);
        impl.compiler->ensureStdlibModule();
    } catch (cajeta::Exception& e) {
        Compiler::setSharedContext(nullptr);
        return setErr(std::string("stdlib prime failed: ") + e.getErrorId()
                      + ": " + e.getMessage());
    }

    static std::mt19937_64 rng(std::random_device{}());
    impl.scratchRoot = std::filesystem::temp_directory_path()
                     / ("cajeta_kernel_" + std::to_string(rng()));
    std::filesystem::create_directories(impl.scratchRoot / "src" / "cajeta"
                                        / "script");
    return s;
}

CellResult KernelSession::execute(const std::string& source) {
    return execute(source, "In[" + std::to_string(impl_->execCount + 1) + "]");
}

CellResult KernelSession::execute(const std::string& source,
                                  const std::string& cellName) {
    CellResult result;
    result.file = cellName;
    Impl& impl = *impl_;
    ++impl.execCount;

    // The cell's source has to reach the compiler as a FILE: the script-unit
    // stem (and so the implicit class name) is path-derived, and the whole
    // parse path is file-oriented. One file per cell under the session's
    // scratch root, named for the cell.
    std::string stem = stemFor(cellName);
    std::filesystem::path cellPath =
        impl.scratchRoot / "src" / "cajeta" / "script" / (stem + ".cajeta");
    {
        std::ofstream out(cellPath);
        out << source;
    }

    // Diagnostics speak the CELL's name and the user's lines (script-units
    // U5 maps wrapper lines back); the ownership table carries across cells.
    impl.compiler->setSessionState(&impl.sessionState, cellName);

    CajetaModulePtr cellModule;
    try {
        cellModule = impl.compiler->createModule(
            cellPath.string(), (impl.scratchRoot / "src").string(),
            (impl.scratchRoot / "archive").string());
        impl.compiler->compile(cellModule);
        // Codegen finalize, mirroring the JIT host's cold path. `compile()`
        // builds the front-end world; bodies, statics, and the reflective
        // thunks are separate passes, and skipping them leaves
        // `__cajeta_*_reflect_invoke/new` and the #ClassObject globals
        // undefined — the JIT then fails to materialize the cell with
        // "Symbols not found". The method loop is a FIXPOINT: emitting a
        // body can instantiate a template, adding methods to emit.
        CajetaModule::validatePlaceholders();
        CajetaModule::resolveAdviceMatches();
        CajetaModule::resolveDependencyGraph();
        // The codegen set INCLUDES the stdlib module: its method bodies are
        // emitted lazily, on demand, and a cell that calls into the stdlib
        // needs those bodies to exist or the cell fails to materialize on
        // cajeta.lang.Object::drop and friends. (This is the same set the
        // JIT host's codegenMods() builds, and the reason a cold launch
        // pays a stdlib-codegen phase.)
        auto codegenMods = [&]() {
            auto own = impl.compiler->getModules();
            std::vector<CajetaModulePtr> mods;
            for (auto& m : own) {
                if (m && m->getLlvmModule()
                    && impl.poisoned.count(m->getLlvmModule())) continue;
                mods.push_back(m);
            }
            if (auto stdlib = CajetaModule::getStdlibModule()) {
                mods.push_back(stdlib);
            }
            return mods;
        };
        size_t prevMethodCount = 0;
        while (true) {
            auto mods = codegenMods();
            size_t methodCount = 0;
            for (auto& m : mods) methodCount += m->getAllMethods().size();
            for (auto& m : mods)
                for (auto& method : m->getAllMethods())
                    method->getLlvmFunctionType();
            for (auto& m : mods) m->completePendingInterfaceVTables();
            for (auto& m : mods)
                for (auto& method : m->getAllMethods()) method->generateCode();
            size_t after = 0;
            for (auto& m : codegenMods()) after += m->getAllMethods().size();
            if (after == methodCount && after == prevMethodCount) break;
            prevMethodCount = after;
        }
        {
            for (auto& m : codegenMods())
                for (auto& [name, klass] : m->getStructures())
                    if (klass) klass->generateStaticInitializers();
        }
        // REFL-2: reflective adapter bodies + #ClassObject registration.
        for (auto& [key, type] : CajetaType::getCanonicalMap()) {
            if (auto klass = std::dynamic_pointer_cast<CajetaClass>(type)) {
                klass->emitReflectInvokeBody();
                klass->emitReflectNewBody();
                klass->finalizeClassObject();
            }
        }
    } catch (cajeta::Exception& e) {
        // script-units 5.5 / spec 2.2 — a failed cell leaves the session
        // exactly as it was. No dylib was created, and the ownership table
        // is only written back on a successful body compile.
        result.errorId = e.getErrorId();
        result.message = e.getMessage();
        result.file = e.getFile().empty() ? cellName : e.getFile();
        result.line = e.getLine();
        if (cellModule && cellModule->getLlvmModule()) {
            impl.poisoned.insert(cellModule->getLlvmModule());
        }
        return result;
    } catch (std::exception& e) {
        result.errorId = "CAJETA_ERROR_INTERNAL";
        result.message = e.what();
        return result;
    }

    // Everything this cell's codegen produced that has not been delivered
    // yet: the cell's own module, plus any module its instantiations landed
    // in (the stdlib module accumulates specializations).
    std::vector<CajetaModulePtr> fresh;
    {
        auto all = impl.compiler->getModules();   // by value — see the
        std::vector<CajetaModulePtr> candidates(all.begin(), all.end());
        // The stdlib module is NOT in getModules() — it is a separate
        // process-wide module that ACCUMULATES template instantiations as
        // cells use them. It must be delivered too, or every cell fails to
        // materialize on cajeta.lang.Object's vtable and drop thunks.
        if (auto stdlib = CajetaModule::getStdlibModule()) {
            candidates.push_back(stdlib);
        }
        for (auto& m : candidates) {
            if (m && m->getLlvmModule()
                && !impl.delivered.count(m->getLlvmModule())
                && !impl.poisoned.count(m->getLlvmModule())) {
                fresh.push_back(m);
            }
        }
    }
    if (fresh.empty()) {
        result.errorId = "CAJETA_ERROR_INTERNAL";
        result.message = "cell produced no module";
        return result;
    }

    // Same preparation the per-module delivery path runs: legalize every
    // module before verifying any (a use from B trips A's verifier), then
    // demote instantiations so a specialization shared with an earlier cell
    // is not a duplicate definition.
    std::vector<CajetaModulePtr> scan(fresh.begin(), fresh.end());
    cajeta::backfillDropFunctions(scan, scan);
    cajeta::pinDropFunctionDefinitions(scan);
    for (auto& m : fresh) {
        cajeta::jit::legalizeCrossModuleRefs(m->getLlvmModule());
        impl.stats.weakDemotedInstantiations +=
            cajeta::jit::demoteInstantiationsToWeakODR(m->getLlvmModule());
    }

    // Session-lived statics. A later cell that merely REFERENCES a class
    // static re-emits the global with an initializer; because a cell's own
    // dylib is searched first, it would then read its private zero copy
    // instead of the value the declaring cell set. Turn any global an
    // earlier cell already defined back into a declaration.
    // EXTERNAL linkage only. Names like `.rtti.str`, `.rtti.methods` and
    // `.cajeta.framedesc` are PRIVATE per-module globals that every module
    // emits under the same name — they are not shared session state, and
    // matching them by name stripped each new cell's own copies into
    // unresolvable declarations ("Symbols not found: .rtti.ctors, ...").
    // A session-lived static is externally linked; nothing else qualifies.
    auto sharedGlobal = [](const llvm::GlobalVariable& g) {
        return g.hasInitializer() && !g.hasLocalLinkage()
            && !g.hasAppendingLinkage()
            && g.getLinkage() != llvm::GlobalValue::PrivateLinkage
            && g.getLinkage() != llvm::GlobalValue::InternalLinkage;
    };
    for (auto& m : fresh) {
        for (auto& g : m->getLlvmModule()->globals()) {
            if (!sharedGlobal(g)) continue;
            if (!impl.definedGlobals.count(g.getName().str())) continue;
            g.setInitializer(nullptr);
            g.setComdat(nullptr);
            g.setLinkage(llvm::GlobalValue::ExternalLinkage);
        }
    }

    auto jdOrErr = impl.jit->createJITDylib(
        "Cell." + std::to_string(impl.execCount) + "." + stem);
    if (!jdOrErr) {
        result.errorId = "CAJETA_ERROR_INTERNAL";
        result.message = "cell dylib create failed: "
                       + llvm::toString(jdOrErr.takeError());
        return result;
    }
    llvm::orc::JITDylib& cellJD = *jdOrErr;
    impl.applyLinkOrder(cellJD);

    for (auto& m : fresh) {
        llvm::Module* lm = m->getLlvmModule();
        std::string verifyErr;
        llvm::raw_string_ostream vs(verifyErr);
        if (llvm::verifyModule(*lm, &vs)) {
            result.errorId = "CAJETA_ERROR_INTERNAL";
            result.message = "module verify failed: " + verifyErr;
            return result;
        }
    }

    // Deliver a SNAPSHOT, not the live module. The front-end keeps owning
    // its llvm::Module and will keep mutating it on later cells (new
    // instantiations land in the stdlib module), so the JIT gets bitcode
    // re-parsed into its own context — the same round-trip the per-module
    // delivery path uses, and the reason a delivered cell can never be
    // disturbed by a later one.
    for (auto& m : fresh) {
        llvm::Module* lm = m->getLlvmModule();
        llvm::SmallVector<char, 0> buf;
        {
            llvm::raw_svector_ostream os(buf);
            llvm::WriteBitcodeToFile(*lm, os);
        }
        auto tsCtx = std::make_unique<llvm::LLVMContext>();
        llvm::orc::ThreadSafeContext tsContext(std::move(tsCtx));
        auto memBuffer = llvm::MemoryBuffer::getMemBufferCopy(
            llvm::StringRef(buf.data(), buf.size()), lm->getModuleIdentifier());
#if LLVM_VERSION_MAJOR >= 21
        auto parsed = tsContext.withContextDo([&](llvm::LLVMContext* ctx) {
            return llvm::parseBitcodeFile(memBuffer->getMemBufferRef(), *ctx);
        });
#else
        auto parsed = llvm::parseBitcodeFile(memBuffer->getMemBufferRef(),
                                             *tsContext.getContext());
#endif
        if (!parsed) {
            result.errorId = "CAJETA_ERROR_INTERNAL";
            result.message = "bitcode reparse failed: "
                           + llvm::toString(parsed.takeError());
            return result;
        }
        auto tsm = llvm::orc::ThreadSafeModule(std::move(*parsed),
                                               std::move(tsContext));
        if (auto err = impl.jit->addIRModule(cellJD, std::move(tsm))) {
            result.errorId = "CAJETA_ERROR_INTERNAL";
            result.message = "addIRModule failed: "
                           + llvm::toString(std::move(err));
            return result;
        }
        impl.delivered.insert(lm);
        for (auto& g : lm->globals()) {
            if (sharedGlobal(g)) impl.definedGlobals.insert(g.getName().str());
        }
    }

    if (auto err = impl.jit->initialize(cellJD)) {
        result.errorId = "CAJETA_ERROR_INTERNAL";
        result.message = "cell initialize failed: "
                       + llvm::toString(std::move(err));
        return result;
    }

    impl.cellJDs.push_back(&cellJD);
    ++impl.stats.cellsCompiled;
    ++impl.stats.cellDylibsCreated;

    // Register this cell's implicit class in the session's cumulative
    // namespace so LATER cells' bare calls can reach its top-level methods
    // (jupyter-kernel 1.2.4). Recorded only on success, so a failed cell
    // contributes nothing (script-units 5.5).
    if (cellModule && !cellModule->getStructures().empty()) {
        for (auto& [canonical, klass] : cellModule->getStructures()) {
            if (klass && klass->isScriptSynthesized()) {
                impl.sessionState.addUnitClass(canonical);
            }
        }
    }

    // Run the cell's entry — its top-level statements.
    //
    // The symbol is MANGLED (`cajeta.script.In_3_::__cajeta_script_entry()`),
    // so an exact lookup of the bare name never matches; lookupShort's
    // `::name(` scan does, and prefers the newest unit class — this cell.
    // Missing it is a HARD failure: the previous version skipped execution
    // silently and still reported ok, so every cell compiled, ran nothing,
    // and looked successful. Statics stayed 0 not because the session seam
    // leaked them but because no assignment had ever executed.
    void* entry = lookupShort(scriptEntryName());
    if (!entry) {
        // A DECLARATION-ONLY cell (`public class Foo { ... }` and nothing
        // else) has no loose statements, so it is an ORDINARY unit and no
        // entry is synthesized (script-units 2.4). That is a perfectly
        // normal notebook cell: it defines a type for later cells and has
        // nothing to run. Only a cell that IS a script unit must have an
        // entry — there, a missing one is the silent-skip bug that let
        // every cell "succeed" while executing nothing.
        if (cellModule && cellModule->isScriptUnit()) {
            result.errorId = "CAJETA_ERROR_INTERNAL";
            result.message =
                "script cell entry symbol not found after materialization";
            return result;
        }
        result.ok = true;
        return result;
    }
    result.value = reinterpret_cast<int32_t (*)()>(entry)();
    result.ok = true;
    return result;
}

void* KernelSession::lookupSymbol(const std::string& exactName) {
    Impl& impl = *impl_;
    // Newest cell first: a redefined name resolves to its newest definition.
    for (auto it = impl.cellJDs.rbegin(); it != impl.cellJDs.rend(); ++it) {
        if (auto sym = impl.jit->lookup(**it, exactName)) {
            return reinterpret_cast<void*>(sym->getValue());
        } else {
            llvm::consumeError(sym.takeError());
        }
    }
    if (impl.bootstrapJD) {
        if (auto sym = impl.jit->lookup(*impl.bootstrapJD, exactName)) {
            return reinterpret_cast<void*>(sym->getValue());
        } else {
            llvm::consumeError(sym.takeError());
        }
    }
    return nullptr;
}

void* KernelSession::lookupShort(const std::string& shortName) {
    // Exact first: runtime symbols and anything unmangled.
    if (void* exact = lookupSymbol(shortName)) return exact;
    Impl& impl = *impl_;

    // Candidates come from the ACTUAL emitted IR, not from a reconstructed
    // mangling: Method::getLlvmSymbolName() does not match the symbol ORC
    // resolves, and building the name by hand returned null for every short
    // lookup. Scan `pkg.Class::name(params)` shapes and keep the owning
    // class, so the choice among several definitions can be ORDERED.
    std::map<std::string, std::string> byOwner;   // class canonical -> symbol
    std::vector<std::string> anyOwner;
    for (auto& m : impl.compiler->getModules()) {
        if (!m || !m->getLlvmModule()) continue;
        if (impl.poisoned.count(m->getLlvmModule())) continue;
        for (auto& F : *m->getLlvmModule()) {
            llvm::StringRef n = F.getName();
            size_t sep = n.find("::");
            if (sep == llvm::StringRef::npos) continue;
            llvm::StringRef after = n.substr(sep + 2);
            size_t paren = after.find('(');
            if (paren == llvm::StringRef::npos) continue;
            if (after.substr(0, paren) != shortName) continue;
            byOwner.emplace(n.substr(0, sep).str(), n.str());
            anyOwner.push_back(n.str());
        }
    }
    // Prefer the NEWEST unit class that defines the name (script-units 5.2
    // last-write-wins). Scanning in module order returned the OLDEST
    // definition, which made redefinition look broken through a direct
    // lookup even though calls resolved correctly.
    const auto& units = impl.sessionState.getUnitClasses();
    for (auto it = units.rbegin(); it != units.rend(); ++it) {
        auto f = byOwner.find(*it);
        if (f == byOwner.end()) continue;
        if (void* hit = lookupSymbol(f->second)) return hit;
    }
    for (auto& c : anyOwner) {
        if (void* hit = lookupSymbol(c)) return hit;
    }
    return nullptr;
}

void KernelSession::shutdown() {
    Impl& impl = *impl_;
    if (impl.shutdownDone) return;
    impl.shutdownDone = true;
    // Join carriers while the code they may re-enter is still live — the
    // ordering constraint CajetaJit's destructor documents. Exactly once for
    // the session: a per-cell shutdown would tear the shared pool out from
    // under later cells.
    if (impl.jit) {
        if (void* fn = lookupSymbol("__cajeta_task_shutdown")) {
            reinterpret_cast<void (*)()>(fn)();
            ++impl.stats.taskShutdownCalls;
        }
    }
    Compiler::setSharedContext(nullptr);
    std::error_code ec;
    std::filesystem::remove_all(impl.scratchRoot, ec);
}

const SessionStats& KernelSession::stats() const { return impl_->stats; }

}  // namespace cajeta::kernel
