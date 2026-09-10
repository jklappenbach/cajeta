#include "cajeta/kernel/KernelSession.h"
#include "cajeta/jit/CajetaJitWinSymbols.h"

#include "cajeta/jit/JitCoffLinking.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
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
#include "cajeta/dap/Json.h"
#include "cajeta/error/DiagnosticEngine.h"
#include "cajeta/error/Diagnostics.h"
#include "cajeta/error/Exception.h"
#include "cajeta/compile/ScriptUnitSynthesis.h"
#include "cajeta/buildtool/ArtifactCache.h"
#include "cajeta/buildtool/Dependency.h"
#include "cajeta/buildtool/Manifest.h"
#include "cajeta/buildtool/ManifestEditor.h"
#include "cajeta/buildtool/KeyRevocation.h"
#include "cajeta/buildtool/OrgKeyCache.h"
#include "cajeta/buildtool/PublisherVerification.h"
#include "cajeta/buildtool/ReleaseIntegrity.h"
#include "cajeta/buildtool/Repository.h"
#include "cajeta/buildtool/Resolver.h"
#include "cajeta/buildtool/Signature.h"
#include "cajeta/cli/TrustStore.h"
#include "cajeta/jit/JitModulePrep.h"
#include "cajeta/jit/CajetaSymbolIndex.h"
#include "cajeta/compile/CajetaArchive.h"
#include "cajeta/jit/CajetaDefinitionGenerator.h"
#include "cajeta/jit/CajetaLazyEmitter.h"
#include "cajeta/jit/LazyCodegen.h"
#include "cajeta/compile/ReflectionKeepSet.h"
#include "cajeta/util/FdCapture.h"

namespace cajeta::kernel {

// The runtime's install bridge. Declared here rather than in a header: the C
// side is the runtime's, and the kernel is only one of its hosts.
extern "C" void __cajeta_session_set_install_hook(
    int32_t (*fn)(const char* name, int32_t nameLen,
                  const char* constraint, int32_t constraintLen,
                  int32_t save, char* out, int32_t outCap, void* ctx),
    void* ctx);

// The bridge's state lives HERE, in the host: the runtime is compiled twice —
// into this binary, and into the bitcode embedded in every JIT session — so a
// definition on the runtime side would give cell code a second, unseen copy.
extern "C" {
__attribute__((used, retain, visibility("default")))
int32_t (*__cajeta_install_hook)(const char*, int32_t, const char*, int32_t,
                                 int32_t, char*, int32_t, void*) = nullptr;
__attribute__((used, retain, visibility("default")))
void* __cajeta_install_ctx = nullptr;
__attribute__((used, retain, visibility("default")))
char __cajeta_install_out[2048] = {0};
}

std::string projectDirForLaunch(const std::string& cwd) {
    std::filesystem::path dir(cwd);
    while (true) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(dir / "cajeta.json", ec)) {
            return dir.string();
        }
        auto parent = dir.parent_path();
        if (parent.empty() || parent == dir) return cwd;
        dir = parent;
    }
}

namespace {

    // The session whose cell is executing right now; JIT'd `Packages.install`
    // reaches its host through here. Single-threaded by contract.
    thread_local KernelSession* g_activeSession = nullptr;

    // True when `version` satisfies `constraint`; empty or "*" accepts any.
    // Delegates to the buildtool's matcher: one implementation of the grammar.
    bool versionSatisfies(const std::string& version,
                          const std::string& constraint) {
        if (constraint.empty() || constraint == "*") return true;
        return cajeta::buildtool::versionSatisfies(version, constraint);
    }

    // Copy `text` into `out` (capacity `cap`), truncated and NUL-terminated.
    void writeOut(char* out, int32_t cap, const std::string& text) {
        if (!out || cap <= 0) return;
        auto n = std::min<size_t>(text.size(), (size_t) cap - 1);
        std::memcpy(out, text.data(), n);
        out[n] = '\0';
    }

    // --- the compiler-jsonl bridge: cell diagnostics as NDJSON on stderr ----

    // The envelope major this kernel understands; an unknown major is refused
    // whole rather than half-read.
    constexpr int kSupportedJsonlMajor = 1;

    // Scopes the process-wide JSON-progress switch to one cell.
    struct JsonGateScope {
        bool prev;
        explicit JsonGateScope(bool on) : prev(cajeta::jsonProgressEnabled()) {
            cajeta::setJsonProgressEnabled(on);
        }
        ~JsonGateScope() { cajeta::setJsonProgressEnabled(prev); }
    };

    // Parse one cell's captured stderr into `out`. Lines that are not JSON are
    // compiler chatter with no structured form; they go to `passthrough` for
    // the caller to re-emit rather than being swallowed.
    void parseJsonlDiagnostics(const std::string& buffer,
                               std::vector<CellDiagnostic>* out,
                               std::string* passthrough) {
        size_t pos = 0;
        bool refused = false;
        while (pos <= buffer.size()) {
            size_t nl = buffer.find('\n', pos);
            std::string line = buffer.substr(
                pos, nl == std::string::npos ? std::string::npos : nl - pos);
            pos = (nl == std::string::npos) ? buffer.size() + 1 : nl + 1;
            if (line.empty()) continue;
            bool ok = false;
            cajeta::dap::Json rec = cajeta::dap::Json::parse(line, &ok);
            if (!ok || !rec.isObject() || !rec.has("kind")) {
                if (passthrough) { *passthrough += line; *passthrough += '\n'; }
                continue;
            }
            if (refused) continue;
            const std::string& kind = rec.at("kind").asString();
            if (kind == "stream") {
                if (rec.at("major").asInt(kSupportedJsonlMajor)
                        != kSupportedJsonlMajor) {
                    refused = true;
                    if (passthrough) {
                        *passthrough += "cajeta kernel: unsupported diagnostic "
                                        "stream major; diagnostics for this "
                                        "cell were not read\n";
                    }
                }
                continue;
            }
            if (kind != "diagnostic") continue;
            CellDiagnostic d;
            d.severity = rec.at("severity").asString();
            d.code = rec.at("code").asString();
            d.message = rec.at("message").asString();
            d.file = rec.at("file").asString();
            d.line = rec.at("line").asInt(0);
            d.column = rec.at("column").asInt(0);
            if (d.severity.empty()) d.severity = "error";
            out->push_back(std::move(d));
        }
    }

    // Append to `out` the classpath of `dir`'s nearest ancestor `cajeta.json` —
    // its resolved manifest dependencies, exactly what `cajeta build` passes.
    // No manifest anywhere up the tree is a stdlib-only session, not an error.
    bool resolveProjectClasspath(const std::string& dir,
                                 std::vector<std::string>* out,
                                 std::string* error) {
        std::error_code ec;
        std::filesystem::path start = std::filesystem::absolute(dir, ec);
        if (ec) {
            if (error) *error = "bad project directory: " + dir;
            return false;
        }
        std::filesystem::path projectRoot;
        for (std::filesystem::path d = start;;  d = d.parent_path()) {
            if (std::filesystem::exists(d / "cajeta.json")) { projectRoot = d; break; }
            if (d == d.root_path() || d.empty()) break;
        }
        if (projectRoot.empty()) return true;

        auto manifest = cajeta::buildtool::loadManifestFile(
            (projectRoot / "cajeta.json").string());
        if (!manifest) {
            if (error) {
                *error = "bad manifest at " + projectRoot.string() + ": "
                       + llvm::toString(manifest.takeError());
            }
            return false;
        }
        auto resolved = cajeta::buildtool::resolveProjectDependencies(
            *manifest, projectRoot.string());
        if (!resolved) {
            if (error) {
                *error = "dependency resolution failed: "
                       + llvm::toString(resolved.takeError());
            }
            return false;
        }
        for (const auto& dep : *resolved) {
            if (!dep.artifactPath.empty()) out->push_back(dep.artifactPath);
        }
        return true;
    }

