#include "cajeta/kernel/KernelSession.h"
#include "cajeta/jit/CajetaJitWinSymbols.h"

#include "cajeta/jit/JitCoffLinking.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
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

// notebook-olla-install U2 (spec 2.1, 2.7) — the runtime's install bridge.
// Declared here rather than in a header: the C side is the runtime's, and
// the kernel is only one of its hosts.
extern "C" void __cajeta_session_set_install_hook(
    int32_t (*fn)(const char* name, int32_t nameLen,
                  const char* constraint, int32_t constraintLen,
                  int32_t save, char* out, int32_t outCap, void* ctx),
    void* ctx);

// The bridge's state lives HERE, in the host, and cajeta_rt_session.c only
// declares it. The runtime is compiled twice — into this binary, and to the
// bitcode embedded in every JIT session — so a definition on the runtime
// side would give cell code a second copy of the hook and the registration
// below would never be seen by the code that calls it. Retained and
// default-visibility so the JIT's process generator can bind them.
extern "C" {
__attribute__((used, retain, visibility("default")))
int32_t (*__cajeta_install_hook)(const char*, int32_t, const char*, int32_t,
                                 int32_t, char*, int32_t, void*) = nullptr;
__attribute__((used, retain, visibility("default")))
void* __cajeta_install_ctx = nullptr;
__attribute__((used, retain, visibility("default")))
char __cajeta_install_out[2048] = {0};
}

namespace {

    // The session whose cell is executing right now. JIT'd
    // `Packages.install` reaches its host through here — single-threaded
    // by the same contract the binding registry documents.
    thread_local KernelSession* g_activeSession = nullptr;

    // Unit 3: the buildtool's semver matcher, not a second one. Unit 2
    // shipped a two-line prefix match as a placeholder; a version grammar
    // must have exactly one implementation, and the resolver's is it.
    bool versionSatisfies(const std::string& version,
                          const std::string& constraint) {
        if (constraint.empty() || constraint == "*") return true;
        return cajeta::buildtool::versionSatisfies(version, constraint);
    }

    void writeOut(char* out, int32_t cap, const std::string& text) {
        if (!out || cap <= 0) return;
        auto n = std::min<size_t>(text.size(), (size_t) cap - 1);
        std::memcpy(out, text.data(), n);
        out[n] = '\0';
    }

    // --- the compiler-jsonl bridge (spec 4.4; compiler-jsonl §2-§3) -------
    //
    // A cell's diagnostics reach the notebook as STRUCTURED payloads, not as
    // scraped text. The compiler already has one machine-readable format for
    // exactly this, so the bridge reads it rather than inventing a second: the
    // cell compile runs with `--diag-format=json` in force, its stderr is
    // captured, and each NDJSON record is parsed.
    //
    // Going through the stream rather than reading the DiagnosticEngine
    // directly is deliberate. Plenty of diagnostics never pass through the
    // engine — the located syntax listener emits straight to the channel —
    // and the stream is the one place they all converge.

    // The envelope major this kernel understands. An unknown major is refused
    // whole rather than half-read (compiler-jsonl 2.1.4).
    constexpr int kSupportedJsonlMajor = 1;

    // Process-wide switch, per-cell decision: scope it (compiler-jsonl 5.1.2,
    // the same reason runLintDriver does this).
    struct JsonGateScope {
        bool prev;
        explicit JsonGateScope(bool on) : prev(cajeta::jsonProgressEnabled()) {
            cajeta::setJsonProgressEnabled(on);
        }
        ~JsonGateScope() { cajeta::setJsonProgressEnabled(prev); }
    };