    // The cell's IDENTIFIER: the script unit's implicit class name, so it must
    // be a legal Cajeta identifier, and it surfaces in mangled symbols and JIT
    // errors. "In[3]" becomes `cell_3`; any other name is sanitized.
    std::string stemFor(const std::string& cellName) {
        if (cellName.size() > 3 && cellName.compare(0, 3, "In[") == 0
                && cellName.back() == ']') {
            std::string n = cellName.substr(3, cellName.size() - 4);
            if (!n.empty() && std::all_of(n.begin(), n.end(), [](unsigned char c) {
                    return std::isdigit(c) != 0;
                })) {
                return "cell_" + n;
            }
        }
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
    // Oldest cell first; lookup walks it BACKWARDS, so newest definition wins.
    std::vector<llvm::orc::JITDylib*> cellJDs;

    // ONE Compiler for the session: cell N must see the types and methods that
    // cells 1..N-1 registered, which live in its type world, not in the JIT.
    std::unique_ptr<Compiler> compiler;
    // The ownership table, carried across cell compiles.
    SessionState sessionState;

    std::filesystem::path scratchRoot;
    KernelSession::StreamHandler streamHandler;
    // The interrupt seam, resolved once on the EXECUTION thread at the first
    // cell: plain pointers, so another thread can set the flag without the JIT.
    void (*requestInterruptFn)() = nullptr;
    void (*clearInterruptFn)() = nullptr;
    void* (*interruptMarker)() = nullptr;
    // The would-be-UB trap's sentinel and its description.
    void* (*trapMarker)() = nullptr;
    const char* (*trapDescription)() = nullptr;
    SessionStats stats;
    int execCount = 0;
    bool shutdownDone = false;

    // Verified organization key documents, cached across a session's installs.
    // Built on first use: a session that never installs never reads the store.
    std::unique_ptr<cajeta::buildtool::OrgKeyCache> orgKeys;

    // Modules already delivered to the JIT, by IR-module pointer: a cell can
    // emit into the stdlib module, so identity decides what is new this cell.
    std::set<llvm::Module*> delivered;
    // Modules of a cell that FAILED to compile: they keep half-built methods, so
    // a later fixpoint would re-run them and re-throw. Skipped forever.
    std::set<llvm::Module*> poisoned;
    // Mangled symbol -> method, rebuilt per cell because codegen instantiates
    // templates and so defines new methods.
    CajetaSymbolIndex symbolIndex;
    // llvm::Modules that came from a CLASSPATH ARCHIVE, not from this session's
    // codegen: a verify failure in one is a dependency's problem, not ours.
    std::set<llvm::Module*> prebuilt;
    // Canonical paths of archives spliced mid-session — the idempotence key.
    std::set<std::string> installedArchives;
    // The project governing this session; an install reads its
    // `settings.repositories` long after create() ran.
    std::string projectDir;
    // What each install DECLARED, so a re-install is judged against the loaded
    // version rather than the path it arrived by.
    struct InstallRecord {
        std::string version;
        std::string path;
    };
    std::map<std::string, InstallRecord> installsByName;
    // Archives acquired by the CURRENTLY executing cell. A cell cannot import
    // what it just installed, and this is what lets the failure say so.
    std::vector<std::string> installedThisCell;
    // A splice runs compiler passes a half-executed cell cannot host, so mid-cell
    // requests queue here and drain at the cell boundary.
    bool cellExecuting = false;
    std::vector<std::string> pendingInstalls;
    // Ctor functions already delivered in an init-extract: an accumulating module
    // is never delivered whole under lazy, only its ctor DELTA.
    std::set<std::string> deliveredCtors;
    // The session's DefinitionGenerator, owned by the main JITDylib; held
    // raw for stats only (generatedCount -> lazyBodiesDelivered).
    cajeta::CajetaDefinitionGenerator* lazyGenerator = nullptr;
    // Globals DEFINED by an already-delivered cell. Statics are session-lived, so
    // a later cell must REFERENCE the storage, never emit its own zero copy.
    std::set<std::string> definedGlobals;

    // Per-cell link order: self, newest prior cells, bootstrap, process dylib
    // LAST — the default binds user code to the runtime's NATIVE copy.
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
    return create(SessionOptions{}, error);
}

std::unique_ptr<KernelSession> KernelSession::create(const SessionOptions& options,
                                                     std::string* error) {
    ensureTargetsInitialized();
    auto setErr = [&](const std::string& m) {
        if (error) *error = m;
        return nullptr;
    };

    std::unique_ptr<KernelSession> s(new KernelSession);
    Impl& impl = *s->impl_;
    auto note = [&](const std::string& phase) {
        if (options.progress) options.progress(phase);
    };

    // COFF: RuntimeDyld's default object layer aborts the process on
    // IMAGE_REL_AMD64_ADDR32NB (see JitCoffLinking.h).
    llvm::orc::LLJITBuilder builder;
    cajeta::jit::applyCoffJitLink(builder);
    auto jitOrErr = builder.create();
    if (!jitOrErr) {
        return setErr("LLJIT create failed: "
                      + llvm::toString(jitOrErr.takeError()));
    }
    impl.jit = std::move(*jitOrErr);

    auto& mainJD = impl.jit->getMainJITDylib();
    // Added FIRST, so a host library sharing a method's name can never shadow
    // a body we can generate. Dark until lazy mode is on.
    {
        Impl* ip = &impl;
        auto gen = std::make_unique<cajeta::CajetaDefinitionGenerator>(
            impl.symbolIndex,
            [ip](llvm::orc::ThreadSafeModule tsm,
                 llvm::orc::JITDylib& jd) -> llvm::Error {
                return ip->jit->addIRModule(jd, std::move(tsm));
            });
        impl.lazyGenerator = gen.get();
        mainJD.addGenerator(std::move(gen));
    }

    // Process symbols — the native runtime the JIT'd code calls into, and the
    // last-resort resolver for every cell.
    auto generator = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
        impl.jit->getDataLayout().getGlobalPrefix());
    if (!generator) {
        return setErr("process-symbol generator failed: "
                      + llvm::toString(generator.takeError()));
    }
    mainJD.addGenerator(std::move(*generator));

    // Windows symbol bridge: a PE exports nothing, so the process generator
    // cannot see the static CRT/libm/cajeta-native symbols. No-op elsewhere.
    {
        size_t winSymCount = 0;
        const cajeta::jit::JitWinSym* winSyms =
            cajeta::jit::winJitSymbols(&winSymCount);
        if (winSymCount) {
            auto& execSession = impl.jit->getExecutionSession();
            llvm::orc::SymbolMap winSymMap;
            for (size_t i = 0; i < winSymCount; ++i) {
                winSymMap[execSession.intern(winSyms[i].name)] =
                    llvm::orc::ExecutorSymbolDef(
                        llvm::orc::ExecutorAddr::fromPtr(winSyms[i].addr),
                        llvm::JITSymbolFlags::Exported);
            }
            if (auto err = mainJD.define(
                    llvm::orc::absoluteSymbols(std::move(winSymMap)))) {
                return setErr("windows symbol bridge define failed: "
                              + llvm::toString(std::move(err)));
            }
        }
    }

    auto bootstrapOrErr = impl.jit->createJITDylib("CajetaBootstrap");
    if (!bootstrapOrErr) {
        return setErr("bootstrap dylib create failed: "
                      + llvm::toString(bootstrapOrErr.takeError()));
    }
    impl.bootstrapJD = &*bootstrapOrErr;
    impl.bootstrapJD->addToLinkOrder(mainJD);

    // The project classpath, resolved FIRST because it decides how the stdlib
    // is built. Pure manifest and file work; no compiler needed yet.
    std::vector<std::string> archives;
    impl.projectDir = options.projectDir;
    if (!options.projectDir.empty()) {
        note("resolving project dependencies");
        std::string resolveError;
        if (!resolveProjectClasspath(options.projectDir, &archives,
                                     &resolveError)) {
            return setErr(resolveError);
        }
    }
    archives.insert(archives.end(), options.classpath.begin(),
                    options.classpath.end());

    // An archive already on the classpath at session start IS loaded, so record
    // it as an install: a re-install of a pinned dependency must be a no-op.
    for (const auto& path : archives) {
        std::error_code archEc;
        auto canon = std::filesystem::weakly_canonical(path, archEc);
        if (archEc) continue;
        try {
            auto archive = CajetaArchive::readFrom(canon.string());
            impl.installedArchives.insert(canon.string());
            impl.installsByName[archive.getName()] =
                Impl::InstallRecord{archive.getVersion(), canon.string()};
        } catch (std::exception&) {
            // Unreadable here is not this code's problem to report: the
            // ingest below fails with a far better message.
        }
    }

    // RESIDENT STDLIB ONLY WHEN THERE IS NO CLASSPATH: restoring the baseline and
    // then splicing archives builds them against a stdlib they never saw.
    const bool useResidentStdlib = archives.empty();

    note(useResidentStdlib ? "priming stdlib" : "building stdlib for classpath");
    try {
        if (useResidentStdlib) {
            auto& core = StdlibReuseCore::instance();
            core.ensurePrimed();
            core.restoreBaseline();
            Compiler::setSharedContext(core.context());
        } else {
            Compiler::setSharedContext(nullptr);
        }
        impl.compiler = std::make_unique<Compiler>();
        impl.compiler->setMode(CompilerMode::Debug);
        impl.compiler->ensureStdlibModule();
    } catch (cajeta::Exception& e) {
        Compiler::setSharedContext(nullptr);
        return setErr(std::string("stdlib prime failed: ") + e.getErrorId()
                      + ": " + e.getMessage());
    }

    // Ingested ONCE, BEFORE any cell is parsed: dependency classes have to be
    // visible while a cell's own imports resolve.
    {
        if (!archives.empty()) {
            note("ingesting " + std::to_string(archives.size())
                 + (archives.size() == 1 ? " dependency archive"
                                         : " dependency archives"));
            for (const auto& cp : archives) impl.compiler->addClasspath(cp);
            try {
                impl.compiler->ingestClasspath();
                // Definitions, not just declarations — the JIT links what it
                // RUNS, and the ingest alone leaves dep symbols unresolved.
                impl.compiler->linkClasspathModules();
                for (auto& m : impl.compiler->getModules()) {
                    if (!m || !m->getLlvmModule()) continue;
                    // NOT the stdlib: it predates the ingest and is this session's
                    // own work, so it belongs on both sides of the check.
                    if (m->getLlvmModule()->getModuleIdentifier()
                            == "cajeta.runtime.__stdlib__") {
                        continue;
                    }
                    impl.prebuilt.insert(m->getLlvmModule());
                }
            } catch (cajeta::Exception& e) {
                Compiler::setSharedContext(nullptr);
                return setErr(std::string("classpath ingest failed: ")
                              + e.getErrorId() + ": " + e.getMessage());
            } catch (std::exception& e) {
                Compiler::setSharedContext(nullptr);
                return setErr(std::string("classpath ingest failed: ") + e.what());
            }
        }
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

namespace {

// The host end of `Packages.install`, reached from JIT'd cell code through the
// runtime bridge. The version and conflict arms are answered HERE, before the
// splice; the splice itself queues to the cell boundary.
int32_t sessionInstallHook(const char* name, int32_t nameLen,
                           const char* constraint, int32_t constraintLen,
                           int32_t save, char* out, int32_t outCap,
                           void* ctx) {
    auto* session = static_cast<KernelSession*>(ctx);
    std::string request(name ? name : "", nameLen > 0 ? nameLen : 0);
    std::string want(constraint ? constraint : "",
                     constraintLen > 0 ? constraintLen : 0);

    if (!session || session != g_activeSession) {
        writeOut(out, outCap,
                 "Packages.install: no live session — installing into a "
                 "running session requires a session host (the Jupyter "
                 "kernel); declare the dependency in cajeta.json instead");
        return 1;
    }
    return session->installFromHook(request, want, save != 0, out, outCap)
        ? 0 : 1;
}

}  // namespace

bool KernelSession::verifySignatureOrFail(
        const std::string& archivePath, const std::string& name,
        const std::string& version,
        const cajeta::buildtool::Repository& repo,
        const std::string& owningOrganization,
        const std::string& signature,
        const cajeta::buildtool::RepositoryDelegation* delegation,
        const std::function<void(const std::string&)>& phase,
        std::string* errorOut) {
    namespace bt = cajeta::buildtool;
    Impl& impl = *impl_;
    const std::string repoName = repo.name();
    if (signature.empty()) return true;      // policy already decided above

    phase("checking signature for " + name + " " + version);
    auto layout = cajeta::cli::resolveTrustStoreLayout();

    if (!owningOrganization.empty()) {
        if (!impl.orgKeys) {
            impl.orgKeys = std::make_unique<bt::OrgKeyCache>(
                cajeta::cli::rootTrustLayoutOf(layout));
        }
        std::time_t now = std::time(nullptr);
        auto doc = impl.orgKeys->documentFor(repo, owningOrganization, now);
        if (!doc) {
            if (errorOut) {
                *errorOut = "Packages.install: the key document for '"
                          + owningOrganization + "' from " + repoName
                          + " could not be used: "
                          + llvm::toString(doc.takeError())
                          + " Nothing was installed.";
            } else {
                llvm::consumeError(doc.takeError());
            }
            return false;
        }
        if (doc->has_value()) {
            // The revocation statement, consulted BEFORE the signature check and
            // failing CLOSED: failing open would un-revoke every key.
            auto revocation = bt::revocationFor(repo, delegation, now, 0);
            if (!revocation) {
                if (errorOut) {
                    *errorOut = "Packages.install: "
                              + llvm::toString(revocation.takeError())
                              + " Nothing was installed.";
                } else {
                    llvm::consumeError(revocation.takeError());
                }
                return false;
            }

            phase("verifying publisher " + owningOrganization);
            auto verdict = bt::verifyAgainstOrgDocument(
                **doc, name, archivePath, signature, now,
                revocation->has_value() ? &**revocation : nullptr);
            if (!verdict.ok()) {
                if (errorOut) {
                    *errorOut = "Packages.install: " + verdict.message
                              + " Nothing was installed.";
                }
                return false;
            }
            // The document DECIDES: falling through to the trust store on a
            // failure is the bypass, so return here either way.
            return true;
        }
        // No document: degrade to the trust store rather than refusing.
    }

    std::vector<std::string> keys;
    for (const auto& entry : cajeta::cli::listTrustedKeys(layout)) {
        keys.push_back(entry.path);
    }
    if (keys.empty()) {
        if (errorOut) {
            *errorOut = "Packages.install: '" + name + "' " + version
                      + " is signed, but this machine trusts no signing "
                        "keys, so the signature cannot be checked. Add the "
                        "publisher's key with `cajeta trust add`.";
        }
        return false;
    }

    auto verified = bt::verifyAgainstAnyKey(archivePath, signature, keys);
    if (!verified) {
        llvm::consumeError(verified.takeError());
        if (errorOut) {
            *errorOut = "Packages.install: the signature for '" + name
                      + "' " + version + " could not be checked.";
        }
        return false;
    }
    if (!verified->has_value()) {
        if (errorOut) {
            *errorOut = "Packages.install: the signature for '" + name + "' "
                      + version + " from " + repoName + " does not match any "
                        "trusted key (" + std::to_string(keys.size())
                      + " checked). Nothing was installed. If this publisher "
                        "is new, add their key with `cajeta trust add`.";
        }
        return false;
    }
    return true;
}

bool KernelSession::resolveForInstall(
        const std::string& name, const std::string& constraint,
        const std::function<void(const std::string&)>& phase,
        std::string* pathOut, std::string* versionOut,
        std::string* errorOut) {
    namespace bt = cajeta::buildtool;
    Impl& impl = *impl_;
    auto fail = [&](const std::string& m) {
        if (errorOut) *errorOut = m;
        return false;
    };

    std::vector<bt::RepositorySpec> specs;
    bool requireSignatures = false;
    std::string projectRoot = impl.projectDir;
    if (!projectRoot.empty()) {
        auto manifestPath =
            (std::filesystem::path(projectRoot) / "cajeta.json").string();
        if (std::filesystem::exists(manifestPath)) {
            auto m = bt::loadManifestFile(manifestPath);
            if (!m) {
                llvm::consumeError(m.takeError());
                return fail("Packages.install: the governing project's "
                            "cajeta.json could not be read: " + manifestPath);
            }
            auto parsed = bt::parseRepositories(*m);
            if (!parsed) {
                llvm::consumeError(parsed.takeError());
                return fail("Packages.install: settings.repositories in "
                            + manifestPath + " could not be parsed");
            }
            specs = *parsed;
            if (auto b = m->settingsRaw.getBoolean("require-signatures")) {
                requireSignatures = *b;
            }
        }
    }
    if (specs.empty()) {
        bt::RepositorySpec central;
        central.name = "central";
        central.type = "http";
        central.url = "https://olla.cajeta.dev";
        specs.push_back(central);
    }

    std::string stage = projectRoot.empty()
        ? (std::filesystem::temp_directory_path() / "cajeta-session-downloads")
              .string()
        : (std::filesystem::path(projectRoot) / ".cajeta" / "cache"
           / "downloads").string();
    auto repos = bt::buildRepositories(specs, stage);
    if (!repos) {
        llvm::consumeError(repos.takeError());
        return fail("Packages.install: the session's repositories could not "
                    "be opened");
    }

    phase("resolving " + name + " " + constraint);
    std::vector<std::string> consulted;
    bt::RepositoryPtr chosen;
    std::string chosenVersion;
    for (const auto& repo : *repos) {
        consulted.push_back(repo->name());
        auto versions = repo->listVersions(name);
        if (!versions) {
            llvm::consumeError(versions.takeError());
            continue;
        }
        for (const auto& v : *versions) {
            if (!versionSatisfies(v, constraint)) continue;
            if (chosenVersion.empty()
                || bt::compareVersions(v, chosenVersion) > 0) {
                chosenVersion = v;
                chosen = repo;
            }
        }
        if (chosen) break;
    }
    if (!chosen) {
        std::string where;
        for (const auto& c : consulted) {
            if (!where.empty()) where += ", ";
            where += c;
        }
        return fail("Packages.install: no version of '" + name
                    + "' satisfies '" + constraint + "'. Repositories "
                      "consulted: " + (where.empty() ? "(none)" : where));
    }

    bt::ArtifactCache cache(projectRoot.empty() ? stage : projectRoot);

    // Resolved BEFORE the cache arm, so a cached artifact is held to the same
    // policy as a fetched one: a cache hit skips the network, not the checks.
    std::string signature;
    if (auto ps = chosen->publishedSignature(name, chosenVersion)) {
        if (ps->has_value()) signature = **ps;
    } else {
        llvm::consumeError(ps.takeError());
    }

    // The hash this install is held to and WHO owns the name, from the signed
    // release metadata. Before the cache arm, for the same reason.
    auto roots = bt::rootsFor(
        cajeta::cli::rootTrustLayoutOf(cajeta::cli::resolveTrustStoreLayout()),
        chosen->name());
    std::string published;
    bool signedHash = false;
    std::string owningOrganization;
    if (!roots) {
        return fail("Packages.install: the trust anchors for repository '"
                    + chosen->name() + "' could not be resolved: "
                    + llvm::toString(roots.takeError()));
    }
    // The repository's delegation, resolved before the metadata so release
    // signatures are checked against the online release key, not the root.
    std::time_t verifyAt = std::time(nullptr);
    if (!impl.orgKeys) {
        impl.orgKeys = std::make_unique<bt::OrgKeyCache>(
            cajeta::cli::rootTrustLayoutOf(
                cajeta::cli::resolveTrustStoreLayout()));
    }
    std::optional<bt::RepositoryDelegation> delegation;
    if (auto d = impl.orgKeys->delegationFor(*chosen, verifyAt)) {
        delegation = *d;
    } else {
        return fail("Packages.install: the delegation for repository '"
                    + chosen->name() + "' could not be used: "
                    + llvm::toString(d.takeError()));
    }

    if (auto integrity = bt::releaseIntegrityFor(
            *chosen, name, chosenVersion, *roots,
            delegation ? &*delegation : nullptr, verifyAt)) {
        published = integrity->sha256;
        signedHash = integrity->fromSignedMetadata;
        // Only a root-SIGNED organization counts. An unsigned one is
        // written as freely as a name prefix by anyone in the path.
        if (integrity->fromSignedMetadata) {
            owningOrganization = integrity->organization;
        }
        // Retraction WARNS and does not refuse: a lockfile pinning this version
        // must keep resolving. Say whether the withdrawal was itself signed.
        if (integrity->retracted) {
            std::string why = integrity->retractedReason.empty()
                                  ? std::string("no reason given")
                                  : integrity->retractedReason;
            cajeta::logLine("warn",
                "[packages] '" + name + "' " + chosenVersion + " from "
                + chosen->name() + " is RETRACTED by its publisher: " + why
                + (integrity->fromSignedMetadata
                       ? ""
                       : " (unsigned metadata — this withdrawal is not"
                         " signed, so treat it as advisory)")
                + "\n");
        }
    } else {
        // Metadata that is present and does not verify is a refusal: a mirror
        // able to strip a signature would make the whole chain optional.
        return fail("Packages.install: " + llvm::toString(integrity.takeError())
                    + " Nothing was installed.");
    }

    if (signature.empty() && requireSignatures) {
        return fail("Packages.install: '" + name + "' " + chosenVersion
                    + " from " + chosen->name() + " publishes no signature, "
                      "and this project sets require-signatures. Install a "
                      "signed release, or drop require-signatures to accept "
                      "checksum-only verification.");
    }

    if (!published.empty()) {
        if (auto hit = cache.lookup(published)) {
            if (!verifySignatureOrFail(*hit, name, chosenVersion, *chosen,
                                       owningOrganization, signature,
                                       delegation ? &*delegation : nullptr,
                                       phase,
                                       errorOut)) {
                return false;
            }
            phase("cached " + name + " " + chosenVersion);
            if (pathOut) *pathOut = *hit;
            if (versionOut) *versionOut = chosenVersion;
            return true;
        }
    }

    phase("fetching " + name + " " + chosenVersion + " from "
          + chosen->name());
    auto fetched = chosen->fetch(name, chosenVersion);
    if (!fetched) {
        llvm::consumeError(fetched.takeError());
        return fail("Packages.install: '" + name + "' " + chosenVersion
                    + " could not be fetched from " + chosen->name()
                    + ". Cache checked: " + cache.projectCacheDir());
    }

    if (!published.empty()) {
        phase("verifying " + name + " " + chosenVersion);
        std::string actual = bt::ArtifactCache::sha256OfFile(*fetched);
        if (actual != published) {
            std::error_code rm;
            std::filesystem::remove(*fetched, rm);
            return fail("Packages.install: checksum mismatch for '" + name
                        + "' " + chosenVersion + " from " + chosen->name()
                        + " — " + (signedHash ? "the root-signed release "
                                                "metadata says "
                                              : "the published checksum says ")
                        + published + ", the bytes hash to "
                        + (actual.empty() ? std::string("nothing") : actual)
                        + ". The download was discarded and nothing was "
                          "installed.");
        }
        if (!verifySignatureOrFail(*fetched, name, chosenVersion, *chosen,
                                   owningOrganization, signature,
                                   delegation ? &*delegation : nullptr, phase,
                                   errorOut)) {
            std::error_code rm;
            std::filesystem::remove(*fetched, rm);
            return false;
        }
        if (auto stored = cache.insert(*fetched)) {
            if (pathOut) *pathOut = *stored;
            if (versionOut) *versionOut = chosenVersion;
            return true;
        } else {
            llvm::consumeError(stored.takeError());
        }
    } else if (!verifySignatureOrFail(*fetched, name, chosenVersion, *chosen,
                                      owningOrganization, signature,
                                      delegation ? &*delegation : nullptr,
                                      phase, errorOut)) {
        std::error_code rm;
        std::filesystem::remove(*fetched, rm);
        return false;
    }

    if (pathOut) *pathOut = *fetched;
    if (versionOut) *versionOut = chosenVersion;
    return true;
}

bool KernelSession::saveToManifest(
        const std::string& name, const std::string& constraint,
        const std::function<void(const std::string&)>& phase,
        std::string* errorOut) {
    namespace bt = cajeta::buildtool;
    Impl& impl = *impl_;
    auto fail = [&](const std::string& m) {
        if (errorOut) *errorOut = m;
        return false;
    };

    if (impl.projectDir.empty()) {
        return fail("Packages.installAndSave: no project governs this "
                    "session, so there is no cajeta.json to write. Start the "
                    "kernel in a project directory, or create one with "
                    "`cajeta init notebook`. (Packages.install still works — "
                    "it just does not survive a restart.)");
    }
    auto manifestPath =
        (std::filesystem::path(impl.projectDir) / "cajeta.json").string();
    if (!std::filesystem::exists(manifestPath)) {
        return fail("Packages.installAndSave: no cajeta.json at "
                    + impl.projectDir + " — create one with "
                      "`cajeta init notebook`.");
    }

    // An unchanged pin writes NOTHING: the same bytes still churn mtime.
    auto current = bt::loadManifestFile(manifestPath);
    if (!current) {
        llvm::consumeError(current.takeError());
        return fail("Packages.installAndSave: " + manifestPath
                    + " could not be read.");
    }
    std::string previous;
    bool alreadyPinned = false;
    if (auto deps = bt::parseDependencies(*current)) {
        for (const auto& d : *deps) {
            if (d.name != name) continue;
            alreadyPinned = true;
            previous = d.versionConstraint;
            break;
        }
    } else {
        llvm::consumeError(deps.takeError());
    }
    if (alreadyPinned && previous == constraint) {
        phase("cajeta.json already pins " + name + " " + constraint);
        return true;
    }

    std::ifstream in(manifestPath, std::ios::binary);
    if (!in) return fail("Packages.installAndSave: cannot open "
                         + manifestPath);
    std::ostringstream buf;
    buf << in.rdbuf();
    in.close();

    auto rewritten = bt::addDependencyToManifest(buf.str(), name, constraint);
    if (!rewritten) {
        llvm::consumeError(rewritten.takeError());
        return fail("Packages.installAndSave: writing " + name + " to "
                    + manifestPath + " would not produce a valid manifest; "
                      "nothing was changed.");
    }

    std::ofstream outFile(manifestPath, std::ios::binary | std::ios::trunc);
    if (!outFile) return fail("Packages.installAndSave: cannot write "
                              + manifestPath);
    outFile << *rewritten;
    outFile.close();

    phase(alreadyPinned
              ? ("cajeta.json: " + name + " " + previous + " -> " + constraint)
              : ("cajeta.json: added " + name + " " + constraint));
    return true;
}

bool KernelSession::installFromHook(const std::string& request,
                                    const std::string& constraint,
                                    bool save,
                                    char* out, int32_t outCap) {
    Impl& impl = *impl_;

    auto phase = [](const std::string& text) {
        std::fputs(("  " + text + "\n").c_str(), stdout);
        std::fflush(stdout);
    };

    // Decided BEFORE anything is fetched or spliced: otherwise a successful
    // install would be reported as a failure with the archive already loaded.
    if (save && impl.projectDir.empty()) {
        writeOut(out, outCap,
                 "Packages.installAndSave: no project governs this session, "
                 "so there is no cajeta.json to write. Start the kernel in a "
                 "project directory, or create one with `cajeta init "
                 "notebook`. (Packages.install still works — it just does "
                 "not survive a restart.)");
        return false;
    }

    // Decided BEFORE resolution: with the library already loaded at an excluded
    // version, no answer the repositories give can change the outcome.
    {
        auto loadedIt = impl.installsByName.find(request);
        if (loadedIt != impl.installsByName.end()
            && !versionSatisfies(loadedIt->second.version, constraint)) {
            writeOut(out, outCap,
                     "Packages.install: '" + request + "' is already loaded "
                     "at " + loadedIt->second.version + ", which '"
                     + constraint + "' excludes. A session cannot replace a "
                     "loaded archive — restart the session to change "
                     "versions.");
            return false;
        }
    }

    std::error_code ec;
    auto canon = std::filesystem::weakly_canonical(request, ec);
    if (ec || !std::filesystem::exists(canon)) {
        std::string resolvedPath;
        std::string resolvedVersion;
        std::string failure;
        if (!resolveForInstall(request, constraint, phase, &resolvedPath,
                               &resolvedVersion, &failure)) {
            writeOut(out, outCap, failure);
            return false;
        }
        canon = std::filesystem::weakly_canonical(resolvedPath, ec);
        if (ec || !std::filesystem::exists(canon)) {
            writeOut(out, outCap,
                     "Packages.install: '" + request + "' resolved to "
                     + resolvedVersion + " but its archive is missing at "
                     + resolvedPath);
            return false;
        }
    }

    std::string archiveName;
    std::string archiveVersion;
    try {
        auto archive = CajetaArchive::readFrom(canon.string());
        archiveName = archive.getName();
        archiveVersion = archive.getVersion();
    } catch (std::exception& e) {
        writeOut(out, outCap,
                 std::string("Packages.install: cannot read archive '")
                 + canon.string() + "': " + e.what());
        return false;
    }

    // Judge the LOADED version against this constraint: a satisfying re-install
    // is a no-op, and an excluded one cannot be honoured without a restart.
    auto it = impl.installsByName.find(archiveName);
    if (it != impl.installsByName.end()) {
        const std::string& loaded = it->second.version;
        if (versionSatisfies(loaded, constraint)) {
            // A no-op INSTALL is still a real SAVE request: the manifest can
            // need the new constraint even when the loaded version satisfies it.
            if (save) {
                std::string saveError;
                if (!saveToManifest(archiveName, constraint, phase,
                                    &saveError)) {
                    writeOut(out, outCap, saveError);
                    return false;
                }
            }
            writeOut(out, outCap, loaded);
            return true;
        }
        writeOut(out, outCap,
                 "Packages.install: '" + archiveName + "' is already loaded "
                 "at " + loaded + ", which '" + constraint + "' excludes. A "
                 "session cannot replace a loaded archive — restart the "
                 "session to change versions.");
        return false;
    }

    if (!versionSatisfies(archiveVersion, constraint)) {
        writeOut(out, outCap,
                 "Packages.install: '" + archiveName + "' is available at "
                 + archiveVersion + ", which '" + constraint
                 + "' excludes; no other version was found.");
        return false;
    }

    // Reject BEFORE announcing the splice: installArchive runs this same scan,
    // but announcing "splicing X" and then refusing narrates a phantom act.
    std::string collision;
    if (collidesWithSession(canon.string(), &collision)) {
        writeOut(out, outCap, "Packages.install: " + collision);
        return false;
    }

    phase("splicing " + archiveName + " " + archiveVersion);
    std::string err;
    if (!installArchive(canon.string(), &err)) {
        writeOut(out, outCap, "Packages.install: " + err);
        return false;
    }
    impl.installsByName[archiveName] = Impl::InstallRecord{archiveVersion,
                                                           canon.string()};
    impl.installedThisCell.push_back(archiveName);

    // The manifest write comes LAST: the splice above is the part that cannot be
    // undone, so a failed save must not follow a claimed install.
    if (save) {
        std::string saveError;
        if (!saveToManifest(archiveName, constraint, phase, &saveError)) {
            writeOut(out, outCap, saveError);
            return false;
        }
    }
    writeOut(out, outCap, archiveVersion);
    return true;
}

CellResult KernelSession::execute(const std::string& source,
                                  const std::string& cellName) {
    CellResult result;
    result.file = cellName;
    Impl& impl = *impl_;
    // Deferred-splice bracket: see Impl::pendingInstalls.
    struct ExecGuard {
        KernelSession* s;
        Impl& impl;
        ExecGuard(KernelSession* s, Impl& impl) : s(s), impl(impl) {
            impl.cellExecuting = true;
            impl.installedThisCell.clear();
            g_activeSession = s;
            __cajeta_session_set_install_hook(&sessionInstallHook, s);
        }
        ~ExecGuard() {
            g_activeSession = nullptr;
            __cajeta_session_set_install_hook(nullptr, nullptr);
            impl.cellExecuting = false;
            auto pending = std::move(impl.pendingInstalls);
            impl.pendingInstalls.clear();
            for (auto& path : pending) {
                std::string err;
                if (!s->installArchive(path, &err)) {
                    cajeta::logLine("warn",
                        "[session] deferred install failed: " + err + "\n");
                }
            }
        }
    } execGuard(this, impl);
    ++impl.execCount;
    // Stamped before anything can fail: the counter advances on a failed cell
    // too, so `In[N]`/`Out[N]` never reuse a number.
    result.executionCount = impl.execCount;


    // The cell's source has to reach the compiler as a FILE: the script-unit stem
    // (and so the implicit class name) is path-derived.
    std::string stem = stemFor(cellName);
    std::filesystem::path cellPath =
        impl.scratchRoot / "src" / "cajeta" / "script" / (stem + ".cajeta");
    {
        std::ofstream out(cellPath);
        out << source;
    }

    // The kernel is the one host with a SHARED TYPE WORLD: this session owns
    // its LLVMContext and type registry for its whole life. A host that builds
    // a fresh world per unit must NOT set this — the type outlives its context.
    impl.sessionState.setSharedTypeWorld(true);
    impl.compiler->setSessionState(&impl.sessionState, cellName);

    // The diagnostics bridge is live for the whole compile; its destructor closes
    // it on EVERY exit path, including the catch blocks below.
    struct DiagBridge {
        CellResult& result;
        Compiler& compiler;
        std::string cellFile;
        DiagFormat priorFormat;
        std::string buffer;
        JsonGateScope gate;
        DiagnosticEngine engine;
        std::unique_ptr<cajeta::util::FdCapture> capture;
        bool finished = false;

        DiagBridge(CellResult& r, Compiler& c, std::string cellDisplayName)
            : result(r), compiler(c), cellFile(std::move(cellDisplayName)),
              priorFormat(c.getFlags().diagFormat), gate(true) {
            CompilerFlags f = compiler.getFlags();
            f.diagFormat = DiagFormat::Json;
            compiler.setFlags(f);
            // Warnings COLLECT, errors keep THROWING: a collected error would let
            // codegen run on into null types, and a cell must fail first.
            engine.setCollectErrors(false);
            DiagnosticEngine::setActive(&engine);
            capture = std::make_unique<cajeta::util::FdCapture>(
                2, [this](const std::string& chunk) { buffer += chunk; });
            cajeta::emitStreamRecord();
        }
        ~DiagBridge() { finish(); }

        void finish() {
            if (finished) return;
            finished = true;
            DiagnosticEngine::setActive(nullptr);
            engine.emit(/*json=*/true);
            capture.reset();
            std::string passthrough;
            std::vector<CellDiagnostic> parsed;
            parseJsonlDiagnostics(buffer, &parsed, &passthrough);
            // A cell's diagnostics are ITS OWN account: the first cell also
            // compiles the stdlib, whose lint rides the same envelope.
            for (auto& d : parsed) {
                if (d.severity != "error" && !d.file.empty()
                        && d.file != cellFile)
                    continue;
                result.diagnostics.push_back(std::move(d));
            }
            if (!passthrough.empty()) {
                std::fwrite(passthrough.data(), 1, passthrough.size(), stderr);
                std::fflush(stderr);
            }
            CompilerFlags f = compiler.getFlags();
            f.diagFormat = priorFormat;
            compiler.setFlags(f);
        }
    } bridge(result, *impl.compiler, cellName);

    const bool cellTiming = std::getenv("CAJETA_PRIME_TIMING") != nullptr;
    auto cellStart = std::chrono::steady_clock::now();
    auto cellMark = cellStart;
    auto cellPhase = [&](const char* name) {
        if (!cellTiming) return;
        auto now = std::chrono::steady_clock::now();
        auto ms = [](auto d) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
        };
        std::fprintf(stderr, "[cell] %-28s %7lld ms   (cumulative %lld ms)\n",
                     name, (long long) ms(now - cellMark),
                     (long long) ms(now - cellStart));
        cellMark = now;
    };

    CajetaModulePtr cellModule;
    try {
        cellModule = impl.compiler->createModule(
            cellPath.string(), (impl.scratchRoot / "src").string(),
            (impl.scratchRoot / "archive").string());
        // SAFEPOINTS, on THIS MODULE only: a safepoint is the only place an
        // interrupt can be taken, and `safepoints` NOT `debugInfo` — that one
        // also retains the class registry and drags the stdlib into the cell.
        {
            CompilerFlags cellFlags = cellModule->getFlags();
            cellFlags.safepoints = true;
    // A would-be-UB trap in the cell unwinds instead of killing the process.
    cellFlags.trapsUnwind = true;
            cellModule->setFlags(cellFlags);
        }
        // Name THIS cell as the session emit target: a stdlib template over a
        // user type must emit HERE, not into the sealed cell that declared it.
        CajetaModule::setActiveUnitModule(cellModule);
        cellPhase("createModule");
        impl.compiler->compile(cellModule);
        cellPhase("compile (front end)");
        // Codegen finalize, mirroring the JIT host's cold path; skipping a pass
        // leaves symbols undefined. The method loop is a FIXPOINT.
        CajetaModule::validatePlaceholders();
        CajetaModule::resolveAdviceMatches();
        CajetaModule::resolveDependencyGraph();
        cellPhase("resolve placeholders/graph");
        // The codegen set INCLUDES the stdlib module: its bodies are emitted on
        // demand, and a cell calling into it fails to materialize without them.
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
        {
            auto ixT0 = std::chrono::steady_clock::now();
            impl.symbolIndex.build(codegenMods());
            if (cellTiming) {
                auto ixMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - ixT0).count();
                std::fprintf(stderr,
                    "[cell] symbol index: %zu entries in %lld ms\n",
                    impl.symbolIndex.size(), (long long) ixMs);
            }
        }

        // Under lazy emission ordinary bodies leave the eager fixpoint; types,
        // declarations and vtable completion stay eager for every module.
        const bool lazyBodies = cajeta::lazyCodegenEnabled();
        size_t prevMethodCount = 0;
        size_t cgIters = 0, cgLastMethods = 0, cgMods = 0;
        while (true) {
            ++cgIters;
            auto mods = codegenMods();
            cgMods = mods.size();
            size_t methodCount = 0;
            for (auto& m : mods) methodCount += m->getAllMethods().size();
            for (auto& m : mods)
                for (auto& method : m->getAllMethods())
                    method->getLlvmFunctionType();
            for (auto& m : mods) m->completePendingInterfaceVTables();
            for (auto& m : mods) {
                if (lazyBodies && m != cellModule) continue;
                for (auto& method : m->getAllMethods()) {
                    method->generateCode();
                    ++impl.stats.eagerBodiesGenerated;
                }
            }
            size_t after = 0;
            for (auto& m : codegenMods()) after += m->getAllMethods().size();
            cgLastMethods = after;
            if (after == methodCount && after == prevMethodCount) break;
            prevMethodCount = after;
        }
        if (cellTiming) {
            std::fprintf(stderr,
                "[cell] codegen fixpoint: %zu iterations over %zu modules, "
                "%zu methods\n", cgIters, cgMods, cgLastMethods);
            auto cgm = codegenMods();
            std::set<CajetaModule*> distinct;
            size_t dupMethods = 0;
            for (auto& m : cgm) {
                if (!distinct.insert(m.get()).second) {
                    dupMethods += m->getAllMethods().size();
                }
            }
            std::fprintf(stderr,
                "[cell]   %zu entries, %zu distinct modules; %zu methods are "
                "duplicate entries\n",
                cgm.size(), distinct.size(), dupMethods);
        }
        cellPhase("codegen method bodies");
        {
            for (auto& m : codegenMods())
                for (auto& [name, klass] : m->getStructures())
                    if (klass) klass->generateStaticInitializers();
        }
        cellPhase("static initializers");
        // Under lazy, registration is gated by the resolved reflection keep-set:
        // a reg ctor's #ClassObject pulls the whole reflect chain with it.
        if (lazyBodies) {
            auto keep = cajeta::resolveReflectionKeepSet();
            for (auto& m : codegenMods()) m->setKeepSet(keep);
        }
        // #ClassObject stays eager in all modes — registration runs at dylib
        // init. The thunk BODIES are named symbols, so lazy serves them on call.
        for (auto& [key, type] : CajetaType::getCanonicalMap()) {
            if (auto klass = std::dynamic_pointer_cast<CajetaClass>(type)) {
                if (!lazyBodies) {
                    klass->emitReflectInvokeBody();
                    klass->emitReflectNewBody();
                }
                klass->finalizeClassObject();
            }
        }
        cellPhase("reflect thunks + ClassObject");
    } catch (cajeta::Exception& e) {
        // A failed cell leaves the session exactly as it was: no dylib was made,
        // and the ownership table is written back only on success.
        result.errorId = e.getErrorId();
        result.message = e.getMessage();
        result.file = e.getFile().empty() ? cellName : e.getFile();
        result.line = e.getLine();
        // A cell cannot import what it installs, and the signal has to be the
        // SOURCE: the import fails while compiling, before the install runs.
        if (source.find("Packages.install") != std::string::npos) {
            result.message +=
                " — this cell calls Packages.install, and a cell cannot "
                "import what it installs: the cell is compiled before the "
                "install runs. Import it from the next cell.";
        }
        // A syntax error arrives as a COUNT with no coordinates — the located
        // records are already out — so take the coordinates from the first.
        if (result.line <= 0) {
            bridge.finish();
            bool located = false;
            for (const auto& d : result.diagnostics) {
                if (d.severity != "error" || d.line <= 0) continue;
                result.file = d.file;
                result.line = d.line;
                located = true;
                break;
            }
            // The fold-back below cannot run now — the engine's channel is
            // closed — so do its job by hand when there was nothing to adopt.
            if (!located) {
                CellDiagnostic d;
                d.severity = "error";
                d.code = result.errorId;
                d.message = result.message;
                d.file = result.file.empty() ? cellName : result.file;
                d.line = result.line > 0 ? result.line : 0;
                d.column = e.getColumn();
                result.diagnostics.push_back(d);
            }
        } else {
            // The throw carried the error out of the stream, so it never became a
            // record. Fold it in, so `diagnostics` is the whole account.
            bridge.engine.report("error", result.errorId, result.message,
                                 result.file, result.line, e.getColumn());
        }
        if (cellModule && cellModule->getLlvmModule()) {
            impl.poisoned.insert(cellModule->getLlvmModule());
        }
        return result;
    } catch (std::exception& e) {
        result.errorId = "CAJETA_ERROR_INTERNAL";
        result.message = e.what();
        bridge.engine.report("error", result.errorId, result.message,
                             cellName);
        return result;
    }
    bridge.finish();