    // Parse one cell's captured stderr into diagnostics. Lines that are not
    // JSON at all are compiler chatter that never got a structured form; they
    // are handed back so the caller can put them where they were going
    // anyway, because swallowing compiler output is worse than not
    // structuring it.
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
                    // Refuse the rest of the stream and say so once.
                    refused = true;
                    if (passthrough) {
                        *passthrough += "cajeta kernel: unsupported diagnostic "
                                        "stream major; diagnostics for this "
                                        "cell were not read\n";
                    }
                }
                continue;
            }
            // Unknown kinds are SKIPPED, not fatal — that is what makes a new
            // record kind a minor bump (compiler-jsonl 2.1.5).
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

    // A cell's DISPLAY name and its IDENTIFIER are two different things. The
    // display name is "In[3]" — spec 4.4 pins it, because that is what a
    // notebook user sees in a diagnostic. The identifier is derived here: it
    // becomes the script unit's implicit class name (script-units 3.2), so it
    // has to be a legal Cajeta identifier and unique within the session, and
    // beyond that we are free to choose it. It is user-visible in its own
    // right — it appears in mangled symbols, so it shows up in JIT errors and
    // stack frames — so "In[3]" becomes `cell_3` rather than the `In_3_` that
    // falls out of mechanically replacing the brackets.
    // Spec 6 / script-units §7.3 — the nearest ancestor `cajeta.json` of
    // `dir` supplies the classpath: its resolved manifest dependencies,
    // exactly the set `cajeta build` would pass. No manifest anywhere up the
    // tree is not an error — that is a notebook outside a project, which is
    // a stdlib-only session and perfectly ordinary.
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

    std::string stemFor(const std::string& cellName) {
        // The "In[N]" shape (what execute()'s no-name overload produces).
        if (cellName.size() > 3 && cellName.compare(0, 3, "In[") == 0
                && cellName.back() == ']') {
            std::string n = cellName.substr(3, cellName.size() - 4);
            if (!n.empty() && std::all_of(n.begin(), n.end(), [](unsigned char c) {
                    return std::isdigit(c) != 0;
                })) {
                return "cell_" + n;
            }
        }
        // Anything else (a caller-supplied name) still just has to become a
        // legal identifier.
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
    KernelSession::StreamHandler streamHandler;
    // U6 (spec 5.1) — the interrupt seam, resolved once on the EXECUTION
    // thread at the first cell (see execute()). `requestInterrupt` is
    // documented as callable from another thread, and it can only honour that
    // if it never touches the JIT: a symbol lookup while the execution thread
    // is materializing a cell is exactly the race the rest of this class
    // exists to avoid. These are plain function pointers into the JIT's
    // runtime copy, written once and read-only thereafter. Resolving through
    // the JIT (not the process copy) matters for the same reason the guard
    // does: the flag the safepoint reads must be the flag the setter sets.
    void (*requestInterruptFn)() = nullptr;
    void (*clearInterruptFn)() = nullptr;
    void* (*interruptMarker)() = nullptr;
    // 2.3.2 — the would-be-UB trap's sentinel and its description.
    void* (*trapMarker)() = nullptr;
    const char* (*trapDescription)() = nullptr;
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
    // lazy-codegen Unit 1 — mangled symbol -> method, rebuilt per cell because
    // codegen instantiates templates and so defines new methods (spec 3.5).
    CajetaSymbolIndex symbolIndex;
    // 7.2.5 — llvm::Modules that came from a CLASSPATH ARCHIVE rather than
    // from this session's codegen. Recorded once at create, right after the
    // ingest, so a cell's verify pass can tell "our IR is malformed" from
    // "a dependency we did not compile is malformed".
    std::set<llvm::Module*> prebuilt;
    // notebook-olla-install U1: canonical paths of archives spliced
    // mid-session — the idempotence key (spec 2.4's substrate).
    std::set<std::string> installedArchives;
    // U3: the project governing this session, kept because an install
    // has to read its `settings.repositories` long after create() ran.
    std::string projectDir;
    // U2 (spec 2.4/2.5): what each install DECLARED, so a re-install is
    // judged against the loaded version rather than the path it arrived
    // by — two paths can carry the same library.
    struct InstallRecord {
        std::string version;
        std::string path;
    };
    std::map<std::string, InstallRecord> installsByName;
    // Archives acquired by the CURRENTLY executing cell. A cell cannot
    // import what it just installed (spec 2.3), and this is what lets the
    // failure say so instead of "unresolved type".
    std::vector<std::string> installedThisCell;
    // A splice runs compiler passes a half-executed cell's context cannot
    // host (measured: mid-cell ingest failed resolving int32). Mid-cell
    // requests queue here and drain at the cell boundary — same-cell
    // imports are impossible anyway (spec 2.3), so nothing is lost.
    bool cellExecuting = false;
    std::vector<std::string> pendingInstalls;
    // lazy-codegen 4.2.4 — ctor functions already delivered in an
    // init-extract. Accumulating modules (the stdlib above all) are never
    // delivered whole under lazy; each cell delivers the ctor DELTA and the
    // generator serves everything else, so a delivered module cannot bind
    // every class's vtable/RTTI/thunk chain at materialization.
    std::set<std::string> deliveredCtors;
    // The session's DefinitionGenerator, owned by the main JITDylib; held
    // raw for stats only (generatedCount -> lazyBodiesDelivered).
    cajeta::CajetaDefinitionGenerator* lazyGenerator = nullptr;
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
    // IMAGE_REL_AMD64_ADDR32NB (see JitCoffLinking.h) — this bare builder was
    // the second site, found when the abort survived the CajetaJitHost fix.
    llvm::orc::LLJITBuilder builder;
    cajeta::jit::applyCoffJitLink(builder);
    auto jitOrErr = builder.create();
    if (!jitOrErr) {
        return setErr("LLJIT create failed: "
                      + llvm::toString(jitOrErr.takeError()));
    }
    impl.jit = std::move(*jitOrErr);

    auto& mainJD = impl.jit->getMainJITDylib();
    // lazy-codegen 2.2.3 — added FIRST, so a host library sharing a method's
    // name can never shadow a body we can generate (the sl_add/libbsd
    // collision class). Dark until lazy mode is on: eager default claims
    // nothing.
    {
        Impl* ip = &impl;
        auto gen = std::make_unique<cajeta::CajetaDefinitionGenerator>(
            impl.symbolIndex,
            [ip](llvm::orc::ThreadSafeModule tsm,
                 llvm::orc::JITDylib& jd) -> llvm::Error {
                return ip->jit->addIRModule(jd, std::move(tsm));
            });
        impl.lazyGenerator = gen.get();   // observability only (stats)
        mainJD.addGenerator(std::move(gen));
    }

    // Process symbols on the main dylib — the native runtime the JIT'd code
    // calls into (and the last-resort resolver for every cell).
    auto generator = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
        impl.jit->getDataLayout().getGlobalPrefix());
    if (!generator) {
        return setErr("process-symbol generator failed: "
                      + llvm::toString(generator.takeError()));
    }
    mainJD.addGenerator(std::move(*generator));

    // Windows symbol bridge: a PE exports nothing, so the process generator
    // above cannot see the statically linked CRT/libm/cajeta-native families
    // (see CajetaJitWinSymbols.cpp). CajetaJitHost installs this map and the
    // kernel session must too — without it every cell fails to materialize on
    // COFF ("Symbols not found: [ close, opendir, fabsf, __cajeta_tls_*, ... ]",
    // the v0.21.0 gate's last Windows failure class). No-op elsewhere.
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

    // Spec 6 — the project classpath, resolved FIRST because it decides how
    // the stdlib is built. Pure manifest/file work; no compiler needed yet.
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

    // U6 — an archive already on the classpath at session start IS loaded,
    // so record it the way a mid-session install is recorded. Without this
    // the idempotence registry only knows about installs performed by cells,
    // and re-installing a dependency the manifest already pins falls past
    // spec 2.4's no-op arm into the collision scan — which is exactly what
    // `installAndSave` on an already-pinned dependency does. Found by
    // SessionPackagesSaveTests once 6.1.2 made that collision visible
    // instead of a swallowed drain-time warning.
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

    // 7.2.5 — RESIDENT STDLIB ONLY WHEN THERE IS NO CLASSPATH.
    //
    // The reuse core's baseline is captured once per thread, before any
    // archive exists. Restoring it and THEN splicing an archive's modules
    // into the list the codegen fixpoint walks means the archive's code is
    // generated against a stdlib world it was not compiled against — two
    // definitions of one specialization with different `llvm::Type`
    // identity — and the cell dies at `module verify failed: Invalid
    // bitcast ... double to ptr`. Not just where archive and stdlib share a
    // generic: `int32 a = 20; a + 22;` died the same way.
    //
    // A per-classpath baseline is not the answer either — the core is
    // thread-global and two notebooks want two different classpaths. So a
    // classpath session builds its stdlib FRESH, which is exactly what
    // `cajeta run` does (CajetaJitHost takes the reuse core only under
    // `opts.resident`), and why the same archive on `--classpath` works
    // there. The cost is the first cell: ~15s of priming instead of the
    // restore, paid once per session and only when there IS a classpath.
    const bool useResidentStdlib = archives.empty();

    // Session-lived Compiler. The reuse core is single-threaded and its
    // baselines are thread_local, so this must be the thread that owns the
    // session (header contract).
    note(useResidentStdlib ? "priming stdlib" : "building stdlib for classpath");
    try {
        if (useResidentStdlib) {
            auto& core = StdlibReuseCore::instance();
            core.ensurePrimed();
            core.restoreBaseline();
            Compiler::setSharedContext(core.context());
        } else {
            // Own context, own stdlib. Explicit rather than assumed: the
            // shared context is a static, so a previous session on this
            // thread could have left it set.
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
    // visible while a cell's own imports resolve, which is the ordering every
    // AOT entry point uses and the one `cajeta run` copies (CajetaJitHost.cpp
    // ~1094). Doing it per cell would re-ingest the world every time and
    // still be too late for cell 1.
    {
        if (!archives.empty()) {
            note("ingesting " + std::to_string(archives.size())
                 + (archives.size() == 1 ? " dependency archive"
                                         : " dependency archives"));
            for (const auto& cp : archives) impl.compiler->addClasspath(cp);
            try {
                impl.compiler->ingestClasspath();
                // Definitions, not just declarations — the JIT links what it
                // RUNS. `ingestClasspath` alone leaves every dep symbol
                // unresolved at materialization ("Symbols not found:
                // dev.cajeta...."); see Compiler.h's note on why the splice
                // is opt-in rather than folded into the ingest.
                impl.compiler->linkClasspathModules();
                // Everything present NOW came out of the archives — this
                // session has not compiled a cell yet.
                for (auto& m : impl.compiler->getModules()) {
                    if (!m || !m->getLlvmModule()) continue;
                    // NOT the stdlib: it exists before the ingest (it is what
                    // `ensureStdlibModule` just built) and it is very much
                    // this session's own work. Marking it prebuilt excluded it
                    // from both sides of the collision check below, which is
                    // how the second duplicate survived the first fix.
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

// notebook-olla-install U2 — the host end of `Packages.install`.
//
// Resolution is STUBBED for this unit (plan 2.1.1): `name` is a local .cja
// path and the constraint is matched against the archive's own version.
// Unit 3 swaps NativeResolver in behind this same signature.
//
// The version and conflict arms (spec 2.4/2.5) are answered HERE, before
// the splice, because they only need the archive's manifest. The splice
// itself still queues to the cell boundary — a half-executed cell cannot
// host the ingest (measured, U1 1.2.3).
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

// notebook-olla-install U4 (spec 3.3) — is this archive vouched for by a
// key THIS MACHINE trusts?
//
// The trust store is the answer to "whose signature counts": `cajeta trust
// add` puts a key in the user tier, and env/user/system precedence is
// already defined there. A repository cannot make itself trusted by
// shipping a key alongside the artifact — that is the whole point.
//
// An unsigned archive reaching here is allowed: the require-signatures
// floor is enforced by the caller, which knows the policy. What is never
// allowed is a signature that fails to verify.
bool KernelSession::verifySignatureOrFail(
        const std::string& archivePath, const std::string& name,
        const std::string& version, const std::string& repoName,
        const std::string& signature,
        const std::function<void(const std::string&)>& phase,
        std::string* errorOut) {
    namespace bt = cajeta::buildtool;
    if (signature.empty()) return true;      // policy already decided above

    phase("checking signature for " + name + " " + version);
    auto layout = cajeta::cli::resolveTrustStoreLayout();
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

// notebook-olla-install U3 (spec 3.1, 3.2, 3.4, 2.6) — name + constraint
// to a verified local archive, through the buildtool's own resolver stack.
// There is no second fetch path here: repositories, cache, and checksums
// are the ones `cajeta build` uses.
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

    // Spec 3.1 — the governing project's repositories, else the default
    // central. A session with no project still installs.
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
            // Spec 3.5 — the policy floor. Read straight off settings: the
            // loader validates top-level blocks only, so this needs no
            // manifest-schema change.
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

    // A manifest's filesystem-repository path is relative to the MANIFEST,
    // which is the only anchor a reader can reason about. Resolving it
    // against the process cwd would make it depend on where the kernel
    // happened to be launched — and Jupyter launches one in the NOTEBOOK's
    // directory, not the project root, so `"path": "./repo"` beside
    // cajeta.json would silently resolve to `notebooks/repo` and the
    // repository would appear to carry no versions at all.
    if (!projectRoot.empty()) {
        for (auto& spec : specs) {
            if (spec.type != "filesystem" || spec.path.empty()) continue;
            if (std::filesystem::path(spec.path).is_absolute()) continue;
            spec.path = std::filesystem::weakly_canonical(
                std::filesystem::path(projectRoot) / spec.path).string();
        }
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

    // Highest satisfying version wins, first repository that carries one.
    phase("resolving " + name + " " + constraint);
    std::vector<std::string> consulted;
    bt::RepositoryPtr chosen;
    std::string chosenVersion;
    for (const auto& repo : *repos) {
        consulted.push_back(repo->name());
        auto versions = repo->listVersions(name);
        if (!versions) {          // a repo that cannot answer is not fatal
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
        // Spec 2.6 — name the constraint AND every repository consulted, so
        // the reader knows whether to fix the constraint or add a repo.
        std::string where;
        for (const auto& c : consulted) {
            if (!where.empty()) where += ", ";
            where += c;
        }
        return fail("Packages.install: no version of '" + name
                    + "' satisfies '" + constraint + "'. Repositories "
                      "consulted: " + (where.empty() ? "(none)" : where));
    }

    // Spec 3.4 — a cache hit is served without touching the network. The
    // published checksum IS the cache key, so this is only reachable when
    // the repository publishes one.
    bt::ArtifactCache cache(projectRoot.empty() ? stage : projectRoot);
    std::string published;
    if (auto pc = chosen->publishedChecksum(name, chosenVersion)) {
        if (pc->has_value()) published = **pc;
    } else {
        llvm::consumeError(pc.takeError());
    }
    // Spec 3.3/3.5 — the signature the repository publishes, resolved
    // BEFORE the cache arm so a cached artifact is held to the same policy
    // as a freshly fetched one. A cache hit is a shortcut past the network,
    // never past the checks.
    std::string signature;
    if (auto ps = chosen->publishedSignature(name, chosenVersion)) {
        if (ps->has_value()) signature = **ps;
    } else {
        llvm::consumeError(ps.takeError());
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
            if (!verifySignatureOrFail(*hit, name, chosenVersion,
                                       chosen->name(), signature, phase,
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

    // Spec 3.2 — verify before trusting. A mismatch discards the bytes and
    // fails; there is never a half-installed state, because nothing has
    // been spliced yet.
    if (!published.empty()) {
        phase("verifying " + name + " " + chosenVersion);
        std::string actual = bt::ArtifactCache::sha256OfFile(*fetched);
        if (actual != published) {
            std::error_code rm;
            std::filesystem::remove(*fetched, rm);
            return fail("Packages.install: checksum mismatch for '" + name
                        + "' " + chosenVersion + " from " + chosen->name()
                        + " — published " + published + ", got "
                        + (actual.empty() ? std::string("nothing") : actual)
                        + ". The download was discarded and nothing was "
                          "installed.");
        }
        if (!verifySignatureOrFail(*fetched, name, chosenVersion,
                                   chosen->name(), signature, phase,
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
            llvm::consumeError(stored.takeError());   // cache is best-effort
        }
    } else if (!verifySignatureOrFail(*fetched, name, chosenVersion,
                                      chosen->name(), signature, phase,
                                      errorOut)) {
        std::error_code rm;
        std::filesystem::remove(*fetched, rm);
        return false;
    }

    if (pathOut) *pathOut = *fetched;
    if (versionOut) *versionOut = chosenVersion;
    return true;
}

// notebook-olla-install U5 (spec 5.2-5.4) — the manifest write.
//
// `install` is session-only and the manifest is the reproducibility
// record; this is the separate, named act that graduates one into the
// other. It goes through the SAME format-preserving editor `cajeta add`
// uses, so a hand-maintained cajeta.json keeps its comments and layout —
// a notebook must not be the reason a project's manifest gets reformatted.
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

    // Spec 5.3 — no project governs this session, so there is nowhere to
    // record the dependency. Say what to do about it.
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

    // Spec 5.4 — an unchanged pin writes NOTHING. Rewriting a file to the
    // same bytes still churns its mtime and any watcher looking at it.
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

    // Spec 6.1 — a network fetch is never a silent stall. The phases go to
    // the cell's own stream, which is what the notebook is already showing.
    auto phase = [](const std::string& text) {
        std::fputs(("  " + text + "\n").c_str(), stdout);
        std::fflush(stdout);
    };

    // Spec 5.3 — decided BEFORE anything is fetched or spliced. Installing
    // and only then discovering there is nowhere to record it would leave
    // the session holding an archive while reporting a failure.
    if (save && impl.projectDir.empty()) {
        writeOut(out, outCap,
                 "Packages.installAndSave: no project governs this session, "
                 "so there is no cajeta.json to write. Start the kernel in a "
                 "project directory, or create one with `cajeta init "
                 "notebook`. (Packages.install still works — it just does "
                 "not survive a restart.)");
        return false;
    }

    // Spec 2.5 is decided BEFORE resolution, and deliberately so. If this
    // library is already loaded at a version the constraint excludes, no
    // answer the repositories give can change the outcome — the session
    // cannot replace a loaded archive. Resolving first would report
    // "no version satisfies '2.*'" when the truth is "you have 1.0.0
    // loaded and swapping it needs a restart", which sends the reader off
    // to look for a version that would not have helped.
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
        // Not a path — resolve it as a library name against the session's
        // repositories (spec 3.1).
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

    // Already loaded? Judge the LOADED version against this constraint —
    // a satisfying re-install is a no-op so run-all is safe (2.4), and an
    // excluded one cannot be honoured without a restart (2.5), because
    // JIT'd code from the loaded copy may be live.
    auto it = impl.installsByName.find(archiveName);
    if (it != impl.installsByName.end()) {
        const std::string& loaded = it->second.version;
        if (versionSatisfies(loaded, constraint)) {
            // A no-op INSTALL is still a real SAVE request (spec 5.4): the
            // manifest can need the new constraint written even when the
            // loaded version already satisfies it, which is precisely the
            // `installAndSave` re-run case.
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

    phase("splicing " + archiveName + " " + archiveVersion);
    std::string err;
    if (!installArchive(canon.string(), &err)) {
        writeOut(out, outCap, "Packages.install: " + err);
        return false;
    }
    impl.installsByName[archiveName] = Impl::InstallRecord{archiveVersion,
                                                           canon.string()};
    impl.installedThisCell.push_back(archiveName);

    // The manifest write comes LAST: a failed save must not leave the
    // session claiming an install it then reports as an error, and the
    // splice above is the part that cannot be undone.
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
            // U2: arm the runtime bridge for the duration of the cell, so
            // `Packages.install` from JIT'd code finds this session and a
            // call outside one still reports "no live session".
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
    // too (spec 2.2), so `In[N]`/`Out[N]` never reuse a number.
    result.executionCount = impl.execCount;


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
    //
    // The kernel is the one host with a SHARED TYPE WORLD: this session owns
    // its LLVMContext and type registry for its whole life, so a type object
    // recorded by cell 1 is still live and still means what it meant when
    // cell 5 compiles. Seeding relies on that to hand an older value its own
    // generation's type (script-units 5.3). Hosts that build a fresh world
    // per unit — `cajeta run`, the test harness — must NOT set this: there
    // the recorded type outlives its context and reading it is a use-after-
    // free (jupyter-kernel 2.2.7).
    impl.sessionState.setSharedTypeWorld(true);
    impl.compiler->setSessionState(&impl.sessionState, cellName);

    // The diagnostics bridge is live for the whole compile (spec 4.4; plan
    // 3.2.3). Its destructor closes it on EVERY exit path, including the
    // early returns in the catch blocks below — a cell that failed is exactly
    // the cell whose diagnostics matter most.
    struct DiagBridge {
        CellResult& result;
        Compiler& compiler;
        std::string cellFile;   // the cell's display name ("In[3]") — see finish()
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
            // Warnings COLLECT, errors keep THROWING. The kernel's failure
            // path depends on the throw: a collected error would let codegen
            // run on into null types (the same reason the JIT harness sets
            // this), and a cell must fail before it can reach a dylib.
            engine.setCollectErrors(false);
            DiagnosticEngine::setActive(&engine);
            capture = std::make_unique<cajeta::util::FdCapture>(
                2, [this](const std::string& chunk) { buffer += chunk; });
            // Each cell is its own stream, so a consumer can tell a clean
            // cell from a cell whose compile died before saying anything
            // (compiler-jsonl 2.1.3). Unlatched — this is the Nth cell in a
            // long-lived process, and it must look like a first.
            cajeta::emitStreamRecord();
        }
        ~DiagBridge() { finish(); }

        void finish() {
            if (finished) return;
            finished = true;
            DiagnosticEngine::setActive(nullptr);
            engine.emit(/*json=*/true);   // into the capture, still live
            capture.reset();              // restores fd 2, drains the tail
            std::string passthrough;
            std::vector<CellDiagnostic> parsed;
            parseJsonlDiagnostics(buffer, &parsed, &passthrough);
            // A cell's diagnostics are ITS OWN account. Under eager codegen
            // the first cell also compiles the stdlib's method bodies, and
            // since lint warnings ride the same NDJSON envelope, a "clean"
            // cell would inherit dozens of stdlib lint hints (41 on the
            // v0.21.0 gate — KernelIoTests.cleanCellHasNoDiagnostics).
            // Errors are kept wherever they point — a cell that broke a
            // stdlib specialization must hear about it — but sub-error
            // diagnostics only count when they name this cell's source (or
            // carry no location at all).
            for (auto& d : parsed) {
                if (d.severity != "error" && !d.file.empty()
                        && d.file != cellFile)
                    continue;
                result.diagnostics.push_back(std::move(d));
            }
            // Compiler chatter with no structured form is still compiler
            // output; put it back where it was going rather than swallow it.
            if (!passthrough.empty()) {
                std::fwrite(passthrough.data(), 1, passthrough.size(), stderr);
                std::fflush(stderr);
            }
            CompilerFlags f = compiler.getFlags();
            f.diagFormat = priorFormat;
            compiler.setFlags(f);
        }
    } bridge(result, *impl.compiler, cellName);

    // CAJETA_PRIME_TIMING=1 — the cell half of a cold start. [prime] and
    // [ingest] together account for under 11s of a ~50s first cell; nothing has
    // ever measured what runs after them.
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
        // U6 (spec 5.1) — SAFEPOINTS, on THIS MODULE only.
        //
        // `Block` gates the per-statement `__cajeta_dbg_safepoint` call on the
        // module's own `debugInfo`, and a safepoint is the only place an
        // interrupt can be taken: without one, a `while (true)` cell emits
        // nothing that can ever notice the request. So the flag goes on the
        // cell, where the user's statements are.
        //
        // `safepoints`, NOT `debugInfo`. Setting debugInfo was the first
        // attempt and it broke the first cell outright: debugInfo also calls
        // `noteForceAll("--debug-info=full")`, which retains the entire class
        // registry, which dragged every stdlib class into the cell's compile
        // — and the cell died on `unknown field type 'bfloat16'` from a
        // stdlib class that does not compile from source in that world. The
        // kernel wants somewhere to stop, not a debugger's worth of metadata.
        //
        // Scoped to the CELL's module, so the stdlib pays nothing. The cost
        // of that scoping is the documented limit (6.3.1): a cell parked
        // inside a long stdlib or native call reaches no safepoint and does
        // not stop until it returns to the cell's own code.
        {
            CompilerFlags cellFlags = cellModule->getFlags();
            cellFlags.safepoints = true;
    // 2.3.2 — and a would-be-UB trap in the cell unwinds instead of killing
    // the process. Scoped to the CELL's module for the same reason
    // safepoints are: the stdlib is not what a notebook author is editing.
    cellFlags.trapsUnwind = true;
            cellModule->setFlags(cellFlags);
        }
        // Name THIS cell as the session emit target: a stdlib template
        // specialized over a user type must emit HERE, not into the cell that
        // declared the type — that one is already sealed in the JIT
        // (jupyter-kernel 2.1.6). Only consulted for user-typed
        // specializations, and only when no codegen frame is open.
        CajetaModule::setActiveUnitModule(cellModule);
        cellPhase("createModule");
        impl.compiler->compile(cellModule);
        cellPhase("compile (front end)");
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
        cellPhase("resolve placeholders/graph");
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
            // getModules() already returns the stdlib once the session has it,
            // so this walks its 11,015 methods twice per fixpoint iteration.
            // Measured 2026-08-16: deduping changes the count 23,394 -> 12,379
            // and the time not at all, because generateCode() is idempotent.
            if (auto stdlib = CajetaModule::getStdlibModule()) {
                mods.push_back(stdlib);
            }
            return mods;
        };
        // lazy-codegen 1.2.2 — index alongside the eager loop. Observed only;
        // Unit 2's DefinitionGenerator is what will consult it.
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

        // lazy-codegen 4.2.1 — ordinary bodies leave the eager fixpoint when
        // lazy emission is on. Types, declarations, and vtable completion
        // stay eager for every module (delivered globals reference them);
        // generateCode() runs only for the CELL's own module, so compile
        // errors in the user's code still surface here, not at first call.
        // Everything else arrives through the DefinitionGenerator.
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
        // lazy-codegen 4.2.4 (spec 2.2.1) — under lazy, registration is
        // gated by the cell's resolved reflection keep-set, the same
        // resolution Lean-mode DCE runs: a reg ctor's #ClassObject pulls the
        // class's whole reflect chain through the init extract, so "register
        // everything" is the cascade. A forces-ALL site resolves to null =
        // keep-all, exactly as in AOT (spec 2.2.2). Already-created reg
        // ctors are permanent; the set only gates NEW ones, so keeps
        // accumulate across cells.
        if (lazyBodies) {
            auto keep = cajeta::resolveReflectionKeepSet();
            for (auto& m : codegenMods()) m->setKeepSet(keep);
        }
        // REFL-2: reflective adapter bodies + #ClassObject registration.
        // #ClassObject stays eager in all modes — registration runs at dylib
        // init, which nothing looks up (spec 2.2). The thunk BODIES are named
        // symbols referenced from the #ClassObject initialiser, so under lazy
        // they arrive through the generator on demand (spec 2.4).
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
        // script-units 5.5 / spec 2.2 — a failed cell leaves the session
        // exactly as it was. No dylib was created, and the ownership table
        // is only written back on a successful body compile.
        result.errorId = e.getErrorId();
        result.message = e.getMessage();
        result.file = e.getFile().empty() ? cellName : e.getFile();
        result.line = e.getLine();
        // notebook-olla-install 2.2.3 / spec 2.3 — a cell that installs
        // cannot also import what it installed. The signal has to be the
        // cell's SOURCE, not the install registry the plan first named:
        // the import fails while COMPILING, so the install has not run yet
        // and the registry is still empty. Without this the failure reads
        // as a broken install rather than an ordering rule.
        if (source.find("Packages.install") != std::string::npos) {
            result.message +=
                " — this cell calls Packages.install, and a cell cannot "
                "import what it installs: the cell is compiled before the "
                "install runs. Import it from the next cell.";
        }
        // A SYNTAX error arrives here as a COUNT ("source has 2 syntax
        // error(s)") with no coordinates: the parse aborts after the
        // listener has already emitted the real, located diagnostics, and
        // the exception is only the signal to stop. Take the coordinates
        // from the first of those records so the flat fields point at the
        // offending line like every other failure does — a notebook shows
        // the flat message, and `line = -1` points nowhere.
        if (result.line <= 0) {
            bridge.finish();               // closes the stream, parses records
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
            // When there WAS, the account is already complete and adding the
            // count summary on top would just be a second, vaguer copy.
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
            // The throw carried the error out of the stream, so it never
            // became a record. Fold it in, so `diagnostics` is the complete
            // account of the cell and a frontend reads one place, not two.
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
    // Compilation is done; everything after this is delivery and execution,
    // and the cell's own stdout must not land in the diagnostic buffer.
    bridge.finish();

    // Everything this cell's codegen produced that has not been delivered
    // yet: the cell's own module, plus any module its instantiations landed
    // in (the stdlib module accumulates specializations).
    std::vector<CajetaModulePtr> fresh;
    {
        auto all = impl.compiler->getModules();   // by value — see the
        std::vector<CajetaModulePtr> candidates(all.begin(), all.end());
        // The stdlib module is a separate process-wide module that ACCUMULATES
        // template instantiations as cells use them. It must be delivered too,
        // or every cell fails to materialize on cajeta.lang.Object's vtable and
        // drop thunks — and whether `getModules()` already contains it depends
        // on which session built it: `ensureStdlibModule` pushes it into the
        // building compiler's list and early-returns for every later one. A
        // RESIDENT session inherits a stdlib built by an earlier compiler and
        // so does not have it in the list; a CLASSPATH session builds its own
        // and does. Push unconditionally and let the dedup below decide.
        if (auto stdlib = CajetaModule::getStdlibModule()) {
            candidates.push_back(stdlib);
        }
        // BY IR-MODULE IDENTITY, and a set rather than a bare append: the same
        // llvm::Module reaching `fresh` twice means addIRModule is called twice
        // with the same bitcode, and ORC rejects the second copy with
        // `duplicate definition of symbol X` — where X is whichever name its
        // hash-ordered table hits first, so the message names an arbitrary
        // stdlib symbol and reads exactly like an archive/stdlib collision.
        // That misreading cost 7.2.5 two wrong diagnoses (see the note on the
        // classpath test): every "different symbol family" a fix appeared to
        // advance to was the same duplicate module, renamed by chance.
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

    // lazy-codegen 4.2.4 — an ACCUMULATING module (the stdlib, or any
    // compiled non-cell module) is never delivered whole under lazy: a
    // whole delivery's vtable/RTTI/#ClassObject definitions bind every
    // class's reflect chain the moment the module materializes (measured:
    // 2,906 of ~3,205 bodies at cell 1). Instead its ctor DELTA is
    // extracted below and everything else arrives through the generator.
    // This also fixes late-first-use instantiations for good: the module
    // stays out of impl.delivered, so nothing is ever stranded in it.
    const bool lazyDelivery = cajeta::lazyCodegenEnabled();
    // By IR-module identity, and ONLY the stdlib: a cell's work spans
    // several CajetaModules (script synthesis mints its own unit for the
    // cell class), all of which must deliver whole — comparing against
    // cellModule alone routed the cell's own entry through the generator.
    llvm::Module* stdlibIr = nullptr;
    if (auto stdlibM = CajetaModule::getStdlibModule())
        stdlibIr = stdlibM->getLlvmModule();
    auto accumulating = [&](const CajetaModulePtr& m) {
        return lazyDelivery && stdlibIr
            && m->getLlvmModule() == stdlibIr;
    };

    // Same preparation the per-module delivery path runs: legalize every
    // module before verifying any (a use from B trips A's verifier), then
    // demote instantiations so a specialization shared with an earlier cell
    // is not a duplicate definition. Accumulating modules skip the live-IR
    // passes — their extract runs the same passes on the clone.
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

    // NO ARCHIVE/STDLIB SYMBOL RECONCILIATION HAPPENS HERE, and a pass that
    // does one was removed on 2026-08-15 rather than fixed. It was written
    // for `duplicate definition of 'cajeta.math.Color::linearToSrgbChannel'`
    // on a classpath session, read as "a `.cja` is self-contained, so it
    // brings its own copies of stdlib code". It does not: `ingestClasspath`
    // reads an archive's ClassSource entries, and those are only the
    // archive's OWN classes (144 `dev.cajeta.ml.*` modules and not one
    // `cajeta.*` one, measured). The duplicate was the SESSION's stdlib
    // module reaching `fresh` twice; the pass never demoted a single symbol.
    //
    // Left as a warning, because the failure mode is convincing: ORC names
    // whichever colliding symbol its hash-ordered table reaches first, so
    // each attempt appeared to advance to a new stdlib family (Color, then
    // nucleo.frame.Exec, then reflect.Constructor, then a reflect_invoke
    // thunk) and looked exactly like chasing a large shared set. It was one
    // module, renamed by chance, every time.

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
        // Never strip an accumulating module's live initializers: they are
        // what findLiveDefinition serves (a stripped global is a
        // declaration, and the generator would answer "Symbols not found").
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
        if (accumulating(m)) continue;   // 4.2.4 — extract path, below
        llvm::Module* lm = m->getLlvmModule();
        std::string verifyErr;
        llvm::raw_string_ostream vs(verifyErr);
        if (llvm::verifyModule(*lm, &vs)) {
            // 7.2.5 — a module out of a CLASSPATH ARCHIVE is not this
            // session's work, and failing the cell over it is the wrong
            // trade. `20 + 22` must not be refused because an unused class in
            // a dependency carries malformed IR; ORC materializes lazily, so
            // that code is never compiled unless something calls it — which
            // is exactly why `cajeta jit-run` runs the same archive happily
            // while the kernel's EAGER verify tripped over it. Report it and
            // carry on; if the cell does reach that code, it fails there,
            // which is the same risk every other host already takes.
            if (impl.prebuilt.count(lm)) {
                // Straight onto the result: the diagnostics bridge has
                // already closed by this point (delivery happens after the
                // compile, so a cell's own stdout cannot land in the
                // diagnostic buffer), and reporting into a closed engine
                // would silently go nowhere.
                CellDiagnostic d;
                d.severity = "warning";
                d.code = "CAJETA_WARN_CLASSPATH_IR";
                d.message = "classpath module `" + lm->getModuleIdentifier()
                          + "` does not verify; it is delivered as-is and "
                            "will fail only if a cell calls into it: "
                          + verifyErr;
                d.file = cellName;
                result.diagnostics.push_back(std::move(d));
                // And do not DELIVER it. Malformed IR cannot survive the
                // bitcode round-trip the delivery path uses ("bitcode reparse
                // failed: Invalid cast"), so "deliver it anyway and let ORC
                // decide" is not actually on the menu. Its symbols go
                // missing; a cell that calls into it fails with a
                // symbol-not-found naming the class, which is a diagnosable
                // answer, and a cell that does not is unaffected.
                skipDelivery.insert(lm);
                continue;
            }
            result.errorId = "CAJETA_ERROR_INTERNAL";
            // Name the MODULE. Without it the message is the same whether the
            // cell's own code is malformed, the stdlib accumulated a bad
            // specialization, or a classpath archive was spliced in — three
            // very different problems (7.2.5 was diagnosed wrong once for
            // exactly this reason).
            result.message = "module verify failed [" + lm->getModuleIdentifier()
                           + "]: " + verifyErr;
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
        if (accumulating(m)) continue;   // 4.2.4 — extract path, below
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
        // The whole delivery just initialized these ctors; a later init
        // delta over this module must carry only ctors born AFTER this
        // point (a late keep's registration), never re-run them.
        if (lazyDelivery) cajeta::recordDeliveredCtors(lm, impl.deliveredCtors);
        for (auto& g : lm->globals()) {
            if (sharedGlobal(g)) impl.definedGlobals.insert(g.getName().str());
        }
    }

    // 4.2.4 — init deltas: the not-yet-delivered ctors (statics + kept
    // registrations) and their closure, nothing else. Over EVERY compiled
    // module, not just the accumulating ones: a later cell's keep can mint a
    // registration ctor in an ALREADY-DELIVERED cell module (forcesAll, or a
    // literal naming an earlier cell's class), and without a delta that ctor
    // is stranded exactly like the pre-existing late-instantiation gap.
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
        if (!(*delta)) continue;   // nothing new since the last cell
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

    // jupyter-kernel 2.1.4 (script-units 5.4) — a BODY-ONLY redefinition
    // swaps the bodies of a class that already has live instances. The
    // front-end kept the class's identity (same struct, same symbols) so
    // those instances stay valid; what is left is that every one of them
    // holds a vtable pointer baked at construction, and that vtable's slots
    // still name the previous cell's functions. Repoint them, in place, now
    // that the new code has been materialized and has addresses.
    //
    // ONE table serves everybody: this cell's own vtable global was turned
    // into a declaration by the session-statics dedup above, so it resolves
    // to the SAME memory the existing objects point at. Patching it reaches
    // values made before the edit and values made after it alike.
    //
    // Offsets come from the vtable StructType through the JIT's DataLayout,
    // never from assuming the header shape — that prefix (version, count,
    // parent_vtable, drop_fn, classObject) is StructureMetadata's business
    // and has grown before.
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
    // Capture the cell's output for the duration of the run (spec 4.1). The
    // handler sees chunks as they are written, from the capture's pump thread;
    // the destructor restores the descriptor and delivers the tail, including
    // on the throw path.
    //
    // The unit result rides a side channel in the session runtime, not the
    // entry's return value (spec 4.2): whether a trailing expression HAS a
    // value is only known after type resolution, long after the entry's
    // signature is fixed. Reached through the JIT's OWN copy of the runtime —
    // `lookupSymbol` walks the same dylibs in the same order the cell's calls
    // resolve through — because the process also links a copy, and reading
    // the wrong one would always report "no result".
    auto* resultClear = reinterpret_cast<void (*)()>(
        lookupSymbol("__cajeta_script_result_clear"));
    auto* resultGet = reinterpret_cast<const char* (*)()>(
        lookupSymbol("__cajeta_script_result_get"));
    if (resultClear) resultClear();
    // U4 (spec 4.4) — the cell runs behind a session-level catch. Without one
    // `__cajeta_throw` finds no exception frame and calls exit(1): the whole
    // kernel, every binding and every earlier cell, gone for one bad line.
    // Resolved through the JIT's own runtime copy, so the frame it pushes is
    // on the same TLS chain the cell's throw walks — the process copy has its
    // own, and a throw would sail straight past it.
    auto* guardCall = reinterpret_cast<void* (*)(int32_t (*)(), int32_t*)>(
        lookupSymbol("__cajeta_session_guard_call"));

    // U6 — the interrupt seam, resolved HERE and once. Not in create(): the
    // runtime's symbols are not resolvable that early, the lookups came back
    // null, and `requestInterrupt` became a silent no-op that looked exactly
    // like an interrupt arriving too late. This is the point where the guard
    // symbol is known to resolve, so it is the point where these do too.
    // Resolving on the EXECUTION thread is also what lets `requestInterrupt`
    // be called from another one: by the time a cell is running the pointers
    // are set and it never has to touch the JIT.
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
    // spec 5.2 — a request that arrived while nothing was running, or while
    // THIS cell was still compiling, is a no-op: the flag never survives into
    // the run. Without this the user's next cell dies for a Ctrl-C they aimed
    // at an already-finished one.
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
        // U6 (spec 5.1): an interrupt arrives as a sentinel ADDRESS, not a
        // Throwable — identity is the whole test, and nothing may dereference
        // it. Rendered here rather than in describeThrow so that function
        // keeps its single job: decoding real thrown objects.
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
        // 2.3.2 — a would-be-UB trap, same shape as the interrupt: a
        // sentinel address, never dereferenced. Without this the cell's
        // `4 / 0` executes `llvm.trap` and the whole kernel dies with SIGILL
        // and nothing said, taking every binding with it.
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
    // Deliberately does nothing but set a flag through a pointer resolved at
    // session creation — no JIT lookup, no lock, no session state. That is
    // what makes it safe to call from the control thread while the execution
    // thread is inside a cell, which is the only time it is any use.
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
    // lazy-codegen 4.2.4 — under init-extract delivery the stdlib's
    // definitions live in the MAIN dylib, where the generator delivers
    // them; a cell's calls resolve there through its link order, so the
    // session must read the SAME copy (the result side-channel above all:
    // reading any other copy reports "no result" forever). Last, so a
    // cell's own definition still wins every earlier lookup.
    if (auto sym = impl.jit->lookup(impl.jit->getMainJITDylib(), exactName)) {
        return reinterpret_cast<void*>(sym->getValue());
    } else {
        llvm::consumeError(sym.takeError());
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
            // Spec 4.4: a frame inside a cell's own entry is the CELL, and
            // renders as such. Everything the user did not write — the
            // implicit class, the synthesized entry's name — is scaffolding,
            // and naming it in a traceback only invites the question "what is
            // cajeta.script.cell_3?".
            // `<script>` is what the frame descriptor carries for a script
            // unit's entry (script-units U5 names it that rather than leaking
            // the implicit class); the other two forms are what an
            // unremapped descriptor would carry.
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
    // A trace is best-effort — capture can be off, and a throw from inside a
    // fiber records none. The payload must still name the cell, or the
    // notebook shows an error from nowhere.
    if (result->traceback.empty()) {
        CellFrame f;
        f.file = cellName;
        f.text = cellName;
        result->traceback.push_back(std::move(f));
    }
}

// notebook-olla-install 6.1.2 (spec 4.3) — does this archive declare a
// canonical name the session already holds?
//
// Pure archive I/O plus registry lookups: no compiler pass, so unlike the
// ingest it CAN run while a cell is mid-execution. That is the whole point.
// Before this existed, a mid-cell install queued its splice to the cell
// boundary and only discovered the collision at drain — after `install`
// had already handed the cell a version string. The rejection landed in a
// log line no notebook user ever sees, and the next cell failed to import
// with no stated reason.
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
            // Spec 4.3's OTHER arm: "or from an earlier cell". A cell's
            // classes do not keep the package they declare — the script-unit
            // pass rewrites them into the reserved `cajeta.script` package
            // (measured 2026-08-28: `package depx; class Answer` in a cell
            // registers as `cajeta.script.Answer`, never `depx.Answer`). So
            // a canonical-only comparison can never match a cell-declared
            // class, and this arm silently never fired.
            //
            // Compare on the SIMPLE name under that one reserved package.
            // Narrow on purpose: matching bare simple names against the whole
            // registry would reject any archive sharing a class name with the
            // stdlib, and only `cajeta.script` holds cell declarations.
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
        return true;      // unreadable is not "no collision"
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
    if (impl.installedArchives.count(key)) return true;   // idempotent

    // 6.1.2 — answer the CALLER, not the drain. A queued splice reports
    // success to the installing cell, so a collision found later would be
    // invisible; this arm makes the rejection reach the call that caused it.
    std::string collision;
    if (collidesWithSession(key, &collision)) {
        return fail("installArchive: " + collision);
    }
    if (impl.cellExecuting) {
        impl.pendingInstalls.push_back(key);   // spliced at the cell boundary
        return true;
    }

    bool ok = false;
    std::string failMsg;
    // The splice is compiler-side work and may re-enter from JIT'd code
    // mid-cell (the host-hook shape) — same gate, same same-thread
    // recursion discipline as the lazy generator.
    cajeta::CompilerGate::instance().run([&] {
        try {
            // Collision scan BEFORE any mutation (spec 4.3): every class the
            // archive declares must be new to the session. Entry names are
            // path-like ("depx/Answer.cajeta"); the canonical is the dotted
            // stem. find(), never operator[] (registry-poisoning rule).
            //
            // NOT redundant with the eager scan in the caller above: a
            // QUEUED splice drains at the cell boundary, and the rest of
            // that cell can define classes after the eager scan ran. This
            // is the authoritative check; the eager one exists so the
            // common rejection reaches the cell that asked for it.
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

            // Snapshot so only the archive's OWN modules get prebuilt-marked
            // (the create path marks "everything present now"; mid-session
            // the world is full of session work).
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
    // Teardown ORDER is load-bearing (plan 4.2.2). Session bindings drop
    // FIRST, while the carrier pool and the JIT'd code their drop functions
    // reach into are both still live; carriers are joined second — the
    // ordering constraint CajetaJit's destructor documents. Reversing the two
    // would run user drop code, which can await or call back into a cell's
    // own methods, against a runtime already torn down. Each exactly once for
    // the session: a per-cell shutdown would tear the shared pool out from
    // under later cells.
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
    // Give this session's USER struct NAMES back to the shared context.
    //
    // llvm struct types are CONTEXT-owned and the resident context outlives
    // the session, so `%cajeta.script.Point` created here is still registered
    // in the context's NamedStructTypes when the NEXT session in this process
    // declares its own `Point` — which then reuses the previous session's
    // layout and GEPs its own field indices into it. There is no way to
    // delete a type from a context (LLVM allocates them there and frees them
    // only with the context); `setName("")` releasing the symbol-table entry
    // is the whole of the available mechanism, and it is enough, because all
    // that matters is that the name be free for a fresh StructType::create.
    //
    // BY MODULE, not by `canonicalMap` — which is why
    // `CajetaType::releaseThrownTransientStructNames()` does not cover this.
    // That walk finds only what the registry currently maps, and a SUPERSEDED
    // GENERATION is not there: after a redefinition, `cajeta.script.Point`
    // holds generation 2, and generation 1's struct — the one that leaks — is
    // unreachable from any key. The modules this session delivered still hold
    // every generation it built.
    //
    // The stdlib's own structs are PRESERVED: they are baseline-resident and
    // the next session reuses them by name on purpose.
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