    std::vector<CajetaModulePtr> fresh;
    {
        auto all = impl.compiler->getModules();
        std::vector<CajetaModulePtr> candidates(all.begin(), all.end());
        // The stdlib module ACCUMULATES instantiations and must be delivered too;
        // whether getModules() holds it depends on which session built it.
        if (auto stdlib = CajetaModule::getStdlibModule()) {
            candidates.push_back(stdlib);
        }
        // BY IR-MODULE IDENTITY: one llvm::Module reaching `fresh` twice is two
        // addIRModule calls, which ORC rejects as a duplicate definition.
        std::set<llvm::Module*> queued;
        for (auto& m : candidates) {
            if (m && m->getLlvmModule()
                && !impl.delivered.count(m->getLlvmModule())
                && !impl.poisoned.count(m->getLlvmModule())
                && queued.insert(m->getLlvmModule()).second) {
                fresh.push_back(m);
            }
        }
    }
    if (fresh.empty()) {
        result.errorId = "CAJETA_ERROR_INTERNAL";
        result.message = "cell produced no module";
        return result;
    }

    // An ACCUMULATING module is never delivered whole under lazy: that would bind
    // every class's reflect chain at once. Its ctor DELTA is extracted below.
    const bool lazyDelivery = cajeta::lazyCodegenEnabled();
    // ONLY the stdlib: a cell's work spans several CajetaModules (script synthesis
    // mints its own unit), and all of those must deliver whole.
    llvm::Module* stdlibIr = nullptr;
    if (auto stdlibM = CajetaModule::getStdlibModule())
        stdlibIr = stdlibM->getLlvmModule();
    auto accumulating = [&](const CajetaModulePtr& m) {
        return lazyDelivery && stdlibIr
            && m->getLlvmModule() == stdlibIr;
    };

    // Legalize every module before verifying any (a use from B trips A's
    // verifier), then demote instantiations shared with an earlier cell.
    std::vector<CajetaModulePtr> scan(fresh.begin(), fresh.end());
    cajeta::backfillDropFunctions(scan, scan);
    cajeta::pinDropFunctionDefinitions(scan);
    for (auto& m : fresh) {
        if (accumulating(m)) continue;
        cajeta::jit::legalizeCrossModuleRefs(m->getLlvmModule());
        impl.stats.weakDemotedInstantiations +=
            cajeta::jit::demoteInstantiationsToWeakODR(m->getLlvmModule());
    }
    cellPhase("legalize + demote");


    // Session-lived statics: a later cell that merely REFERENCES a class static
    // re-emits the global with an initializer and, its own dylib being searched
    // first, reads its private zero copy. EXTERNAL linkage only.
    auto sharedGlobal = [](const llvm::GlobalVariable& g) {
        return g.hasInitializer() && !g.hasLocalLinkage()
            && !g.hasAppendingLinkage()
            && g.getLinkage() != llvm::GlobalValue::PrivateLinkage
            && g.getLinkage() != llvm::GlobalValue::InternalLinkage;
    };
    for (auto& m : fresh) {
        // Never strip an accumulating module's live initializers: they are what
        // findLiveDefinition serves, and a stripped global is a declaration.
        if (accumulating(m)) continue;
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

    std::set<llvm::Module*> skipDelivery;
    for (auto& m : fresh) {
        if (accumulating(m)) continue;
        llvm::Module* lm = m->getLlvmModule();
        std::string verifyErr;
        llvm::raw_string_ostream vs(verifyErr);
        if (llvm::verifyModule(*lm, &vs)) {
            // A module from a CLASSPATH ARCHIVE is not this session's work, and
            // ORC materializes lazily, so unused malformed IR never compiles.
            if (impl.prebuilt.count(lm)) {
                CellDiagnostic d;
                d.severity = "warning";
                d.code = "CAJETA_WARN_CLASSPATH_IR";
                d.message = "classpath module `" + lm->getModuleIdentifier()
                          + "` does not verify; it is delivered as-is and "
                            "will fail only if a cell calls into it: "
                          + verifyErr;
                d.file = cellName;
                result.diagnostics.push_back(std::move(d));
                // And do not DELIVER it: malformed IR cannot survive the bitcode
                // round-trip, so its symbols go missing instead.
                skipDelivery.insert(lm);
                continue;
            }
            result.errorId = "CAJETA_ERROR_INTERNAL";
            result.message = "module verify failed [" + lm->getModuleIdentifier()
                           + "]: " + verifyErr;
            return result;
        }
    }

    // Deliver a SNAPSHOT, not the live module: the front end keeps mutating its
    // llvm::Module on later cells, so the JIT gets re-parsed bitcode.
    for (auto& m : fresh) {
        if (accumulating(m)) continue;
        llvm::Module* lm = m->getLlvmModule();
        if (skipDelivery.count(lm)) continue;
        if (std::getenv("CAJETA_PRIME_TIMING")) {
            size_t defs = 0, decls = 0;
            for (auto& gv : lm->global_values())
                (gv.isDeclaration() ? decls : defs)++;
            std::fprintf(stderr,
                         "[deliver] whole %s: %zu defs, %zu decls\n",
                         lm->getModuleIdentifier().c_str(), defs, decls);
        }
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
        // The whole delivery just initialized these ctors; a later init delta
        // over this module must carry only ctors born after this point.
        if (lazyDelivery) cajeta::recordDeliveredCtors(lm, impl.deliveredCtors);
        for (auto& g : lm->globals()) {
            if (sharedGlobal(g)) impl.definedGlobals.insert(g.getName().str());
        }
    }

    // Init deltas: the not-yet-delivered ctors and their closure, over EVERY
    // compiled module — a later keep can mint one in a delivered cell module.
    std::vector<CajetaModulePtr> deltaTargets;
    if (lazyDelivery) {
        std::set<llvm::Module*> seen;
        auto consider = [&](const CajetaModulePtr& m) {
            if (!m || !m->getLlvmModule()) return;
            llvm::Module* lm = m->getLlvmModule();
            if (impl.prebuilt.count(lm) || impl.poisoned.count(lm)) return;
            if (seen.insert(lm).second) deltaTargets.push_back(m);
        };
        for (auto& m : impl.compiler->getModules()) consider(m);
        if (auto stdlibM = CajetaModule::getStdlibModule()) consider(stdlibM);
    }
    for (auto& m : deltaTargets) {
        auto delta = cajeta::extractInitDelta(m->getLlvmModule(),
                                              impl.deliveredCtors);
        if (!delta) {
            result.errorId = "CAJETA_ERROR_INTERNAL";
            result.message = "init-delta extract failed ["
                           + m->getLlvmModule()->getModuleIdentifier() + "]: "
                           + llvm::toString(delta.takeError());
            return result;
        }
        if (!(*delta)) continue;
        if (auto err = impl.jit->addIRModule(cellJD, std::move(*delta))) {
            result.errorId = "CAJETA_ERROR_INTERNAL";
            result.message = "init-delta addIRModule failed: "
                           + llvm::toString(std::move(err));
            return result;
        }
    }

    if (auto err = impl.jit->initialize(cellJD)) {
        result.errorId = "CAJETA_ERROR_INTERNAL";
        result.message = "cell initialize failed: "
                       + llvm::toString(std::move(err));
        return result;
    }

    impl.cellJDs.push_back(&cellJD);
    cellPhase("verify + JIT materialize");
    ++impl.stats.cellsCompiled;
    ++impl.stats.cellDylibsCreated;

    // A BODY-ONLY redefinition swaps the bodies of a class that already has
    // live instances: each holds a vtable pointer baked at construction whose
    // slots still name the previous cell's functions, so repoint them in place.
    for (const std::string& canonical :
             impl.sessionState.takeBodyOnlyRedefinitions()) {
        auto klass = std::dynamic_pointer_cast<CajetaClass>(
            CajetaType::find(canonical));
        if (!klass) continue;
        auto* vtTy = llvm::dyn_cast_or_null<llvm::StructType>(
            klass->getVirtualTableType());
        if (!vtTy || vtTy->getNumElements() <= 5) continue;
        void* vtable = lookupSymbol(klass->symbolBase() + "#VTable");
        if (!vtable) continue;

        // Offsets come from the vtable StructType through the JIT's DataLayout,
        // never from assuming the header shape: that prefix has grown before.
        const llvm::DataLayout& dl = impl.jit->getDataLayout();
        const llvm::StructLayout* vtLayout = dl.getStructLayout(vtTy);
        auto* entriesTy =
            llvm::dyn_cast<llvm::ArrayType>(vtTy->getTypeAtIndex(5u));
        if (!entriesTy) continue;
        auto* entryTy =
            llvm::dyn_cast<llvm::StructType>(entriesTy->getElementType());
        if (!entryTy) continue;
        const uint64_t entriesOffset = vtLayout->getElementOffset(5);
        const uint64_t stride = dl.getTypeAllocSize(entryTy);
        const uint64_t fnOffset = dl.getStructLayout(entryTy)->getElementOffset(1);

        size_t slot = 0;
        for (auto& method : klass->getVirtualMethodList()) {
            const size_t index = slot++;
            if (index >= entriesTy->getNumElements()) break;
            // The symbol as EMITTED, not as reconstructed: a rebuilt mangling
            // does not always match what ORC resolves (see lookupShort).
            if (!method || !method->getLlvmFunction()) continue;
            void* fn = lookupSymbol(method->getLlvmFunction()->getName().str());
            if (!fn) continue;
            std::memcpy(static_cast<char*>(vtable) + entriesOffset
                            + index * stride + fnOffset,
                        &fn, sizeof(void*));
            ++impl.stats.vtableSlotsRepointed;
        }
    }

    // Register this cell's implicit class in the session's cumulative namespace,
    // so LATER cells' bare calls reach it. On success only.
    if (cellModule && !cellModule->getStructures().empty()) {
        for (auto& [canonical, klass] : cellModule->getStructures()) {
            if (klass && klass->isScriptSynthesized()) {
                impl.sessionState.addUnitClass(canonical);
            }
        }
    }

    // A notebook cell has no argv: an empty vector makes "no arguments" a stated
    // fact rather than a store nobody wrote.
    if (auto argsSym = lookupShort("__cajeta_args_install")) {
        reinterpret_cast<void (*)(int64_t, char**)>(argsSym)(0, nullptr);
    }

    // Run the cell's entry — its top-level statements. The symbol is MANGLED, so
    // only lookupShort matches it; missing it is a HARD failure.
    void* entry = lookupShort(scriptEntryName());
    if (!entry) {
        // A declaration-only cell is an ORDINARY unit with no synthesized entry.
        // Only a cell that IS a script unit must have one.
        if (cellModule && cellModule->isScriptUnit()) {
            result.errorId = "CAJETA_ERROR_INTERNAL";
            result.message =
                "script cell entry symbol not found after materialization";
            return result;
        }
        result.ok = true;
        return result;
    }
    // Capture the cell's output for the run. The unit result rides a side channel,
    // read through the JIT's OWN runtime copy, never the process's.
    auto* resultClear = reinterpret_cast<void (*)()>(
        lookupSymbol("__cajeta_script_result_clear"));
    auto* resultGet = reinterpret_cast<const char* (*)()>(
        lookupSymbol("__cajeta_script_result_get"));
    if (resultClear) resultClear();
    // The cell runs behind a session-level catch: without one `__cajeta_throw`
    // finds no frame and calls exit(1). The JIT's copy, so the TLS chain matches.
    auto* guardCall = reinterpret_cast<void* (*)(int32_t (*)(), int32_t*)>(
        lookupSymbol("__cajeta_session_guard_call"));

    // The interrupt seam, resolved HERE and once: in create() these symbols do
    // not resolve yet, and `requestInterrupt` becomes a silent no-op.
    if (!impl.requestInterruptFn) {
        impl.requestInterruptFn = reinterpret_cast<void (*)()>(
            lookupSymbol("__cajeta_session_request_interrupt"));
        impl.clearInterruptFn = reinterpret_cast<void (*)()>(
            lookupSymbol("__cajeta_session_clear_interrupt"));
        impl.interruptMarker = reinterpret_cast<void* (*)()>(
            lookupSymbol("__cajeta_session_interrupt_marker"));
        impl.trapMarker = reinterpret_cast<void* (*)()>(
            lookupSymbol("__cajeta_session_trap_marker"));
        impl.trapDescription = reinterpret_cast<const char* (*)()>(
            lookupSymbol("__cajeta_session_trap_description"));
    }
    // A request that arrived while nothing was running, or while THIS cell was
    // still compiling, is a no-op: the flag never survives into the run.
    if (impl.clearInterruptFn) impl.clearInterruptFn();

    void* thrown = nullptr;
    {
        std::unique_ptr<cajeta::util::FdCapture> capture;
        if (impl.streamHandler) {
            capture = std::make_unique<cajeta::util::FdCapture>(
                1, [&impl](const std::string& chunk) {
                    impl.streamHandler(chunk);
                });
        }
        if (guardCall) {
            thrown = guardCall(reinterpret_cast<int32_t (*)()>(entry),
                               &result.value);
        } else {
            result.value = reinterpret_cast<int32_t (*)()>(entry)();
        }
    }
    if (thrown) {
        // An interrupt arrives as a sentinel ADDRESS, not a Throwable: identity
        // is the whole test, and nothing may dereference it.
        if (impl.interruptMarker && thrown == impl.interruptMarker()) {
            result.threw = true;
            result.exceptionType = "KeyboardInterrupt";
            result.message = "interrupted";
            result.file = cellName;
            CellFrame frame;
            frame.file = cellName;
            frame.text = cellName;
            result.traceback.push_back(frame);
            return result;
        }
        // A would-be-UB trap, same shape: a sentinel address, never dereferenced.
        // Without this a cell's `4 / 0` kills the kernel with SIGILL.
        if (impl.trapMarker && thrown == impl.trapMarker()) {
            result.threw = true;
            result.exceptionType = "ArithmeticError";
            result.message = impl.trapDescription ? impl.trapDescription()
                                                  : "arithmetic fault";
            result.file = cellName;
            CellFrame frame;
            frame.file = cellName;
            frame.text = cellName;
            result.traceback.push_back(frame);
            return result;
        }
        describeThrow(thrown, cellName, &result);
        return result;
    }
    if (resultGet) {
        if (const char* text = resultGet()) {
            result.hasResult = true;
            result.result = text;
        }
    }
    result.ok = true;
    return result;
}

void KernelSession::setStreamHandler(StreamHandler handler) {
    impl_->streamHandler = std::move(handler);
}

void KernelSession::requestInterrupt() {
    if (impl_->requestInterruptFn) impl_->requestInterruptFn();
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
    // Under init-extract delivery the stdlib lives in the MAIN dylib, and the
    // session must read the copy a cell's calls resolve to. Last, so cells win.
    if (auto sym = impl.jit->lookup(impl.jit->getMainJITDylib(), exactName)) {
        return reinterpret_cast<void*>(sym->getValue());
    } else {
        llvm::consumeError(sym.takeError());
    }
    return nullptr;
}

void* KernelSession::lookupShort(const std::string& shortName) {
    if (void* exact = lookupSymbol(shortName)) return exact;
    Impl& impl = *impl_;

    // Candidates come from the ACTUAL emitted IR, not a reconstructed mangling;
    // keep the owning class so the choice among definitions can be ORDERED.
    std::map<std::string, std::string> byOwner;
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
    // Prefer the NEWEST unit class that defines the name (last-write-wins);
    // scanning in module order returns the OLDEST definition.
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

void KernelSession::describeThrow(void* thrown, const std::string& cellName,
                                  CellResult* result) {
    result->ok = false;
    result->threw = true;
    result->errorId = "CAJETA_ERROR_UNCAUGHT_THROW";
    result->file = cellName;

    auto* typeOf = reinterpret_cast<const char* (*)(void*)>(
        lookupSymbol("__cajeta_throwable_type"));
    auto* messageInto = reinterpret_cast<int32_t (*)(void*, char*, int32_t)>(
        lookupSymbol("__cajeta_throwable_message_into"));
    auto* frameCount = reinterpret_cast<int32_t (*)(void*)>(
        lookupSymbol("__cajeta_throwable_frame_count"));
    auto* frameAt = reinterpret_cast<int32_t (*)(
        void*, int32_t, const char**, const char**, const char**, int32_t*)>(
        lookupSymbol("__cajeta_throwable_frame"));

    if (typeOf) {
        if (const char* t = typeOf(thrown)) result->exceptionType = t;
    }
    if (messageInto) {
        char buf[4096];
        if (messageInto(thrown, buf, static_cast<int32_t>(sizeof(buf))) > 0) {
            result->message = buf;
        }
    }
    if (result->message.empty()) {
        result->message = result->exceptionType.empty()
            ? "uncaught throw" : ("uncaught " + result->exceptionType);
    }

    if (frameCount && frameAt) {
        int32_t n = frameCount(thrown);
        for (int32_t i = 0; i < n; ++i) {
            const char* type = "";
            const char* method = "";
            const char* file = "";
            int32_t line = 0;
            if (!frameAt(thrown, i, &type, &method, &file, &line)) continue;
            CellFrame f;
            f.type = type ? type : "";
            f.method = method ? method : "";
            f.file = file ? file : "";
            f.line = line;
            // A frame inside a cell's own entry IS the cell and renders as such;
            // `<script>` is what the frame descriptor carries for that entry.
            bool cellFrame = f.type == "<script>"
                || f.method == scriptEntryName()
                || f.type.rfind("cajeta.script.", 0) == 0;
            if (cellFrame) {
                std::string where = f.file.empty() ? cellName : f.file;
                f.text = where + ", line " + std::to_string(f.line);
            } else {
                f.text = f.type + "." + f.method + " (" + f.file + ":"
                       + std::to_string(f.line) + ")";
            }
            result->traceback.push_back(std::move(f));
        }
    }
    if (result->traceback.empty()) {
        CellFrame f;
        f.file = cellName;
        f.text = cellName;
        result->traceback.push_back(std::move(f));
    }
}

bool KernelSession::collidesWithSession(const std::string& archivePath,
                                        std::string* error) {
    try {
        auto archive = CajetaArchive::readFrom(archivePath);
        auto& cmap = CajetaType::getCanonicalMap();
        for (const auto& e : archive.getEntries()) {
            if (e.kindTag != CajetaArchive::EntryKind::ClassSource) continue;
            std::string canonical = e.name;
            const std::string suffix = ".cajeta";
            if (canonical.size() > suffix.size()
                && canonical.compare(canonical.size() - suffix.size(),
                                     suffix.size(), suffix) == 0) {
                canonical.resize(canonical.size() - suffix.size());
            }
            std::replace(canonical.begin(), canonical.end(), '/', '.');
            if (cmap.find(canonical) != cmap.end()) {
                if (error) {
                    *error = "'" + canonical + "' is already loaded in this "
                             "session — an install never shadows session "
                             "state";
                }
                return true;
            }
            // A cell's classes do not keep the package they declare — the
            // script-unit pass rewrites them into the reserved `cajeta.script`.
            auto dot = canonical.rfind('.');
            std::string simple = dot == std::string::npos
                ? canonical : canonical.substr(dot + 1);
            std::string asCellDeclared =
                std::string(cajeta::scriptDefaultPackage()) + "." + simple;
            if (cmap.find(asCellDeclared) != cmap.end()) {
                if (error) {
                    *error = "'" + simple + "' was declared by an earlier "
                             "cell, and '" + canonical + "' would collide "
                             "with it — an install never shadows session "
                             "state. Rename the cell's class, or restart the "
                             "session to install cleanly.";
                }
                return true;
            }
        }
    } catch (std::exception& e) {
        if (error) {
            *error = std::string("cannot read archive '") + archivePath
                   + "': " + e.what();
        }
        return true;
    }
    return false;
}

bool KernelSession::installArchive(const std::string& cjaPath,
                                   std::string* error) {
    Impl& impl = *impl_;
    auto fail = [&](const std::string& m) {
        if (error) *error = m;
        return false;
    };
    if (!impl.compiler) return fail("installArchive: no live session");
    std::error_code ec;
    auto canon = std::filesystem::weakly_canonical(cjaPath, ec);
    if (ec || !std::filesystem::exists(canon)) {
        return fail("installArchive: no such archive: " + cjaPath);
    }
    const std::string key = canon.string();
    if (impl.installedArchives.count(key)) return true;

    // Answer the CALLER, not the drain: a queued splice reports success to the
    // installing cell, so a collision found later would be invisible.
    std::string collision;
    if (collidesWithSession(key, &collision)) {
        return fail("installArchive: " + collision);
    }
    if (impl.cellExecuting) {
        impl.pendingInstalls.push_back(key);
        return true;
    }

    bool ok = false;
    std::string failMsg;
    // The splice is compiler-side work and may re-enter from JIT'd code mid-cell,
    // so it takes the same gate as the lazy generator.
    cajeta::CompilerGate::instance().run([&] {
        try {
            // Collision scan BEFORE any mutation. NOT redundant with the caller's
            // eager scan: a queued splice drains after the rest of the cell ran.
            auto archive = CajetaArchive::readFrom(key);
            auto& cmap = CajetaType::getCanonicalMap();
            for (const auto& e : archive.getEntries()) {
                if (e.kindTag != CajetaArchive::EntryKind::ClassSource)
                    continue;
                std::string canonical = e.name;
                const std::string suffix = ".cajeta";
                if (canonical.size() > suffix.size()
                    && canonical.compare(canonical.size() - suffix.size(),
                                         suffix.size(), suffix) == 0) {
                    canonical.resize(canonical.size() - suffix.size());
                }
                std::replace(canonical.begin(), canonical.end(), '/', '.');
                if (cmap.find(canonical) != cmap.end()) {
                    failMsg = "installArchive: '" + canonical
                            + "' is already loaded in this session — an "
                              "install never shadows session state";
                    return;
                }
            }

            std::set<llvm::Module*> before;
            for (auto& m : impl.compiler->getModules()) {
                if (m && m->getLlvmModule()) before.insert(m->getLlvmModule());
            }

            impl.compiler->addClasspath(key);
            impl.compiler->ingestClasspath();
            impl.compiler->linkClasspathModules();

            for (auto& m : impl.compiler->getModules()) {
                if (!m || !m->getLlvmModule()) continue;
                llvm::Module* lm = m->getLlvmModule();
                if (before.count(lm)) continue;
                if (lm->getModuleIdentifier() == "cajeta.runtime.__stdlib__")
                    continue;
                impl.prebuilt.insert(lm);
            }
            impl.installedArchives.insert(key);
            ok = true;
        } catch (cajeta::Exception& e) {
            failMsg = std::string("installArchive: ") + e.getErrorId() + ": "
                    + e.getMessage();
        } catch (std::exception& e) {
            failMsg = std::string("installArchive: ") + e.what();
        }
    });
    if (!ok) return fail(failMsg.empty()
                             ? std::string("installArchive: failed") : failMsg);
    return true;
}

void KernelSession::shutdown() {
    Impl& impl = *impl_;
    if (impl.shutdownDone) return;
    impl.shutdownDone = true;
    // Teardown ORDER is load-bearing: session bindings drop FIRST, while the
    // carrier pool and the JIT'd code their drop functions reach into are still
    // live; carriers are joined second. Each exactly once for the session.
    if (impl.jit) {
        if (void* fn = lookupSymbol("__cajeta_session_count")) {
            impl.stats.sessionBindingsAtShutdown =
                static_cast<int>(reinterpret_cast<int64_t (*)()>(fn)());
        }
        if (void* fn = lookupSymbol("__cajeta_session_drop_all")) {
            reinterpret_cast<void (*)()>(fn)();
            ++impl.stats.sessionDropAllCalls;
        }
        if (void* fn = lookupSymbol("__cajeta_session_count")) {
            impl.stats.liveSessionBindings =
                static_cast<int>(reinterpret_cast<int64_t (*)()>(fn)());
        }
        if (void* fn = lookupSymbol("__cajeta_task_shutdown")) {
            reinterpret_cast<void (*)()>(fn)();
            ++impl.stats.taskShutdownCalls;
        }
    }
    // Give this session's USER struct NAMES back to the shared context: llvm
    // struct types are context-owned, so the next session's `Point` would reuse
    // this one's layout. BY MODULE — a superseded generation has no key.
    {
        std::set<std::string> stdlibResident;
        if (auto stdlib = CajetaModule::getStdlibModule()) {
            if (auto* lm = stdlib->getLlvmModule()) {
                for (auto* st : lm->getIdentifiedStructTypes()) {
                    if (st->hasName()) stdlibResident.insert(st->getName().str());
                }
            }
        }
        for (llvm::Module* lm : impl.delivered) {
            if (!lm) continue;
            for (auto* st : lm->getIdentifiedStructTypes()) {
                if (st->hasName() && !stdlibResident.count(st->getName().str())) {
                    st->setName("");
                }
            }
        }
    }
    Compiler::setSharedContext(nullptr);
    // Thread-global: leaving it set would point the next compiler in this
    // process (another test, a lint pass) at a module that is about to die.
    CajetaModule::setActiveUnitModule(nullptr);
    std::error_code ec;
    std::filesystem::remove_all(impl.scratchRoot, ec);
}

const SessionStats& KernelSession::stats() const {
    if (impl_->lazyGenerator) {
        impl_->stats.lazyBodiesDelivered =
            (long long) impl_->lazyGenerator->generatedCount();
    }
    return impl_->stats;
}

}  // namespace cajeta::kernel
