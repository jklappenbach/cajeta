//
// Created by James Klappenbach on 10/24/22.
//

#include "Compiler.h"
#include "CajetaArchive.h"
#include "cajeta/buildtool/skill/SkillPackager.h"
#include "CajetaModule.h"
#include "NativeLink.h"
#include "CajetaLlvmVisitor.h"
#include "Optimizer.h"
#include "StdlibEmbedded.h"
#include "cajeta/runtime/EmbeddedTls.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/Analysis/ModuleSummaryAnalysis.h"   // buildModuleSummaryIndex (ThinLTO)
#include "llvm/Analysis/ProfileSummaryInfo.h"       // ProfileSummaryInfo (summary input)
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/SubtargetFeature.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include "../asn/AbstractSyntaxNode.h"
#include "../type/CajetaType.h"
#include "../type/CajetaArray.h"
#include "../type/FormalParameter.h"
#include "../type/QualifiedName.h"
#include "cajeta/error/CajetaExceptions.h"
#include "CajetaParserBaseVisitor.h"
#include "../xpu/core/XpuAttributes.h"
#include "../xpu/XpuTarget.h"
#include "../xpu/nvidia/NvptxBackend.h"
#include "../xpu/nvidia/NvptxKernelLowering.h"
#include "../xpu/amd/AmdgpuBackend.h"
#include "../xpu/amd/AmdgpuKernelLowering.h"
#include "../xpu/vulkan/SpirvBackend.h"
#include "../xpu/vulkan/SpirvKernelLowering.h"
#include "../xpu/cpu/CpuBackend.h"
#include "../xpu/cpu/CpuKernelLowering.h"
#include "../method/Method.h"
#include "cajeta/buildtool/Subprocess.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Analysis/ValueTracking.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sys/stat.h>
#include <unordered_map>
#include <unordered_set>

#ifdef CAJETA_HAS_LLD
#include "lld/Common/Driver.h"
LLD_HAS_DRIVER(elf)
#endif


using namespace antlr4;
using namespace std;

namespace cajeta {

    // 0c diagnostic: serialize the lean keep-set + per-class provenance to JSON.
    // Generated artifact (banner says do-not-edit); the leanness feedback surface.
    static void writeKeepsetJson(const std::string& path,
                                 const std::map<std::string, std::string>& keptBy,
                                 bool forcesAll) {
        auto esc = [](const std::string& s) {
            std::string o;
            for (char c : s) {
                if (c == '"' || c == '\\') { o += '\\'; o += c; }
                else o += c;
            }
            return o;
        };
        std::ofstream out(path);
        if (!out) {
            std::cerr << "warning: could not write keepset JSON to " << path << "\n";
            return;
        }
        out << "{\n";
        out << "  \"_comment\": \"GENERATED — DO NOT EDIT. cajeta lean keep-set "
               "(--link-mode=lean).\",\n";
        out << "  \"linkMode\": \"lean\",\n";
        out << "  \"forcesAll\": " << (forcesAll ? "true" : "false") << ",\n";
        if (forcesAll) {
            out << "  \"count\": null,\n";
            out << "  \"classes\": []\n";
        } else {
            out << "  \"count\": " << keptBy.size() << ",\n";
            out << "  \"classes\": [\n";
            size_t i = 0;
            for (auto& [canon, reason] : keptBy) {
                out << "    {\"class\": \"" << esc(canon)
                    << "\", \"keptBy\": \"" << esc(reason) << "\"}"
                    << (++i < keptBy.size() ? "," : "") << "\n";
            }
            out << "  ]\n";
        }
        out << "}\n";
    }

    // Null in production: every Compiler owns its context and resets globals.
    // The test StdlibCache installs a shared context here so a primed stdlib
    // survives across Compiler instances (see Compiler ctor).
    llvm::LLVMContext* Compiler::s_sharedContext = nullptr;
    bool Compiler::s_sharedInitialized = false;
    bool Compiler::s_reuseHazardArmed = false;

    void Compiler::rebuildTargetMachine() {
        string error;
        // LLVM 21 dropped the std::string overloads of lookupTarget /
        // createTargetMachine in favor of llvm::Triple. Build it once.
        llvm::Triple triple(targetTriple);
        target = llvm::TargetRegistry::lookupTarget(triple, error);
        if (!target) {
            cerr << "cajeta: could not lookup target '" << targetTriple << "': " << error << std::endl;
            targetMachine = nullptr;
            return;
        }
        // Default to PIC so .o files link cleanly into PIE executables — modern
        // Linux distros (Ubuntu ≥ 17.04, etc.) configure their toolchains to
        // produce PIE by default, and a non-PIC object trips
        //   relocation R_X86_64_32 against symbol `foo' can not be used
        //   when making a PIE object
        // Caller can still override RM via setRelocationModel for embedded /
        // kernel targets that want absolute addressing.
        auto effectiveRM = RM.has_value() ? RM : std::optional<llvm::Reloc::Model>(llvm::Reloc::PIC_);
        // Emit one ELF section per function and per data global. The linker's
        // --gc-sections pass can then drop sections nothing references —
        // critical for HelloWorld-class programs that link against the full
        // stdlib (parsed once into a single .o) but exercise only a handful
        // of stdlib symbols. Without per-symbol sections, --gc-sections
        // can't safely drop individual functions / globals and the binary
        // carries all of JSON / hashing / parallel-stream / etc.
        opt.FunctionSections = true;
        opt.DataSections     = true;
        // Emit llvm.global_ctors as `.init_array` (modern ELF), not the legacy
        // `.ctors` section. TargetOptions defaults UseInitArray to false, which
        // makes the AsmPrinter emit `.ctors` — a section modern glibc startup
        // does NOT run, so every AOT global constructor (per-class clinit, the
        // UnrecoverableException vtable marker, the embedded runtime's
        // __attribute__((constructor)) init, and the XPU kernel/backend
        // registration ctors) silently never fired. clang/llc set this; we must
        // too for the --emit=obj/exe path to honor static initializers.
        opt.UseInitArray     = true;
        // `--cpu=native` resolves to the host's LLVM CPU name plus its detected
        // feature set — the equivalent of clang/gcc `-march=native`. Without
        // this the default `generic` target only emits the baseline ISA (e.g.
        // x86-64 SSE2), leaving AVX2/AVX-512/FMA on the table; resolving native
        // lets the optimizer's loop/SLP vectorizers use the host's wide vectors.
        std::string effectiveCpu = cpu;
        std::string effectiveFeatures = features;
        if (cpu == "native") {
            effectiveCpu = llvm::sys::getHostCPUName().str();
            llvm::SubtargetFeatures feats(features); // seed with any explicit --features
            for (const auto& f : llvm::sys::getHostCPUFeatures())
                feats.AddFeature(f.first(), f.second);
            effectiveFeatures = feats.getString();
        }
        targetMachine = target->createTargetMachine(triple, effectiveCpu, effectiveFeatures, opt, effectiveRM);
    }

    // ANTLR-based pre-scan visitor. Walks the parse tree shallowly
    // looking for typeDeclaration contexts at top-level and nested
    // inside class bodies. For each declared class/interface/struct,
    // registers (canonical, shortName) in the archive so cross-
    // file forward references in the main visitor pass can create
    // placeholders only for names that are actually declared
    // somewhere in the compilation unit.
    //
    // ANTLR was chosen over regex because cajeta supports nested
    // type declarations (class-inside-class) whose canonical name
    // depends on the enclosing class stack; a flat regex can spot
    // the names but can't compose the right canonical, and regex
    // is fragile around string literals and comments anyway.
    class ArchivePrescanVisitor : public CajetaParserBaseVisitor {
    public:
        std::string package;
        std::vector<std::string> enclosingStack;

        std::any visitPackageDeclaration(
                CajetaParser::PackageDeclarationContext* ctx) override {
            std::vector<CajetaParser::IdentifierContext*> ids =
                ctx->qualifiedName()->identifier();
            std::string p;
            for (size_t i = 0; i < ids.size(); ++i) {
                if (i) p += ".";
                p += ids[i]->getText();
            }
            package = p;
            return defaultResult();
        }

        std::any visitClassDeclaration(
                CajetaParser::ClassDeclarationContext* ctx) override {
            // markValueType so fromContext's placeholder synthesis builds a
            // cross-file `Vec2 a;` declaration's type carrying VALUE_TYPE_FLAG |
            // BY_VALUE_FLAG from birth — the stale-instance fix (mirrors
            // markEnum). Detected here because the @ValueType annotation sits on
            // the enclosing typeDeclaration's modifiers, not the class body.
            registerAndRecurse(ctx->identifier()->getText(), ctx,
                                /*markEnum=*/false,
                                /*markValueType=*/classHasValueTypeAnnotation(ctx));
            captureTemplateMeta(ctx);
            return defaultResult();
        }

        std::any visitInterfaceDeclaration(
                CajetaParser::InterfaceDeclarationContext* ctx) override {
            // markInterface=true so fromContext's placeholder synthesis
            // builds a FAT 24-byte interface pointer for a forward-
            // referenced interface-typed field/param/local (e.g.
            // `ByteChannel stream;` in AsyncReader, parsed before
            // ByteChannel.cajeta). Without the mark the placeholder is a
            // thin class pointer and interface dispatch through such a
            // field is silently dropped at codegen.
            registerAndRecurse(ctx->identifier()->getText(), ctx,
                                /*markEnum=*/false, /*markValueType=*/false,
                                /*markInterface=*/true);
            captureTemplateMeta(ctx);
            return defaultResult();
        }

        std::any visitViewDeclaration(
                CajetaParser::ViewDeclarationContext* ctx) override {
            registerAndRecurse(ctx->identifier()->getText(), ctx);
            return defaultResult();
        }

        std::any visitEnumDeclaration(
                CajetaParser::EnumDeclarationContext* ctx) override {
            // markEnum=true so fromContext's placeholder synthesis
            // builds an i32 enum CajetaType for cross-file field
            // declarations referencing this name. Without the mark,
            // the placeholder would be a class-shaped CajetaClass —
            // wrong layout for enum-typed fields, and trips the
            // "return value lowered to null" error at codegen.
            registerAndRecurse(ctx->identifier()->getText(), ctx,
                                /*markEnum=*/true);
            return defaultResult();
        }

    private:
        // True if the class declaration is annotated @ValueType. The
        // annotation lives on the enclosing typeDeclaration's
        // classOrInterfaceModifier list (the `@ValueType` precedes the
        // `class` keyword), so reach up to the parent and scan modifiers.
        static bool classHasValueTypeAnnotation(
                CajetaParser::ClassDeclarationContext* ctx) {
            auto* td = dynamic_cast<CajetaParser::TypeDeclarationContext*>(
                ctx->parent);
            if (!td) return false;
            for (auto* mod : td->classOrInterfaceModifier()) {
                auto* ann = mod->annotation();
                if (!ann) continue;
                auto* qn = ann->qualifiedName();
                if (!qn) continue;
                auto ids = qn->identifier();
                if (!ids.empty() && ids.back()->getText() == "ValueType") {
                    return true;
                }
            }
            return false;
        }

        void registerAndRecurse(const std::string& shortName,
                                 antlr4::tree::ParseTree* tree,
                                 bool markEnum = false,
                                 bool markValueType = false,
                                 bool markInterface = false) {
            // Compose canonical from package + enclosing class
            // stack + this short name. Mirrors CajetaLlvmVisitor's
            // visitClassDeclaration package-adjustment for nested
            // types.
            std::string canonical;
            if (!package.empty()) canonical = package;
            for (auto& e : enclosingStack) {
                if (!canonical.empty()) canonical += ".";
                canonical += e;
            }
            if (!canonical.empty()) canonical += ".";
            canonical += shortName;
            CajetaType::registerArchive(canonical, shortName);
            if (markEnum) CajetaType::markArchiveEnum(canonical);
            if (markValueType) CajetaType::markArchiveValueType(canonical);
            if (markInterface) CajetaType::markArchiveInterface(canonical);
            lastCanonical = canonical;
            enclosingStack.push_back(shortName);
            visitChildren(tree);
            enclosingStack.pop_back();
        }

        // Capture template metadata for a class / interface declaration that
        // carries a `typeParameters` clause. Mirrors the visitor's parse-time
        // capture (CajetaLlvmVisitor.h:220-247) so a use-site `T<args>`
        // reference in an OTHER file that parses before this one can still
        // instantiate up front. The visitor's later real parse may overwrite
        // these with the same values — harmless.
        template <typename ClassOrInterfaceCtx>
        void captureTemplateMeta(ClassOrInterfaceCtx* ctx) {
            auto* tps = ctx->typeParameters();
            if (!tps) return;
            std::vector<cajeta::TypeParameter> params;
            for (auto* tp : tps->typeParameter()) {
                cajeta::TypeParameter param(tp->identifier()->getText());
                if (auto* pt = tp->primitiveType()) {
                    // Non-type (integer-constant) parameter: `primitiveType identifier`.
                    param.isNonType = true;
                    param.nonTypePrimitive = pt->getText();
                } else {
                    if (auto* bound = tp->typeBound()) {
                        for (auto* tt : bound->typeType()) {
                            if (auto* coi = tt->classOrInterfaceType()) {
                                param.bounds.push_back(cajeta::QualifiedName::fromContext(coi));
                            }
                        }
                    }
                    // Default type argument `<T = float32>`.
                    if (auto* dflt = tp->typeType()) {
                        param.defaultType = dflt->getText();
                    }
                }
                params.push_back(std::move(param));
            }
            // Capture the literal text of the enclosing typeDeclaration so
            // `instantiate(args)` can re-parse the body with substitution.
            antlr4::ParserRuleContext* enclosing = ctx;
            if (auto* td = dynamic_cast<CajetaParser::TypeDeclarationContext*>(ctx->parent)) {
                enclosing = td;
            }
            std::string source;
            auto* startTok = enclosing->getStart();
            auto* stopTok = enclosing->getStop();
            if (startTok && stopTok && startTok->getInputStream()) {
                antlr4::misc::Interval interval(
                    startTok->getStartIndex(), stopTok->getStopIndex());
                source = startTok->getInputStream()->getText(interval);
            }
            CajetaType::registerArchiveTemplate(lastCanonical, params, source);
        }

        std::string lastCanonical;
    };

    // Pre-scan one source via ANTLR.
    static void prescanSource(antlr4::ANTLRInputStream& input) {
        CajetaLexer lexer(&input);
        CommonTokenStream tokens(&lexer);
        tokens.fill();
        CajetaParser parser(&tokens);
        antlr4::tree::ParseTree* tree = parser.compilationUnit();
        ArchivePrescanVisitor v;
        tree->accept(&v);
    }

    // Pre-scan every source file under a root, building the
    // archive registry of class declarations available to the
    // compile. Called once at the start of Compiler::compile(
    // entryMethod, ...) before any modules parse.
    void prescanSourceRoot(const std::string& rootPath) {
        using recursive_directory_iterator = std::filesystem::recursive_directory_iterator;
        std::filesystem::path root(rootPath);
        for (const auto& entry : recursive_directory_iterator(root)) {
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() != CAJETA_EXTENSION) continue;
            std::ifstream in(entry.path());
            if (!in) continue;
            antlr4::ANTLRInputStream input(in);
            prescanSource(input);
        }
    }

    list<string>* listModulePaths(string rootPath) {
        list<string>* result = new list<string>;

        using recursive_directory_iterator = std::filesystem::recursive_directory_iterator;
        std::filesystem::path sourcePath(rootPath);

        // The previous extension filter — `path.string().find(".code")`
        // — was doubly broken: it used the wrong extension (`.code`)
        // and used `find` as a presence test, where the return is a
        // position that's nonzero for nearly every path, so every
        // regular file passed. Today the walker is fed source trees
        // that only contain `.cajeta` files, so the bug hasn't bitten,
        // but it means a stray editor backup or generated artifact
        // under the source root would crash the parser. Match the
        // declared file extension explicitly.
        for (const auto& dirEntry: recursive_directory_iterator(sourcePath)) {
            if (dirEntry.is_regular_file()
                    && dirEntry.path().extension() == CAJETA_EXTENSION) {
                result->push_back(dirEntry.path().string());
            }
        }

        return result;
    }

    // Stdlib prelude — the minimal class hierarchy every Cajeta compilation
    // unit gets implicitly. v1 carries the error-model types from
    // ErrorModel.md (Throwable / Exception / RecoverableException /
    // UnrecoverableException); stdlib/ describes the full
    // intended scope.
    //
    // Sources live as actual `.cajeta` files under runtime/src/cajeta/ and
    // are baked into the compiler binary at CMake-configure time by
    // src/EmbedStdlib.cmake. The generated cajeta::stdlib::g_files manifest
    // exposes each (relativePath, content, contentBytes) tuple; parse()
    // iterates the manifest, treating each entry as a stdlib source.
    // Keeping the stdlib on disk rather than as inline C strings makes it
    // reviewable / editable like any other cajeta code; the bake-in keeps
    // the distribution self-contained.
    //
    // Constructors in the stdlib don't call super(...) — `super` is still
    // UnsupportedExpression. Inherited fields are written directly via
    // `this.message`. Each subclass repeats the field assignment.

    // Run one compilationUnit through the parser/visitor against `module`.
    // Used twice from parse() — first to load the stdlib prelude, then the
    // user source. Two calls on the same module both register their
    // typeDeclarations into module->getStructures() and canonicalMap; the
    // second call's package declaration overwrites the first (the user's
    // package wins, which is what we want — stdlib's package only matters
    // for canonical naming of its own types).
    static void parseSource(CajetaModulePtr module,
                            antlr4::ANTLRInputStream& input,
                            const char* label) {
        CajetaLexer lexer(&input);
        CommonTokenStream tokens(&lexer);
        tokens.fill();
        CajetaParser parser(&tokens);
        antlr4::tree::ParseTree* parseTree = parser.compilationUnit();
        auto prevActive = CajetaModule::getActiveModule();
        CajetaModule::setActiveModule(module);
        auto visitor = new CajetaLlvmVisitor(module);
        parseTree->accept(visitor);
        // Skip the noisy tree dump for the stdlib parse — already-known
        // content, would drown out the user's parse tree in test logs.
        // For user code, the dump is **off by default** and gated behind
        // the OUTPUT_PARSE_TREE env var. Set OUTPUT_PARSE_TREE=true in
        // the test environment when debugging a parser issue; leaving it
        // off in the common case keeps the test log readable and shaves
        // a meaningful chunk of wall-clock off the suite (the tree dump
        // is many KB per file × hundreds of test cases).
        if (label && label[0] != '\0') {
            const char* env = std::getenv("OUTPUT_PARSE_TREE");
            if (env && std::string(env) == "true") {
                cout << "\n\n";
                std::cout << parseTree->toStringTree(&parser, true) << std::endl;
            }
        }
        delete visitor;
        CajetaModule::setActiveModule(prevActive);
    }

    // ───────────────────────────────────────────────────────────────────
    // Lazy stdlib package loading.
    //
    // The embedded stdlib is eagerly prescanned + parsed at Compiler startup
    // (parseStdlibInto). A designated set of packages is instead parsed ON
    // DEMAND — the first time a compile references one (an `import`, or a
    // hardcoded type such as Matrix) — so an unused heavy package (the
    // numpy-equivalent cajeta.math) costs nothing at compile time, the
    // compile-time analogue of the link-time DCE that already makes it free
    // in the output. Mechanism:
    //   - parseStdlibInto skips lazy packages (neither prescanned nor parsed).
    //   - The import hook (CajetaModule::stdlibImportHook, body
    //     noteStdlibImportImpl) prescans a lazy package immediately — so its
    //     class names land in the archive and references in the current parse
    //     resolve to placeholders — and enqueues it.
    //   - drainLazyStdlib(), called after each user-source parse, fully parses
    //     the enqueued packages into the stdlib module and lays them out
    //     (buildPendingPrototypes), filling those placeholders. The
    //     prescan-then-full-parse split mirrors the eager two-phase path.
    // The bookkeeping is process-global (the stdlib statics are too);
    // parseStdlibInto resets it for the fresh-Compiler path, and
    // Compiler::resetLazyStdlibState() lets the reuse harness clear it.

    // dotted package -> indices into cajeta::stdlib::g_files. Built once;
    // g_files is constant for the process, so this survives resetGlobals.
    static const std::map<std::string, std::vector<size_t>>& stdlibPackageIndex() {
        // Built once from the embedded file table, then read-only — shared across
        // threads (C++ guarantees thread-safe init of the function-local static;
        // no mutation after, so concurrent reads are safe). (threadsafe U4 — the
        // one Unit-4 datum kept shared rather than thread_local, per spec §8.3.)
        static const std::map<std::string, std::vector<size_t>> index = [] {
            std::map<std::string, std::vector<size_t>> m;
            for (size_t i = 0; i < cajeta::stdlib::g_fileCount; ++i) {
                std::string rel = cajeta::stdlib::g_files[i].relativePath;
                auto slash = rel.find_last_of('/');
                std::string pkg = (slash == std::string::npos)
                    ? std::string() : rel.substr(0, slash);
                std::replace(pkg.begin(), pkg.end(), '/', '.');
                m[pkg].push_back(i);
            }
            return m;
        }();
        return index;
    }

    static bool isLazyStdlibPackage(const std::string& pkg) {
        // cajeta.math — the numpy-equivalent, including its nested submodules
        // (cajeta.math.linalg / fft / random / stats) — is parsed on demand.
        if (pkg == "cajeta.math" || pkg.rfind("cajeta.math.", 0) == 0) return true;
        // cajeta.xpu.mesh (Qem / MeshSimplifier) is the ONLY eager-prelude
        // consumer of cajeta.math; making it lazy keeps cajeta.math out of the
        // eager prelude (MathLazyParse — guarded by CompilerTests).
        return pkg == "cajeta.xpu.mesh" || pkg.rfind("cajeta.xpu.mesh.", 0) == 0;
    }

    // Instrumentation + lazy bookkeeping. thread_local (threadsafe U4): each
    // thread tracks the lazy packages IT has parsed into its own (thread_local)
    // registries. Units 5-6 share frozen lazy packages under a one-time lock.
    static thread_local std::set<std::string> g_stdlibParsedPackages;  // every pkg parsed (eager + lazy)
    static thread_local std::set<std::string> g_lazyPrescanned;        // lazy pkgs prescanned into the archive
    static thread_local std::set<std::string> g_lazyParsed;            // lazy pkgs fully parsed
    static thread_local std::vector<std::string> g_lazyQueue;          // lazy pkgs awaiting full parse

    static void resetLazyStdlibStateImpl() {
        g_stdlibParsedPackages.clear();
        g_lazyPrescanned.clear();
        g_lazyParsed.clear();
        g_lazyQueue.clear();
    }

    static void prescanStdlibPackage(const std::string& pkg) {
        if (g_lazyPrescanned.count(pkg)) return;
        auto it = stdlibPackageIndex().find(pkg);
        if (it != stdlibPackageIndex().end()) {
            for (size_t idx : it->second) {
                const auto& f = cajeta::stdlib::g_files[idx];
                antlr4::ANTLRInputStream in(
                    std::string(f.content, f.contentBytes));
                prescanSource(in);
            }
        }
        g_lazyPrescanned.insert(pkg);
    }

    // Import-hook body: prescan + enqueue a lazy stdlib package. No-op for
    // eager packages and for packages already fully parsed.
    static void noteStdlibImportImpl(const std::string& pkg) {
        if (!isLazyStdlibPackage(pkg)) return;
        if (g_lazyParsed.count(pkg)) return;
        prescanStdlibPackage(pkg);
        if (std::find(g_lazyQueue.begin(), g_lazyQueue.end(), pkg)
                == g_lazyQueue.end()) {
            g_lazyQueue.push_back(pkg);
        }
    }

    // Fully parse every enqueued lazy package into the stdlib module, then
    // lay them out. Queue-guarded and idempotent; safe to call after every
    // parse (a cheap no-op when the queue is empty).
    static void drainLazyStdlib() {
        if (g_lazyQueue.empty()) return;
        auto stdlib = CajetaModule::getStdlibModule();
        if (!stdlib) { g_lazyQueue.clear(); return; }
        auto prevActive = CajetaModule::getActiveModule();
        CajetaModule::setActiveModule(stdlib);
        QualifiedNamePtr originalQName = stdlib->getQName();
        bool parsedAny = false;
        while (!g_lazyQueue.empty()) {
            std::string pkg = g_lazyQueue.back();
            g_lazyQueue.pop_back();
            if (g_lazyParsed.count(pkg)) continue;
            // Mark parsed BEFORE walking the package's files: a body within the
            // package that references one of its own lazy types (e.g.
            // Matrix.transpose returns Matrix<T,C,R>) re-fires the import hook;
            // the guard above then dedupes rather than re-enqueueing mid-parse.
            g_lazyParsed.insert(pkg);
            prescanStdlibPackage(pkg);   // whole-package archive (idempotent)
            auto it = stdlibPackageIndex().find(pkg);
            if (it == stdlibPackageIndex().end()) continue;
            for (size_t idx : it->second) {
                const auto& f = cajeta::stdlib::g_files[idx];
                std::string rel = f.relativePath;
                auto slash = rel.find_last_of('/');
                std::string fileName = (slash == std::string::npos)
                    ? rel : rel.substr(slash + 1);
                auto dot = fileName.find_last_of('.');
                if (dot != std::string::npos) fileName = fileName.substr(0, dot);
                stdlib->setQName(QualifiedName::getOrInsert(fileName, pkg));
                antlr4::ANTLRInputStream in(
                    std::string(f.content, f.contentBytes));
                parseSource(stdlib, in, /*label=*/"");
            }
            g_stdlibParsedPackages.insert(pkg);
            parsedAny = true;
        }
        stdlib->setQName(originalQName);
        if (parsedAny) {
            CajetaModule::buildPendingPrototypes();
        }
        CajetaModule::setActiveModule(prevActive);
    }

    // Parse every embedded stdlib file into `module`. Called exactly
    // once per Compiler instance, against the Compiler's dedicated
    // stdlib module. Two-step:
    //
    //   1. Pre-scan each file's class names into the archive
    //      registry so forward references between stdlib files
    //      (Exception.cajeta sorts alphabetically before
    //      Throwable.cajeta but references it) can create
    //      placeholders during the body walk.
    //   2. Walk each file. Per-file `package X;` declarations get
    //      checked against the module's qName, so we swap qName to
    //      match each file's path-derived package before invocation.
    //
    // The runtime bitcode is also linked into this module so
    // runtime helpers (__cajeta_new_array, __cajeta_drop_push,
    // etc.) have their definitions co-resident with stdlib code.
    // User modules pick up the runtime through module-local extern
    // declarations that resolve to these definitions at merge time.
    void parseStdlibInto(CajetaModulePtr module) {
        // Lazy-load bookkeeping is process-global; a fresh Compiler re-runs
        // this function, so reset it here. (The reuse harness calls
        // Compiler::resetLazyStdlibState when it restores its baseline.)
        resetLazyStdlibStateImpl();

        // Package (dotted) of stdlib file i, from its embedded relative path.
        auto packageOf = [](size_t i) {
            std::string rel = cajeta::stdlib::g_files[i].relativePath;
            auto slash = rel.find_last_of('/');
            std::string pkg = (slash == std::string::npos)
                ? std::string() : rel.substr(0, slash);
            std::replace(pkg.begin(), pkg.end(), '/', '.');
            return pkg;
        };

        for (size_t i = 0; i < cajeta::stdlib::g_fileCount; ++i) {
            if (isLazyStdlibPackage(packageOf(i))) continue;  // on-demand only
            const auto& f = cajeta::stdlib::g_files[i];
            antlr4::ANTLRInputStream prescanIn(
                std::string(f.content, f.contentBytes));
            prescanSource(prescanIn);
        }

        QualifiedNamePtr originalQName = module->getQName();
        for (size_t i = 0; i < cajeta::stdlib::g_fileCount; ++i) {
            std::string pkg = packageOf(i);
            if (isLazyStdlibPackage(pkg)) continue;           // on-demand only
            const auto& f = cajeta::stdlib::g_files[i];
            std::string relPath = f.relativePath;
            auto lastSlash = relPath.find_last_of('/');
            std::string fileName = (lastSlash == std::string::npos)
                ? relPath
                : relPath.substr(lastSlash + 1);
            auto dotIdx = fileName.find_last_of('.');
            if (dotIdx != std::string::npos) {
                fileName = fileName.substr(0, dotIdx);
            }
            module->setQName(QualifiedName::getOrInsert(fileName, pkg));
            antlr4::ANTLRInputStream stdlibInput(
                std::string(f.content, f.contentBytes));
            parseSource(module, stdlibInput, /*label=*/"");
            g_stdlibParsedPackages.insert(pkg);
        }
        module->setQName(originalQName);

        // Stdlib code may call runtime helpers (e.g. constructors
        // pushing drop entries) — embed the runtime bitcode so
        // those calls resolve in this module's IR.
        module->linkRuntime();
    }

    void parse(CajetaModulePtr module) {
        // User-source parse. Stdlib classes are already in canonicalMap
        // from the Compiler's one-shot stdlib parse, so references like
        // `Throwable` here resolve to the real (not placeholder)
        // CajetaClass. Cross-module IR fixup
        // (CajetaModule::ensureFunctionInModule /
        // ensureGlobalInModule) inserts module-local extern decls when
        // the user references stdlib vtables / methods, which the
        // merge step resolves to the stdlib module's definitions.
        ifstream stream;
        stream.open(module->getSourcePath());
        stream.seekg(0);
        antlr4::ANTLRInputStream userInput(stream);
        parseSource(module, userInput, /*label=*/"user");
        // Fully parse any on-demand stdlib packages this source pulled in
        // (via an import or a hardcoded type like Matrix) before the caller's
        // prototype sweep / placeholder validation runs.
        drainLazyStdlib();
    }

    // Error-model #210: publish UnrecoverableException's vtable address
    // to the runtime so `__cajeta_is_unrecoverable` can match a thrown
    // instance's vtable chain against it and bypass user catch handlers.
    //
    // Mechanism: emit a module global constructor that calls the runtime
    // setter `__cajeta_set_unrecoverable_vtable(UnrecoverableException#VTable)`.
    // This replaced an earlier weak-global-override scheme that only
    // resolved under ELF — on MachO/COFF the JIT never bound the
    // compiler-emitted strong definition over the runtime's weak slot, so
    // JIT-mode detection silently read NULL. A global ctor + plain runtime
    // call resolves identically on ELF/MachO/COFF and in both JIT (LLJIT
    // runs llvm.global_ctors at initialize()) and AOT (the C runtime runs
    // them before main), because runtime symbols are always resolvable and
    // global_ctors is honored by every object format. The Linker merges
    // each module's global_ctors during the JIT module-merge, so the
    // stdlib-emitted ctor carries into the primary module.
    //
    // Must run AFTER CajetaModule::buildPendingPrototypes — the vtable
    // global is created during UnrecoverableException's generatePrototype,
    // which the deferred-prototype machinery may delay until after the
    // module's parse finishes.
    void emitUnrecoverableMarker(CajetaModulePtr module) {
        auto& structures = module->getStructures();
        auto it = structures.find("cajeta.error.UnrecoverableException");
        if (it == structures.end()) return;
        CajetaClassPtr unrecClass = it->second;
        llvm::GlobalVariable* unrecVT = unrecClass->getVirtualTableGlobal();
        if (!unrecVT) return;
        llvm::Module* lmod = module->getLlvmModule();
        const char* ctorName = "__cajeta_register_unrecoverable_vtable";
        if (lmod->getFunction(ctorName)) return;   // re-entry guard

        auto& llvmCtx = *module->getLlvmContext();
        llvm::PointerType* ptrTy = llvm::PointerType::get(llvmCtx, 0);
        // Setter is defined by the linked runtime bitcode.
        llvm::FunctionType* setterTy = llvm::FunctionType::get(
            llvm::Type::getVoidTy(llvmCtx), {ptrTy}, /*isVarArg=*/false);
        llvm::FunctionCallee setter = lmod->getOrInsertFunction(
            "__cajeta_set_unrecoverable_vtable", setterTy);

        llvm::FunctionType* ctorTy = llvm::FunctionType::get(
            llvm::Type::getVoidTy(llvmCtx), /*isVarArg=*/false);
        llvm::Function* ctor = llvm::Function::Create(
            ctorTy, llvm::Function::PrivateLinkage, ctorName, lmod);
        llvm::BasicBlock* bb = llvm::BasicBlock::Create(llvmCtx, "entry", ctor);
        llvm::IRBuilder<> b(bb);
        b.CreateCall(setter, {unrecVT});
        b.CreateRetVoid();

        // Same priority bucket as the clinit ctors; order among them is
        // irrelevant — this one only needs to run before any user code.
        llvm::appendToGlobalCtors(*lmod, ctor, /*Priority=*/65535);
    }

    bool fileExists(string& sourcePath) {
        struct stat buffer;
        return (stat(sourcePath.c_str(), &buffer) == 0);
    }

    CajetaModulePtr Compiler::createModule(string sourcePath, string sourceRootPath, string targetRootPath) {
        if (!fileExists(sourcePath))
            throw FileNotFoundException(sourcePath);

        auto module = make_shared<CajetaModule>(activeContext,
            sourcePath,
            sourceRootPath,
            targetRootPath,
            targetTriple,
            targetMachine);
        // Propagate compiler-level flags so codegen for this module respects them.
        module->setFlags(flags);
        // Reproducible builds: now that flags (with --debug-prefix-map) are
        // set, scrub the absolute source path the constructor embedded so the
        // emitted IR is byte-identical across hosts.
        module->canonicalizeSourceFileName();
        return module;
    }

    // Read every classpath archive, find each ClassSource entry, and
    // re-parse it into a fresh CajetaModule registered in the
    // canonical-name map. After this returns:
    //   - Every class declared in any classpath archive is in the
    //     canonical-name map; user code's `import deplib.Util;` and
    //     direct-canonical references like `deplib.Util.ten()` resolve.
    //   - The classpath modules live in `externalModules` (not in
    //     `modules`); the emitter never writes their IR / bitcode out.
    //     User code that calls into them produces extern decls at
    //     codegen time, resolved at link/uber-bundle time against the
    //     dep's own bitcode.
    void Compiler::ingestClasspath() {
        if (classpath.empty()) return;

        // Phase 1 — prescan. Pre-register every classpath class's
        // canonical name in the archive registry so forward refs from
        // ONE classpath archive into another (e.g. `Json` extending
        // `cajeta.lang.Object` is fine without prescan since stdlib's
        // already there; but `deplib.Util` extending `otherlib.Base`
        // needs both names available before either body walks) work
        // exactly like the user-source prescan does.
        for (const auto& cpPath : classpath) {
            try {
                auto arc = CajetaArchive::readFrom(cpPath);
                for (const auto& entry : arc.getEntries()) {
                    if (entry.kindTag != CajetaArchive::EntryKind::ClassSource)
                        continue;
                    std::string text(
                        (const char*) entry.data.data(), entry.data.size());
                    antlr4::ANTLRInputStream input(text);
                    prescanSource(input);
                }
            } catch (const std::exception& e) {
                std::cerr << "cajeta: --classpath read failed for `"
                          << cpPath << "`: " << e.what() << std::endl;
                throw;
            }
        }

        // Phase 2 — full parse. Each ClassSource entry becomes a
        // standalone CajetaModule. The module's qName is derived from
        // the entry name (`deplib/Util.cajeta` → `deplib.Util`); its
        // LLVM module is a throwaway (vtables / RTTI for the class
        // get emitted there, but we never write the module out — the
        // authoritative bitcode is the classpath archive's own `.bc`).
        for (const auto& cpPath : classpath) {
            auto arc = CajetaArchive::readFrom(cpPath);
            for (const auto& entry : arc.getEntries()) {
                if (entry.kindTag != CajetaArchive::EntryKind::ClassSource)
                    continue;

                // entry.name has the shape `<pkg-slashed>/<Class>.cajeta`.
                std::string entryName = entry.name;
                const std::string suffix = ".cajeta";
                if (entryName.size() <= suffix.size()
                    || entryName.compare(entryName.size() - suffix.size(),
                        suffix.size(), suffix) != 0) {
                    continue;
                }
                std::string canonicalPath = entryName.substr(
                    0, entryName.size() - suffix.size());
                auto slashIdx = canonicalPath.rfind('/');
                std::string pkg = (slashIdx == std::string::npos)
                    ? std::string()
                    : canonicalPath.substr(0, slashIdx);
                std::string cls = (slashIdx == std::string::npos)
                    ? canonicalPath
                    : canonicalPath.substr(slashIdx + 1);
                std::replace(pkg.begin(), pkg.end(), '/', '.');

                auto qName = QualifiedName::getOrInsert(cls, pkg);
                auto extMod = std::make_shared<CajetaModule>(
                    activeContext, qName, targetTriple, targetMachine);
                extMod->setFlags(flags);
                externalModules.push_back(extMod);

                auto prevActive = CajetaModule::getActiveModule();
                CajetaModule::setActiveModule(extMod);
                std::string text(
                    (const char*) entry.data.data(), entry.data.size());
                antlr4::ANTLRInputStream input(text);
                parseSource(extMod, input, /*label=*/"");
                CajetaModule::setActiveModule(prevActive);
            }
        }

        // Lay out every parsed classpath class so its vtable / RTTI
        // globals exist in its (throwaway) LLVM module, and — more
        // importantly — its methods become resolvable LLVM Functions
        // that user code's codegen can wire to via extern decls.
        // Without this, user calls to classpath methods come back as
        // null llvm::Value at codegen time and the return gets
        // silently lowered to null. Mirrors the stdlib's own
        // ensureStdlibModule → buildPendingPrototypes flow.
        CajetaModule::buildPendingPrototypes();
    }

    CajetaModulePtr Compiler::ensureStdlibModule() {
        // Lazy stdlib: install the on-demand import hook (idempotent). Set
        // unconditionally so the reuse path — where the stdlib module already
        // exists and we early-return below — still has it bound.
        CajetaModule::stdlibImportHook = [](const std::string& pkg) {
            noteStdlibImportImpl(pkg);
        };

        auto existing = CajetaModule::getStdlibModule();
        if (existing) return existing;

        auto stdlibQName = QualifiedName::getOrInsert(
            "__stdlib__", "cajeta.runtime");
        auto stdlib = make_shared<CajetaModule>(
            activeContext, stdlibQName, targetTriple, targetMachine);
        stdlib->setFlags(flags);
        CajetaModule::setStdlibModule(stdlib);
        modules.push_back(stdlib);

        auto prevActive = CajetaModule::getActiveModule();
        CajetaModule::setActiveModule(stdlib);
        parseStdlibInto(stdlib);
        CajetaModule::setActiveModule(prevActive);

        // Lay out every parsed stdlib class so its vtable / RTTI
        // globals live in this module — user modules will reach
        // them via extern decls and never need to re-prototype.
        CajetaModule::buildPendingPrototypes();

        // REFL-1.7: cajeta.reflect.Class is a template Class<T>. Force-build the
        // canonical Class<?> instantiation HERE, as part of the stdlib build and
        // BEFORE any user module parses. Class<?>'s method bodies reference the
        // reflect object types (Modifiers / Field / Method / Constructor /
        // Parameter / Annotation / TemplateParameter / TemplateArgument); with
        // the concrete `Class` gone, this instantiation is now the only thing
        // that pulls those types into the canonical map. Doing it here makes them
        // resolvable when user code names them directly (e.g. `Modifiers m = ...`)
        // — the eager-build the concrete `Class` used to provide. Also gives
        // every #ClassObject a real Class<?> vtable to embed.
        CajetaModule::setActiveModule(stdlib);
        CajetaClass::ensureClassWildcardInstantiated();
        CajetaModule::buildPendingPrototypes();
        CajetaModule::setActiveModule(prevActive);

        emitUnrecoverableMarker(stdlib);
        return stdlib;
    }

    bool Compiler::stdlibPackageParsed(const std::string& pkg) {
        return g_stdlibParsedPackages.count(pkg) != 0;
    }

    const std::set<std::string>& Compiler::stdlibParsedPackages() {
        return g_stdlibParsedPackages;
    }

    void Compiler::resetLazyStdlibState() {
        resetLazyStdlibStateImpl();
    }

    void Compiler::compile(CajetaModulePtr module) {
        ensureStdlibModule();
        modules.push_back(module);
        parse(module);
        // Single-module entry point — drive the prototype sweep so
        // callers that use this entry directly (rather than the
        // multi-file compile(entryMethod, ...) overload) don't have
        // to remember to. Idempotent — when multiple modules go
        // through this entry sequentially or when the multi-file
        // overload also runs it at the end, the re-runs are cheap
        // no-ops. The unrecoverable-marker emit was previously here
        // too, but the marker now lives in the stdlib module and is
        // emitted by ensureStdlibModule, so no per-user-module emit
        // is required.
        CajetaModule::buildPendingPrototypes();
    }

    void Compiler::compile(string entryMethod, string sourceRootPath, string archiveRootPath) {
        // Stash on the instance so the post-Phase-2 emitCMainShim call can
        // see what the user passed without threading it through Phase 1/2.
        this->entryMethod = entryMethod;

        // DCE Tier-0b: clear the compile-scoped reflection-usage accumulator.
        // The compiler process is reused across compiles (the stdlib-prime
        // cache), so a flag left set by a prior reflection-using build would
        // wrongly force keep-all here. Repopulated during the codegen loop.
        CajetaModule::resetReflectionKeep();

        if (sourceRootPath[sourceRootPath.size() - 1] != '/') {
            sourceRootPath.append("/");
        }

        if (archiveRootPath[archiveRootPath.size() - 1] != '/') {
            archiveRootPath.append("/");
        }

        // Remember where the package's hand-authored skills/ lives so
        // emitArchive can embed them into the .cja (skill-discovery D.3). The
        // build tool passes --skill-root=<project-root> (skills/ sits next to
        // cajeta.json, not under the deeper src/main/cajeta source root); the
        // low-level compile form has no override and falls back to the source
        // root.
        this->skillSourceRoot =
            this->skillRootOverride.empty() ? sourceRootPath
                                            : this->skillRootOverride;

//        std::filesystem::path cwd = std::filesystem::current_path();

        ensureStdlibModule();

        // Stdlib module was constructed without an archiveRoot (its alt ctor
        // doesn't take one). For binary emit (--emit=obj/exe) we need the .o
        // to land alongside the user modules' .o files, so retro-fit the
        // archive root onto it now that we know it. Harmless for IR emit;
        // writeIRFileTarget concatenates archiveRoot + archivePath either
        // way.
        if (auto stdlib = CajetaModule::getStdlibModule()) {
            stdlib->setArchiveRoot(archiveRootPath);
        }

        // Ingest classpath archives — read each `.cja`'s ClassSource
        // entries, re-parse them into externalModules, and register
        // their CajetaClass objects in the canonical-name map BEFORE
        // user-source prescan starts. User imports like
        // `import deplib.Util;` and direct refs like
        // `deplib.Util.ten()` then resolve like any other compile-unit
        // class. The classpath modules' own LLVM bitcode is never
        // emitted — definitions live in each classpath archive's `.bc`
        // entries, which the link / uber-bundle step consumes.
        ingestClasspath();

        // Pre-scan: enumerate every class/interface/struct declared
        // anywhere under sourceRootPath into the archive registry.
        // Lets fromContext's miss path vouch for forward references
        // before deciding placeholder vs unknown-type error.
        prescanSourceRoot(sourceRootPath);

        list<string>* modulePaths = listModulePaths(sourceRootPath);

        for (string sourcePath: *modulePaths) {
            CajetaModulePtr module = createModule(sourcePath, sourceRootPath, archiveRootPath);
            compile(module);
            cout << "\n";
        }

//        Method* method = Method::getArchive()[entryMethod];
//        if (method == nullptr) {
//            return;
//        }

        // Forward-reference validation. Catches any placeholder
        // CajetaClass created during the parse passes that no
        // visitClassDeclaration ever filled in — i.e., the archive
        // pre-scan vouched for a name that didn't actually arrive
        // via a real declaration. Defense-in-depth; normal flow
        // sees zero placeholders left.
        CajetaModule::validatePlaceholders();

        // Drive deferred-prototype layout to fixed point. Classes whose
        // visitClassDeclaration deferred (parents were placeholders at
        // visit time) get their generatePrototype call here once the
        // parents are filled in.
        CajetaModule::buildPendingPrototypes();

        // The unrecoverable-vtable marker lives in the stdlib module
        // (alongside UnrecoverableException's vtable and the runtime
        // that reads it). User modules reach it through extern decls
        // at merge time, so we only emit the definition once here.
        if (auto stdlib = CajetaModule::getStdlibModule()) {
            emitUnrecoverableMarker(stdlib);
        }

        // AspectModel.md § A3 pointcut-matching pass. Runs after
        // every module has been parsed (all classes + advice methods
        // registered, all annotations captured with their args) and
        // before Phase 1 starts emitting prototypes. The pass
        // populates each user method's matchingAdvice list so A4+'s
        // codegen wrappers can find their advice at IR emit time.
        CajetaModule::resolveAdviceMatches();

        // AspectModel.md § A8 DI graph validation. Same parse-
        // complete point as A3; it validates @Component / @Inject
        // shape and reports missing/cycle/ambiguous errors before
        // any IR emission. A9 will read the resolved graph to
        // synthesize singleton + factory helpers.
        CajetaModule::resolveDependencyGraph();

        // REFL-1.7: cajeta.reflect.Class is a template Class<T>. Force-build the
        // canonical wildcard instantiation Class<?> here — after all modules are
        // parsed/prototyped, before Phase 1/2 codegen — so (a) its method bodies
        // are emitted in the loop below and (b) every type's #ClassObject can
        // embed its vtable (StructureMetadata::populate / finalizeClassObject
        // look it up by "cajeta.reflect.Class<?>"). The phantom T means all
        // Class<T> share identical code, so this one instantiation backs every
        // reflected type's #ClassObject regardless of the static T.
        CajetaClass::ensureClassWildcardInstantiated();

        // Phase 1 (signatures) + Phase 2 (bodies), looped until quiescent.
        // A user method body can trigger a stdlib template instantiation
        // mid-codegen (e.g. `xs.stream()` → ArrayStream<int32>); the new
        // methods land in the stdlib module's structures AFTER stdlib's
        // earlier pass already ran. The do/while re-iterates both
        // phases — both Method::getLlvmFunctionType and ::generateCode
        // are idempotent so already-emitted methods cost nothing to
        // revisit. Per-module emitForModule moves out of the loop and
        // runs once after quiescence so each module's IR / .o is written
        // exactly once, with the freshest method set.
        // Binary emit (--emit=obj/exe) must LINK the classpath deps, not just
        // resolve their declarations: a user call into a dep is an extern at
        // codegen, and the dep's published .cja bitcode is a stdlib-stripped
        // library (its template instantiations live as `cajeta.*` canonicals the
        // archive drops). Re-driving the dep's bodies through the consumer's own
        // codegen below emits them target-correct AND pulls in exactly the stdlib
        // instantiations they need; gc-sections then strips whatever the entry
        // never reaches. Archive/IR emit keep deps external (declarations only).
        const bool linkClasspathDeps =
            (emitMode == EmitMode::Obj || emitMode == EmitMode::Exe)
            && !externalModules.empty();

        size_t prevMethodCount = 0;
        while (true) {
            // Rebuilt each iteration so modules added mid-codegen (a body that
            // triggers a fresh template instantiation) are picked up next pass.
            std::vector<CajetaModulePtr> codegenModules(
                modules.begin(), modules.end());
            if (linkClasspathDeps) {
                codegenModules.insert(codegenModules.end(),
                    externalModules.begin(), externalModules.end());
            }

            size_t methodCount = 0;
            for (auto& module: codegenModules) {
                methodCount += module->getAllMethods().size();
            }
            for (auto& module: codegenModules) {
                for (auto& method: module->getAllMethods()) {
                    method->getLlvmFunctionType();
                }
            }
            for (auto& module: codegenModules) {
                for (auto& method: module->getAllMethods()) {
                    method->generateCode();
                }
            }
            size_t after = 0;
            for (auto& module: codegenModules) {
                after += module->getAllMethods().size();
            }
            if (after == methodCount && after == prevMethodCount) break;
            prevMethodCount = after;
        }
        // DCE Tier-0b-2a — keep-set (see lean-linker-dce.md §3.2). Codegen has
        // quiesced (reflectionKeep() is final) and finalizeClassObject below
        // reads keepsClass(). No non-reflect reflection ⇒ keep only @Retained;
        // forcesAll ⇒ leave NULL (keep-all). 0b-2b unions narrow per-site sets.
        if (flags.linkMode == LinkMode::Lean) {
            auto& rk = CajetaModule::reflectionKeep();
            // 0c classifier: warn on every forces-ALL site. The build still
            // succeeds and is sound (conservative keep-all); the warning just
            // points at the selector to tighten.
            for (auto& reason : rk.forceAllReasons) {
                std::cerr << "warning: [reflection-forces-keep-all] " << reason
                          << " — the lean linker retains the whole class registry"
                             " for this build.\n";
            }
            if (!rk.forcesAll) {
                auto keep = std::make_shared<std::set<std::string>>();
                // canon -> first reason it was kept (provenance for --why-kept
                // and cajeta.keepset.json).
                std::map<std::string, std::string> keptBy;
                auto addKeep = [&](const std::string& canon,
                                   const std::string& reason) {
                    keep->insert(canon);
                    keptBy.emplace(canon, reason);
                };
                // Key by toCanonical(): the canonicalMap reaches a class via both
                // its short-name alias and its FQ name, but keepsClass() (and the
                // reg-ctor) only ever uses toCanonical() — so dedup to that, or the
                // keep-set/keepset.json over-report aliases that never register.
                std::vector<std::pair<std::string, CajetaClassPtr>> classes;
                for (auto& [mapKey, type] : CajetaType::getCanonicalMap()) {
                    if (auto k = std::dynamic_pointer_cast<CajetaClass>(type)) {
                        std::string canon = k->toCanonical();
                        classes.emplace_back(canon, k);
                        if (k->getModifiers().count(REFLECT_RETAINED) > 0) {
                            addKeep(canon, "@Retained keep-pin");
                        }
                    }
                }
                std::function<bool(const CajetaClassPtr&, const std::string&)>
                    derivesFrom = [&](const CajetaClassPtr& c,
                                      const std::string& t) -> bool {
                        if (!c) return false;
                        if (c->toCanonical() == t) return true;
                        for (auto& p : c->getSuperClasses()) {
                            if (derivesFrom(p, t)) return true;
                        }
                        return false;
                    };
                for (auto& site : rk.sites) {
                    using RS = CajetaModule::ReflSite;
                    switch (site.kind) {
                        case RS::BoundClosure:
                            for (auto& [canon, k] : classes) {
                                if (derivesFrom(k, site.selector))
                                    addKeep(canon,
                                        "subtype closure of " + site.selector);
                            }
                            break;
                        case RS::ForNameLiteral:
                            if (CajetaType::getCanonicalMap().count(site.selector))
                                addKeep(site.selector,
                                    "forName(\"" + site.selector + "\")");
                            break;
                        case RS::PackageLiteral:
                            for (auto& [canon, k] : classes) {
                                auto p = canon.rfind('.');
                                if (p != std::string::npos
                                        && canon.substr(0, p) == site.selector) {
                                    addKeep(canon,
                                        "classesInPackage(\"" + site.selector + "\")");
                                }
                            }
                            break;
                        case RS::Annotated:
                            for (auto& [canon, k] : classes) {
                                if (k->findAnnotation(site.selector)) {
                                    addKeep(canon,
                                        "classesAnnotated(@" + site.selector + ")");
                                }
                            }
                            break;
                    }
                }
                for (auto& module : modules) {
                    module->setKeepSet(keep);
                }
                if (!flags.whyKept.empty()) {
                    auto it = keptBy.find(flags.whyKept);
                    if (it != keptBy.end())
                        std::cout << "why-kept: " << flags.whyKept
                                  << " — kept by " << it->second << "\n";
                    else
                        std::cout << "why-kept: " << flags.whyKept
                                  << " — STRIPPED (no reflection site keeps it)\n";
                }
                if (!flags.keepsetJson.empty()) {
                    writeKeepsetJson(flags.keepsetJson, keptBy, /*forcesAll=*/false);
                }
            } else {
                if (!flags.whyKept.empty())
                    std::cout << "why-kept: " << flags.whyKept
                              << " — kept: this build uses forces-ALL reflection"
                                 " (whole registry retained)\n";
                if (!flags.keepsetJson.empty())
                    writeKeepsetJson(flags.keepsetJson, {}, /*forcesAll=*/true);
            }
        }

        // REFL-2 — emit the reflective adapter bodies now that Phase 1/2 has
        // quiesced and every method's LLVM function exists. Each class that had
        // an invoke adapter forward-declared into its #Rtti (during prototype
        // generation) gets its switch-over-index body filled here; classes
        // without a decl are a no-op. Runs once, before clinit/emit.
        for (auto& [key, type] : CajetaType::getCanonicalMap()) {
            if (auto klass = std::dynamic_pointer_cast<CajetaClass>(type)) {
                klass->emitReflectInvokeBody();
                klass->emitReflectNewBody();
                // REFL-8/10: patch + register #ClassObjects deferred at populate
                // (classes parsed before cajeta.reflect.Class) now that Class is
                // built — makes them forName/allClasses-discoverable.
                klass->finalizeClassObject();
            }
        }

        // P6.2 — after Phase 1/2 quiescence, emit any per-class clinit
        // for static fields whose initializers didn't constant-fold.
        // Runs once at this point (not inside the loop) so the
        // expression codegen sees the final method set, and so we
        // don't pay the cost on each Phase 2 iteration. Registered
        // with llvm.global_ctors so the JIT (and AOT) module-load
        // step executes it before any user code.
        for (auto& module: modules) {
            for (auto& [name, klass] : module->getStructures()) {
                if (klass) klass->generateStaticInitializers();
            }
        }
        if (linkClasspathDeps) {
            for (auto& module: externalModules) {
                for (auto& [name, klass] : module->getStructures()) {
                    if (klass) klass->generateStaticInitializers();
                }
            }
        }
        // XPU device codegen (--xpu-backend=nvptx): embed each @Kernel's cubin +
        // registration ctor into its host module, before the host module is
        // written out below. No-op for the default host-only path.
        emitXpuKernels(archiveRootPath);

        // Binary emit needs a C-ABI `main` to satisfy the loader. Synthesize
        // a shim that bridges crt0's `main(argc, argv)` to the user's static
        // entry: it installs `-Dkey=value` props, marshals argv into the
        // cajeta `String[]` the entry expects, calls it, and returns its int32.
        // Skipped for IR emit; the JIT and IR-archive consumers invoke the
        // entry by mangled name directly. Uber archives are the "runnable,
        // self-contained deployment artifact" form, so they carry the shim too
        // — linking an uber's bitcode into a binary then needs no external
        // entry. (Plain `cja` archives are classpath libraries — no shim, so a
        // consumer's own `main` can't collide.)
        if ((emitMode == EmitMode::Obj || emitMode == EmitMode::Exe
                || emitMode == EmitMode::Uber)
                && !entryMethod.empty()) {
            emitCMainShim(entryMethod);
        }

        // Tier-1 RTA — after the main shim + all ctors exist, before per-module
        // emit. Report (Phase A) only prints; On (Phase B) prunes unreachable
        // cajeta method bodies so their native deps are never linked.
        if (emitMode == EmitMode::Exe) {
            if (flags.treeShake == TreeShake::Report) {
                reportTreeShake();
            } else if (flags.treeShake == TreeShake::On) {
                pruneDeadClinits();   // before the method prune so dead clinit
                pruneUnreachable();   // callees cascade into it
            }
        }

        // Archive emit bundles every parsed module's bitcode into one
        // `.cja` file. The exploded per-module loop below is skipped —
        // a single artifact is the whole point.
        if (emitMode == EmitMode::Cja || emitMode == EmitMode::Uber) {
            emitArchive(archiveRootPath, emitMode == EmitMode::Uber);
        } else {
            for (auto& module: modules) {
                // Runtime is linked once into the stdlib module (see
                // parseStdlibInto); user modules carry only extern decls
                // for runtime helpers, resolved by the JIT/AOT link step.
                emitForModule(module);
                // Incremental compilation (Phase 2): emit the per-module
                // instantiation-obligation sidecar alongside its IR.
                module->writeObligationsSidecar();
            }
            // Classpath deps for binary emit: emit each (now body-generated)
            // external module's object so its symbols are defined at link. The
            // external ctor set archivePath from the canonical name but no
            // archiveRoot; point it at the build root so the .o lands there.
            if (linkClasspathDeps) {
                for (auto& module: externalModules) {
                    module->setArchiveRoot(archiveRootPath);
                    emitForModule(module);
                }
            }
        }

        if (emitMode == EmitMode::Exe) {
            linkExecutable(archiveRootPath);
        }

        delete modulePaths;
    }

    // Per-module emission driven by --emit. IR (default) writes the .ll file the
    // archive path already names. Obj uses TargetMachine's legacy passes to write a
    // native ELF/Mach-O/etc. object. Exe defers linking until after every module's
    // object has been written; see linkExecutable.
    void Compiler::emitForModule(CajetaModulePtr module) {
        // Attach TBAA !tbaa tags to array-element / object-field loads & stores
        // (recorded during codegen) before any optimization runs — so LICM/GVN
        // can hoist field loads across array-element stores. Runs for every emit
        // mode (IR mode writes the .ll so the tags are inspectable).
        module->applyTbaaTags();
        switch (emitMode) {
            case EmitMode::IR:
                module->writeIRFileTarget();
                return;
            case EmitMode::Obj:
            case EmitMode::Exe: {
                if (!targetMachine) {
                    cerr << "cajeta: no TargetMachine available for object emission" << std::endl;
                    return;
                }
                string objPath = module->getArchiveRoot() + module->getArchivePath();
                // Swap the .ll suffix for .o (the archivePath was computed for IR output).
                if (objPath.size() >= 3 && objPath.substr(objPath.size() - 3) == ".ll") {
                    objPath.replace(objPath.size() - 3, 3, ".o");
                }
                std::filesystem::create_directories(
                    std::filesystem::path(objPath).parent_path());

                std::error_code ec;
                llvm::raw_fd_ostream dest(objPath, ec, llvm::sys::fs::OF_None);
                if (ec) {
                    cerr << "cajeta: could not open " << objPath
                         << " for writing: " << ec.message() << std::endl;
                    return;
                }

                // ThinLTO (--lto=thin, --emit=exe): emit ThinLTO bitcode + module
                // summary instead of a native object, so the link-time backend can
                // import + inline ACROSS module boundaries (e.g. an @Inline
                // `ArrayList<int32>::add` — emitted into the stdlib module — folds
                // into a user append loop in another module). Without this, each
                // module is a closed native object and `alwaysinline`/`@Inline`
                // can't cross the boundary. linkExecutable drives the fork's
                // version-matched lld over these. (Obj emit stays native — there's
                // no link step there to run the ThinLTO backend.)
                if (flags.lto == LtoMode::Thin && emitMode == EmitMode::Exe) {
                    llvm::Module& M = *module->getLlvmModule();
                    // Stamp the host CPU + feature set as per-function attributes.
                    // The ld.lld ThinLTO backend builds its OWN TargetMachine and
                    // reads `target-cpu`/`target-features` from each function (the
                    // way clang emits them) — it does NOT see this compiler's
                    // TargetMachine. Without the attributes the backend codegens
                    // the generic x86-64 baseline (SSE2), so the SIMD hot loops
                    // (e.g. XXHash3.hashLong) lose AVX-512/AVX2/FMA and run ~3x
                    // slower than the non-LTO build. (The non-LTO path is fine —
                    // it codegens directly through the native TargetMachine.)
                    {
                        llvm::StringRef tcpu = targetMachine->getTargetCPU();
                        std::string tfeat = targetMachine->getTargetFeatureString().str();
                        for (llvm::Function& f : M) {
                            if (f.isDeclaration()) continue;
                            if (!tcpu.empty() && !f.hasFnAttribute("target-cpu"))
                                f.addFnAttr("target-cpu", tcpu);
                            if (!tfeat.empty() && !f.hasFnAttribute("target-features"))
                                f.addFnAttr("target-features", tfeat);
                        }
                    }
                    optimizeModuleThinLTOPreLink(M, targetMachine, flags.opt);
                    llvm::ProfileSummaryInfo psi(M);
                    llvm::ModuleSummaryIndex index =
                        llvm::buildModuleSummaryIndex(M, /*GetBFICallback=*/nullptr, &psi);
                    llvm::WriteBitcodeToFile(M, dest,
                        /*ShouldPreserveUseListOrder=*/false, &index,
                        /*GenerateHash=*/true);
                    dest.flush();
                    objectFiles.push_back(objPath);
                    return;
                }

                // IR optimization (--opt). No-op at O0 (the historical default);
                // O2/O3 run the full per-module pipeline (incl. LoopVectorize +
                // SLP) over this user module before codegen.
                optimizeModule(*module->getLlvmModule(), targetMachine, flags.opt);

                llvm::legacy::PassManager pm;
                auto fileType = llvm::CodeGenFileType::ObjectFile;
                if (targetMachine->addPassesToEmitFile(pm, dest, nullptr, fileType)) {
                    cerr << "cajeta: target machine cannot emit object files for "
                         << targetTriple << std::endl;
                    return;
                }
                pm.run(*module->getLlvmModule());
                dest.flush();
                objectFiles.push_back(objPath);
                return;
            }
        }
    }

    // XPU device codegen for the AOT path. Mirrors the JIT helper's kernel
    // registration (test/jit/JitTestHelper.cpp): for each parsed module, embed
    // each @Kernel's cubin + a registration ctor into that module's own host
    // LLVM module — correct for AOT since every module's object links together.
    // When --xpu-emit is ptx/cubin, also drop a standalone per-kernel artifact
    // for inspection. Never throws (NvptxRegistration swallows XPU-N01 / absent
    // ptxas; the artifact path catches the same and skips with a diagnostic).
    void Compiler::emitXpuKernels(const std::string& archiveRootPath) {
        if (xpuBackends.empty()) {
            return;
        }
        // Map a compiler-level backend onto the xpu-layer dispatch enum (the
        // xpu/ code does not depend on compile/) and supply its default arch.
        // With multiple backends a single --xpu-arch can't serve all, so each
        // uses its own default; a lone backend still honors --xpu-arch.
        const bool singleBackend = xpuBackends.size() == 1;
        auto toLayer = [](XpuBackend cb) -> cajeta::xpu::Backend {
            switch (cb) {
                case XpuBackend::Amdgpu: return cajeta::xpu::Backend::Amdgpu;
                case XpuBackend::Vulkan: return cajeta::xpu::Backend::Spirv;
                case XpuBackend::Cpu:    return cajeta::xpu::Backend::Cpu;
                default:                 return cajeta::xpu::Backend::Nvptx;
            }
        };
        auto defaultArch = [](XpuBackend cb) -> std::string {
            switch (cb) {
                case XpuBackend::Amdgpu: return "gfx1151";
                case XpuBackend::Vulkan: return "vulkan1.3";
                case XpuBackend::Cpu:    return "";
                default:                 return "sm_89";
            }
        };
        for (auto& module : modules) {
            std::vector<MethodPtr> kernels;
            std::vector<MethodPtr> shaders;
            for (auto& method : module->getAllMethods()) {
                if (!method) continue;
                if (cajeta::xpu::isKernel(*method)) {
                    kernels.push_back(method);
                } else if (cajeta::xpu::isGraphicsShader(*method)) {
                    // @Vertex/@Fragment/… — the rasterization parallel of a
                    // @Kernel; registered alongside kernels (Vulkan-only).
                    shaders.push_back(method);
                }
            }
            if (kernels.empty() && shaders.empty()) {
                continue;
            }
            // Per-kernel artifact base: the module's IR output path
            // (archiveRoot + archivePath, ending .ll) minus the extension.
            std::string base = module->getArchiveRoot() + module->getArchivePath();
            if (base.size() >= 3 && base.substr(base.size() - 3) == ".ll") {
                base.resize(base.size() - 3);
            }

            // The bundled-backend manifest the runtime dispatcher reads: one
            // ctor per selected backend calling __cajeta_xpu_register_backend.
            // (cajeta-cpu.md Increment 4 — explicit-only bundling.)
            std::vector<cajeta::xpu::Backend> layerBackends;
            for (XpuBackend cb : xpuBackends) layerBackends.push_back(toLayer(cb));
            cajeta::xpu::emitBackendManifest(layerBackends,
                                             *module->getLlvmModule());

            // Each selected backend embeds its registration into this module's
            // host module; with --xpu-emit it also drops an inspection artifact.
            for (XpuBackend cb : xpuBackends) {
                cajeta::xpu::Backend backend = toLayer(cb);
                std::string arch = singleBackend ? xpuArch : defaultArch(cb);
                cajeta::xpu::emitKernelRegistration(
                    backend, kernels, *module->getLlvmModule(), arch);
                // Graphics shaders register alongside kernels — a no-op for the
                // non-Vulkan backends (no raster pipeline). Rides the same build.
                cajeta::xpu::emitGraphicsRegistration(
                    backend, shaders, *module->getLlvmModule(), arch);

                if (xpuEmit == XpuEmit::None) {
                    continue;
                }
                std::filesystem::create_directories(
                    std::filesystem::path(base).parent_path());

                for (auto& kernel : kernels) {
                try {
                    std::string stem = base + "." + kernel->getName();
                    if (backend == cajeta::xpu::Backend::Nvptx) {
                        if (xpuEmit != XpuEmit::Ptx && xpuEmit != XpuEmit::Cubin) {
                            cerr << "cajeta: XPU: --xpu-emit=isa/hsaco is amdgpu-"
                                    "only; ignoring for nvptx" << std::endl;
                            continue;
                        }
                        auto tm = cajeta::xpu::nvidia::createNvptxTargetMachine(arch);
                        if (!tm) {
                            cerr << "cajeta: XPU: nvptx target unavailable in this "
                                    "LLVM build; skipping " << kernel->getName() << std::endl;
                            continue;
                        }
                        llvm::LLVMContext deviceCtx;
                        llvm::Module deviceModule("xpu_device", deviceCtx);
                        cajeta::xpu::nvidia::configureDeviceModule(deviceModule, *tm);
                        cajeta::xpu::nvidia::lowerKernel(kernel, deviceModule);
                        std::string ptx = cajeta::xpu::nvidia::emitPtx(deviceModule, *tm);
                        if (ptx.empty()) {
                            cerr << "cajeta: XPU: PTX emission produced nothing for "
                                 << kernel->getName() << std::endl;
                            continue;
                        }
                        if (xpuEmit == XpuEmit::Ptx) {
                            std::ofstream out(stem + ".ptx", std::ios::binary);
                            out << ptx;
                        } else {  // XpuEmit::Cubin
                            std::vector<uint8_t> cubin =
                                cajeta::xpu::nvidia::assembleCubin(ptx, arch);
                            if (cubin.empty()) {
                                cerr << "cajeta: XPU: ptxas unavailable or failed; no "
                                        "cubin for " << kernel->getName() << std::endl;
                                continue;
                            }
                            std::ofstream out(stem + ".cubin", std::ios::binary);
                            out.write(reinterpret_cast<const char*>(cubin.data()),
                                      (std::streamsize) cubin.size());
                        }
                    } else if (backend == cajeta::xpu::Backend::Amdgpu) {
                        if (xpuEmit != XpuEmit::Isa && xpuEmit != XpuEmit::Hsaco) {
                            cerr << "cajeta: XPU: --xpu-emit=ptx/cubin is nvptx-"
                                    "only; ignoring for amdgpu" << std::endl;
                            continue;
                        }
                        // `arch` may be a comma-separated list → a multi-arch
                        // bundle for --xpu-emit=hsaco; the config TM uses the first.
                        std::vector<std::string> archList =
                            cajeta::xpu::amd::splitArchList(arch);
                        if (archList.empty()) continue;
                        auto tm = cajeta::xpu::amd::createAmdgpuTargetMachine(
                            archList[0]);
                        if (!tm) {
                            cerr << "cajeta: XPU: amdgcn target unavailable in this "
                                    "LLVM build; skipping " << kernel->getName() << std::endl;
                            continue;
                        }
                        llvm::LLVMContext deviceCtx;
                        llvm::Module deviceModule("xpu_device", deviceCtx);
                        cajeta::xpu::amd::configureDeviceModule(deviceModule, *tm);
                        cajeta::xpu::amd::lowerKernel(kernel, deviceModule);
                        if (xpuEmit == XpuEmit::Isa) {
                            std::string isa =
                                cajeta::xpu::amd::emitIsa(deviceModule, *tm);
                            if (isa.empty()) {
                                cerr << "cajeta: XPU: ISA emission produced nothing for "
                                     << kernel->getName() << std::endl;
                                continue;
                            }
                            std::ofstream out(stem + ".isa", std::ios::binary);
                            out << isa;
                        } else {  // XpuEmit::Hsaco
                            std::vector<uint8_t> hsaco =
                                cajeta::xpu::amd::assembleHsacoBundle(deviceModule,
                                                                     archList);
                            if (hsaco.empty()) {
                                cerr << "cajeta: XPU: ld.lld unavailable or failed; no "
                                        "hsaco for " << kernel->getName() << std::endl;
                                continue;
                            }
                            std::ofstream out(stem + ".hsaco", std::ios::binary);
                            out.write(reinterpret_cast<const char*>(hsaco.data()),
                                      (std::streamsize) hsaco.size());
                        }
                    } else if (backend == cajeta::xpu::Backend::Spirv) {
                        if (xpuEmit != XpuEmit::Spirv && xpuEmit != XpuEmit::Spvasm) {
                            cerr << "cajeta: XPU: --xpu-emit=ptx/cubin/isa/hsaco is "
                                    "not vulkan; use spirv/spvasm" << std::endl;
                            continue;
                        }
                        auto tm = cajeta::xpu::vulkan::createSpirvTargetMachine(arch);
                        if (!tm) {
                            cerr << "cajeta: XPU: spirv target unavailable in this "
                                    "LLVM build; skipping " << kernel->getName() << std::endl;
                            continue;
                        }
                        llvm::LLVMContext deviceCtx;
                        llvm::Module deviceModule("xpu_device", deviceCtx);
                        cajeta::xpu::vulkan::configureDeviceModule(deviceModule, *tm);
                        cajeta::xpu::vulkan::lowerKernel(kernel, deviceModule);
                        if (xpuEmit == XpuEmit::Spvasm) {
                            std::string text =
                                cajeta::xpu::vulkan::emitSpirvText(deviceModule, *tm);
                            if (text.empty()) {
                                cerr << "cajeta: XPU: SPIR-V text emission produced "
                                        "nothing for " << kernel->getName() << std::endl;
                                continue;
                            }
                            std::ofstream out(stem + ".spvasm", std::ios::binary);
                            out << text;
                        } else {  // XpuEmit::Spirv
                            std::vector<uint8_t> spirv =
                                cajeta::xpu::vulkan::emitSpirv(deviceModule, *tm);
                            if (spirv.empty()) {
                                cerr << "cajeta: XPU: SPIR-V emission produced nothing "
                                        "for " << kernel->getName() << std::endl;
                                continue;
                            }
                            std::ofstream out(stem + ".spv", std::ios::binary);
                            out.write(reinterpret_cast<const char*>(spirv.data()),
                                      (std::streamsize) spirv.size());
                        }
                    } else {  // cajeta::xpu::Backend::Cpu
                        if (xpuEmit != XpuEmit::Object) {
                            cerr << "cajeta: XPU: --xpu-emit for cpu is obj only; "
                                    "ignoring" << std::endl;
                            continue;
                        }
                        auto tm = cajeta::xpu::cpu::createCpuTargetMachine();
                        if (!tm) {
                            cerr << "cajeta: XPU: host target unavailable; skipping "
                                 << kernel->getName() << std::endl;
                            continue;
                        }
                        llvm::LLVMContext deviceCtx;
                        llvm::Module deviceModule("xpu_cpu", deviceCtx);
                        cajeta::xpu::cpu::configureHostModule(deviceModule, *tm);
                        cajeta::xpu::cpu::lowerKernel(kernel, deviceModule);
                        std::vector<uint8_t> obj =
                            cajeta::xpu::cpu::emitObject(deviceModule, *tm);
                        if (obj.empty()) {
                            cerr << "cajeta: XPU: object emission produced nothing "
                                    "for " << kernel->getName() << std::endl;
                            continue;
                        }
                        std::ofstream out(stem + ".o", std::ios::binary);
                        out.write(reinterpret_cast<const char*>(obj.data()),
                                  (std::streamsize) obj.size());
                    }
                } catch (cajeta::Exception& e) {
                    // XPU-N01 (unsupported construct) or similar — skip, don't abort.
                    cerr << "cajeta: XPU: skipping kernel " << kernel->getName()
                         << " (" << e.getErrorId() << "): " << e.getMessage()
                         << std::endl;
                }
                }
            }
        }
    }

    // For --emit=exe: link the per-module .o files into a single executable. Uses lld
    // when it was found at CMake-configure time (see CAJETA_HAS_LLD in CMakeLists.txt);
    // otherwise emits a clear diagnostic so the user can link with their toolchain.
    void Compiler::linkExecutable(const string& archiveRootPath) {
        string outPath = outputPath.empty()
            ? (archiveRootPath + "a.out")
            : outputPath;

        // Link through the system C compiler/driver rather than calling a raw
        // linker: the driver locates the platform's CRT, startup objects, libc,
        // and library search paths, and selects the right object format
        // (ELF / COFF-mingw / Mach-O) for the host — far more robust and
        // portable than reconstructing a per-OS link line. (Linking inherently
        // needs the platform CRT/libc, so a system toolchain is required
        // regardless.) Honor $CC, then fall back to the usual driver names.
        std::vector<std::string> drivers;
        if (const char* envCc = std::getenv("CC")) {
            if (*envCc) drivers.emplace_back(envCc);
        }
        drivers.emplace_back("cc");
        drivers.emplace_back("clang");
        drivers.emplace_back("gcc");

        // Prefer LLD for the final link when it's available. GNU ld's section
        // GC is conservative — on COFF it keeps unreferenced external COMDAT
        // sections, and across platforms it dead-strips less than lld — so
        // preferring lld measurably shrinks `--emit=exe` output (a HelloWorld
        // drops ~30% on Windows). We pass `-fuse-ld=lld` to the C driver rather
        // than calling lld directly so the driver still supplies the CRT/libc
        // and lld picks the right flavor (lld-link/MinGW for PE, ld.lld for
        // ELF, ld64.lld for Mach-O). Gated on lld actually being locatable so
        // we degrade to the platform default linker instead of failing when
        // lld is absent.
        bool haveLld = (bool) llvm::sys::findProgramByName("ld.lld")
                    || (bool) llvm::sys::findProgramByName("lld");
#if defined(_WIN32)
        if (!haveLld) haveLld = (bool) llvm::sys::findProgramByName("lld-link");
#endif

        // Materialize the embedded TLS native object beside the output so it can
        // be added to the link. The produced exe references `__cajeta_tls_*` from
        // the always-linked stdlib TlsConnection thunks, but those natives live in
        // a standalone object kept out of the embedded JIT bitcode (see
        // EmbeddedTls.h / src/CMakeLists.txt). The build machine has a C toolchain
        // but not cajeta's runtime source, so the bytes ride along in the compiler
        // binary. All platforms — on Linux/macOS the undefined `__cajeta_tls_*`
        // symbols otherwise break ANY exe, not just TLS-using programs.
        std::string tlsObjPath = archiveRootPath + "__cajeta_tls.o";
        {
            std::ofstream tlsOut(tlsObjPath, std::ios::binary);
            tlsOut.write(reinterpret_cast<const char*>(cajeta_tls_o),
                         (std::streamsize) cajeta_tls_o_len);
        }

        // OptiX AS stubs for the AOT link. The runtime (cajeta_runtime.c)
        // references `cajeta_xpu_optix_*`; the JIT resolves those to the real
        // impl (OptixAccel.cpp) via the process-symbol generator, but an
        // `--emit=exe` executable has no such generator, so they'd be undefined
        // at link on a build without the OptiX SDK / a non-NVIDIA host. Mirrors
        // the TLS object: a tiny generated translation unit linked into every
        // exe (the link's own `cc` compiles the `.c`). Weak, so a future
        // GPU-enabled AOT link can override them; here `available()` returns 0
        // and the runtime falls back to the software-BVH path (hardware-gated).
        std::string optixStubPath = archiveRootPath + "__cajeta_xpu_optix_stub.c";
        {
            std::ofstream s(optixStubPath, std::ios::binary);
            s << "#include <stdint.h>\n"
                 "#define W __attribute__((weak))\n"
                 "W int      cajeta_xpu_optix_available(void){return 0;}\n"
                 "W void*    cajeta_xpu_optix_context(void){return 0;}\n"
                 "W void*    cajeta_xpu_optix_cuda_context(void){return 0;}\n"
                 "W int64_t  cajeta_xpu_optix_accel_build_aabbs(const float*a,uint32_t b){(void)a;(void)b;return 0;}\n"
                 "W int64_t  cajeta_xpu_optix_accel_build_triangles(const float*a,uint32_t b,uint32_t c){(void)a;(void)b;(void)c;return 0;}\n"
                 "W uint64_t cajeta_xpu_optix_traversable(int64_t a){(void)a;return 0;}\n"
                 "W uint64_t cajeta_xpu_optix_accel_boxes(int64_t a){(void)a;return 0;}\n"
                 "W void     cajeta_xpu_optix_accel_free(int64_t a){(void)a;}\n"
                 "W int      cajeta_xpu_optix_launch(const char*a,uint64_t b,const char*c,const char*d,const char*e,const char*f,const void*g,uint64_t h,uint32_t i){(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;(void)i;return -1;}\n"
                 "W int      cajeta_xpu_optix_launch_tri(const char*a,uint64_t b,const char*c,const char*d,const char*e,const char*f,const void*g,uint64_t h,uint32_t i){(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;(void)i;return -1;}\n";
        }

        // Native-dependency link inputs (native-deps unit 7). DCE-aware: only
        // libs whose @Native symbol is still live after tree-shaking (scanned
        // across user + classpath-dep modules); linked as static archives so
        // --gc-sections + archive-member semantics still strip unused members.
        // No live native reqs → nativeArchives is empty → the link line is
        // unchanged for every program that uses no @Native lib.
        std::set<std::string> liveNativeLibs;
        auto scanLive = [&](const std::list<CajetaModulePtr>& mods) {
            for (auto& mod : mods) {
                if (!mod || !mod->getLlvmModule()) continue;
                auto libs = collectLiveNativeLibs(*mod->getLlvmModule());
                liveNativeLibs.insert(libs.begin(), libs.end());
            }
        };
        scanLive(modules);
        scanLive(externalModules);
        std::vector<std::string> nativeArchives;
        if (!liveNativeLibs.empty()) {
            auto resolved = resolveNativeArchivesForLink(
                liveNativeLibs, hostNativePlatform(), nativeLinkSearchDirs());
            if (!resolved) {
                cerr << "cajeta: --emit=exe: "
                     << llvm::toString(resolved.takeError()) << std::endl;
                return;
            }
            nativeArchives = std::move(*resolved);
        }

        // ThinLTO link: drive the C compiler (for CRT/libc) but force the
        // FORK's version-matched ld.lld, which can read this compiler's bitcode
        // summary and run the ThinLTO backend (cross-module import + inlining).
        // A system lld on PATH may be a different LLVM version and reject the
        // bitcode. We point the driver at the fork's tools dir with `-B<dir>` and
        // `-fuse-ld=lld` (gcc rejects an absolute `-fuse-ld=` path, but searches
        // `-B` prefixes for `ld.lld`; clang honors both). Fall back to PATH `lld`
        // with a warning so a non-ThinLTO use is unaffected.
        std::vector<std::string> thinLtoLinkArgs;  // empty unless lto=thin
        if (flags.lto == LtoMode::Thin) {
#ifdef CAJETA_LLVM_TOOLS_BIN
            std::string forkBin = CAJETA_LLVM_TOOLS_BIN;
            if (std::filesystem::exists(forkBin + "/ld.lld")) {
                thinLtoLinkArgs.push_back("-B" + forkBin);
                thinLtoLinkArgs.push_back("-fuse-ld=lld");
            }
#endif
            if (thinLtoLinkArgs.empty()) {
                cerr << "cajeta: --lto=thin: version-matched fork ld.lld not found; "
                        "falling back to PATH lld (may reject the bitcode if its "
                        "LLVM version differs)."
                     << std::endl;
                if (haveLld) thinLtoLinkArgs.push_back("-fuse-ld=lld");
            }
            // Drive the ThinLTO backend at O3 to match the non-LTO O3 pipeline
            // (buildPerModuleDefaultPipeline(O3)). lld's default LTO opt level is
            // O2, which vectorizes the SIMD hot loops (e.g. XXHash3.hashLong)
            // far less aggressively — a ~3x throughput regression vs the non-LTO
            // build. The per-function target-cpu/target-features (cpu=native)
            // propagate into the backend, so AVX-512/FMA stay available; only
            // the backend opt level needed lifting.
            if (!thinLtoLinkArgs.empty()) {
                thinLtoLinkArgs.push_back("-Wl,--lto-O3");
            }
        }

        buildtool::SubprocessResult res;
        bool launched = false;
        std::string usedDriver;
        for (const auto& drv : drivers) {
            buildtool::SubprocessOptions opt;
            opt.argv.push_back(drv);
            if (!thinLtoLinkArgs.empty()) {
                for (const auto& a : thinLtoLinkArgs) opt.argv.push_back(a);
            } else if (haveLld) {
                opt.argv.push_back("-fuse-ld=lld");
            }
            for (const auto& obj : objectFiles) opt.argv.push_back(obj);
            // The standalone TLS native object (resolves `__cajeta_tls_*`,
            // always referenced by the stdlib). Linked on every platform; the
            // matching OpenSSL libs are added per-OS below.
            opt.argv.push_back(tlsObjPath);
            // OptiX AS stubs (resolves `cajeta_xpu_optix_*`, referenced by the
            // runtime's CUDA ray-query provider). `--gc-sections` drops them
            // when the entry never reaches GPU AS code.
            opt.argv.push_back(optixStubPath);
            // Native-dep archives AFTER the objects that reference them (single-
            // pass linkers pull only the referenced members; --gc-sections drops
            // the rest). DCE-aware + gated above, so empty for non-@Native progs.
            for (const auto& a : nativeArchives) opt.argv.push_back(a);
            opt.argv.push_back("-o");
            opt.argv.push_back(outPath);
            // Dead-strip unreferenced sections. The codegen emits one section
            // per function/global (-ffunction-sections, see the comment near
            // the top of this file), so the linker can drop everything the
            // entry point doesn't transitively reach — including the stdlib's
            // OpenSSL-backed TLS natives (`__cajeta_tls_*`) when the program
            // never touches TLS, so they don't need to be resolved or have
            // -lssl/-lcrypto on the link line. (A program that *does* use TLS
            // still needs those libs; that wider link set is a follow-up.)
            // Matches what samples/*/build-bin.sh has always passed.
#if defined(__APPLE__)
            opt.argv.push_back("-Wl,-dead_strip");
#else
            opt.argv.push_back("-Wl,--gc-sections");
#endif
            // Platform libraries the cajeta runtime references (the driver adds
            // the CRT + libc itself).
#if defined(_WIN32)
            // Per-mode link policy: an `--emit=exe` is a deliverable, so it
            // STATICALLY links the mingw runtime (libgcc / libstdc++ / winpthread)
            // and OpenSSL — the produced binary then depends only on Windows
            // SYSTEM DLLs (kernel32, ws2_32, crypt32, bcrypt, advapi32, user32,
            // msvcrt), all present on every install, and runs with no extra
            // install or PATH setup. (cja / uber artifacts deliberately stay
            // dynamic: they execute inside cajeta's JIT host, which already
            // provides the runtime + these libs in-process — see the JIT path.)
            opt.argv.push_back("-static");    // libgcc / libstdc++ / winpthread
            opt.argv.push_back("-lssl");      // TLS engine (__cajeta_tls_*, cajeta_tls.o)
            opt.argv.push_back("-lcrypto");
            opt.argv.push_back("-lws2_32");   // Winsock (cajeta.io.net natives in the bitcode)
            opt.argv.push_back("-lcrypt32");  // OS trust-store shim (cajeta_tls.c)
            opt.argv.push_back("-lbcrypt");   // BCryptGenRandom (runtime RNG)
            opt.argv.push_back("-ladvapi32"); // OpenSSL CryptoAPI dependencies
            opt.argv.push_back("-luser32");
            opt.argv.push_back("-lpthread");  // winpthreads (resolved static via -static)
#elif defined(__APPLE__)
            // TLS natives (cajeta_tls.o) → OpenSSL. cajeta_tls.c uses only
            // portable OpenSSL on non-Windows (SSL_CTX_set_default_verify_paths),
            // so no Security.framework is needed.
            opt.argv.push_back("-lssl");
            opt.argv.push_back("-lcrypto");
            opt.argv.push_back("-lpthread");
#else
            // TLS natives (cajeta_tls.o) → OpenSSL, then the base runtime libs.
            opt.argv.push_back("-lssl");
            opt.argv.push_back("-lcrypto");
            opt.argv.push_back("-lpthread");
            opt.argv.push_back("-lm");
            opt.argv.push_back("-ldl");
#endif
            res = buildtool::runSubprocess(opt);
            if (res.launched) { launched = true; usedDriver = drv; break; }
        }

        if (!launched) {
            cerr << "cajeta: --emit=exe could not find a C compiler to link "
                 << "with (tried $CC, cc, clang, gcc). Set $CC to your "
                 << "toolchain's driver." << std::endl;
            return;
        }
        if (res.code() != 0) {
            cerr << "cajeta: link failed — '" << usedDriver << "' exited "
                 << res.code() << std::endl;
        }
    }

    void Compiler::emitCMainShim(const std::string& entryMethod) {
        // entryMethod arrives as `pkg.subpkg.Class.method` (dotted). Split
        // on the last '.' to separate class canonical from method name.
        // No method name → no shim.
        // Accept the canonical `package.Class::method` form (what the build
        // tool emits) as well as the legacy all-dotted `package.Class.method`.
        std::string classCanonical;
        std::string methodName;
        auto sep = entryMethod.find("::");
        if (sep != std::string::npos) {
            classCanonical = entryMethod.substr(0, sep);
            methodName     = entryMethod.substr(sep + 2);
        } else {
            auto lastDot = entryMethod.rfind('.');
            if (lastDot == std::string::npos || lastDot + 1 >= entryMethod.size()) {
                cerr << "cajeta: --emit=" << (emitMode == EmitMode::Exe ? "exe" : "obj")
                     << " entry method `" << entryMethod
                     << "` must be in `package.Class::method` form" << std::endl;
                return;
            }
            classCanonical = entryMethod.substr(0, lastDot);
            methodName     = entryMethod.substr(lastDot + 1);
        }
        if (classCanonical.empty() || methodName.empty()) {
            cerr << "cajeta: --emit=" << (emitMode == EmitMode::Exe ? "exe" : "obj")
                 << " entry method `" << entryMethod
                 << "` must be in `package.Class::method` form" << std::endl;
            return;
        }

        // Walk every user module looking for the matching class + method.
        // Static; int32 or void return. Two accepted shapes: a no-arg
        // `main()`, or `main(String[] args)` which receives the command-line
        // arguments (materialized from C argv by the shim below).
        MethodPtr entry;
        bool entryTakesArgs = false;
        for (auto& m : modules) {
            auto it = m->getStructures().find(classCanonical);
            if (it == m->getStructures().end() || !it->second) continue;
            for (auto& mEntry : it->second->getMethods()) {
                auto& candidate = mEntry.second;
                if (!candidate || candidate->getName() != methodName) continue;
                if (candidate->isMethodTemplate()) continue;
                auto& mods = candidate->getModifiers();
                if (mods.find(STATIC) == mods.end()) continue;
                // Static method's parameterList has no `this` prepended.
                const auto& params = candidate->getParameterList();
                if (params.empty()) {
                    entry = candidate;
                    entryTakesArgs = false;
                    break;
                }
                // Accept exactly one `String[]` parameter (the args vector).
                if (params.size() == 1 && params[0] && params[0]->getType()) {
                    auto arr = std::dynamic_pointer_cast<CajetaArray>(
                        params[0]->getType());
                    if (arr && arr->getElementType() &&
                        arr->getElementType()->getQName() &&
                        arr->getElementType()->getQName()->getTypeName()
                            == "String" &&
                        arr->getElementType()->getQName()->getPackageName()
                            == "cajeta.lang") {
                        entry = candidate;
                        entryTakesArgs = true;
                        break;
                    }
                }
            }
            if (entry) break;
        }
        if (!entry || !entry->getLlvmFunction()) {
            cerr << "cajeta: --emit=" << (emitMode == EmitMode::Exe ? "exe" : "obj")
                 << " could not find static no-arg method `" << entryMethod
                 << "` to use as the program entry point" << std::endl;
            return;
        }

        // Synthesize `int main(void)` in the stdlib module (always linked).
        auto stdlib = CajetaModule::getStdlibModule();
        if (!stdlib) return;
        llvm::LLVMContext& ctx = *stdlib->getLlvmContext();
        llvm::Module* lmod = stdlib->getLlvmModule();
        llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);

        // External decl for the cajeta-mangled entry symbol in this module.
        llvm::Function* entryFn = entry->getLlvmFunction();
        llvm::Function* entryExtern = llvm::Function::Create(
            entryFn->getFunctionType(),
            llvm::GlobalValue::ExternalLinkage,
            entryFn->getName(),
            lmod);

        // `int main(int argc, char** argv)` — argv lets us honor Java-style
        // `-Dkey=value` startup args by installing each as a system property
        // before user code runs. Residual argv (without the -D entries) is
        // currently dropped because the user entry doesn't take parameters
        // yet; once `static int32 main(String[] args)` lands we'll forward
        // the residual through.
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
        llvm::FunctionType* mainTy = llvm::FunctionType::get(
            i32Ty, {i32Ty, ptrTy}, false);
        llvm::Function* mainFn = llvm::Function::Create(
            mainTy, llvm::GlobalValue::ExternalLinkage, "main", lmod);
        llvm::BasicBlock* bb = llvm::BasicBlock::Create(ctx, "entry", mainFn);
        llvm::IRBuilder<> b(bb);

        // Walk argv looking for `-Dkey=value` (or `-Dkey`) tokens and feed
        // each to __cajeta_property_install. Other args are skipped for
        // now (no String[]-typed entry yet).
        {
            llvm::Value* argcVal = mainFn->getArg(0);
            llvm::Value* argvVal = mainFn->getArg(1);

            // Declare strncmp + the installer.
            llvm::FunctionType* strncmpTy = llvm::FunctionType::get(
                i32Ty, {ptrTy, ptrTy, llvm::Type::getInt64Ty(ctx)}, false);
            llvm::FunctionCallee strncmpFn =
                lmod->getOrInsertFunction("strncmp", strncmpTy);
            llvm::FunctionType* installerTy = llvm::FunctionType::get(
                llvm::Type::getVoidTy(ctx), {ptrTy}, false);
            llvm::FunctionCallee installerFn =
                lmod->getOrInsertFunction(
                    "__cajeta_property_install", installerTy);

            // const char* prefix = "-D";
            llvm::Constant* dashD = b.CreateGlobalString("-D",
                ".cajeta.dashD", /*AddressSpace=*/0, lmod);

            llvm::BasicBlock* loopHead = llvm::BasicBlock::Create(
                ctx, "argv.head", mainFn);
            llvm::BasicBlock* loopBody = llvm::BasicBlock::Create(
                ctx, "argv.body", mainFn);
            llvm::BasicBlock* checkD   = llvm::BasicBlock::Create(
                ctx, "argv.checkD", mainFn);
            llvm::BasicBlock* installD = llvm::BasicBlock::Create(
                ctx, "argv.installD", mainFn);
            llvm::BasicBlock* loopStep = llvm::BasicBlock::Create(
                ctx, "argv.step", mainFn);
            llvm::BasicBlock* afterLoop = llvm::BasicBlock::Create(
                ctx, "argv.done", mainFn);

            llvm::AllocaInst* iSlot = b.CreateAlloca(i32Ty, nullptr, "argv.i");
            b.CreateStore(llvm::ConstantInt::get(i32Ty, 1), iSlot);   // skip argv[0]
            b.CreateBr(loopHead);

            b.SetInsertPoint(loopHead);
            llvm::Value* iCur = b.CreateLoad(i32Ty, iSlot);
            llvm::Value* cond = b.CreateICmpSLT(iCur, argcVal);
            b.CreateCondBr(cond, loopBody, afterLoop);

            b.SetInsertPoint(loopBody);
            llvm::Value* iCur2 = b.CreateLoad(i32Ty, iSlot);
            llvm::Value* slotPtr = b.CreateGEP(ptrTy, argvVal, iCur2);
            llvm::Value* tokenPtr = b.CreateLoad(ptrTy, slotPtr);
            b.CreateBr(checkD);

            b.SetInsertPoint(checkD);
            // strncmp(token, "-D", 2) == 0 → install
            llvm::Value* cmpResult = b.CreateCall(strncmpFn,
                {tokenPtr, dashD, llvm::ConstantInt::get(
                    llvm::Type::getInt64Ty(ctx), 2)});
            llvm::Value* isDash = b.CreateICmpEQ(cmpResult,
                llvm::ConstantInt::get(i32Ty, 0));
            b.CreateCondBr(isDash, installD, loopStep);

            b.SetInsertPoint(installD);
            // installer takes the substring past "-D".
            llvm::Value* afterDashD = b.CreateGEP(
                llvm::Type::getInt8Ty(ctx), tokenPtr,
                llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx), 2));
            b.CreateCall(installerFn, {afterDashD});
            b.CreateBr(loopStep);

            b.SetInsertPoint(loopStep);
            llvm::Value* iNext = b.CreateAdd(
                b.CreateLoad(i32Ty, iSlot),
                llvm::ConstantInt::get(i32Ty, 1));
            b.CreateStore(iNext, iSlot);
            b.CreateBr(loopHead);

            b.SetInsertPoint(afterLoop);
        }

        // For `main(String[] args)`, materialize the cajeta String[] from
        // (argc, argv) and pass it. __cajeta_args_make takes the String class's
        // total size + field byte offsets (from DataLayout) and its vtable, so
        // the runtime writes each instance without any hardcoded String ABI.
        std::vector<llvm::Value*> callArgs;
        if (entryTakesArgs) {
            auto klass = std::dynamic_pointer_cast<CajetaClass>(
                CajetaType::of("String"));
            llvm::StructType* strStructTy =
                (klass && llvm::isa_and_nonnull<llvm::StructType>(
                              klass->getLlvmType()))
                    ? llvm::cast<llvm::StructType>(klass->getLlvmType())
                    : nullptr;
            if (!strStructTy) {
                cerr << "cajeta: --emit=exe entry `" << entryMethod
                     << "` takes String[] but the String class is unavailable"
                     << std::endl;
                return;
            }
            const llvm::DataLayout& dl = lmod->getDataLayout();
            const llvm::StructLayout* sl = dl.getStructLayout(strStructTy);
            llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
            auto i64c = [&](uint64_t v) {
                return llvm::ConstantInt::get(i64Ty, v);
            };
            // Field order matches the literal materialization: 0 vtable,
            // 1 bytes, 2 byteLength, 3 mode, 4 cachedCpLength.
            llvm::Value* strSize    = i64c(dl.getTypeAllocSize(strStructTy));
            llvm::Value* offBytes   = i64c(sl->getElementOffset(1));
            llvm::Value* offByteLen = i64c(sl->getElementOffset(2));
            llvm::Value* offMode    = i64c(sl->getElementOffset(3));
            llvm::Value* offCpLen   = i64c(sl->getElementOffset(4));

            llvm::Constant* vtableRef =
                llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx, 0));
            if (auto* vt = klass->getVirtualTableGlobal()) {
                vtableRef = CajetaModule::ensureGlobalInModule(lmod, vt);
            }

            llvm::FunctionType* argsMakeTy = llvm::FunctionType::get(
                ptrTy,
                {i64Ty, ptrTy, ptrTy, i64Ty, i64Ty, i64Ty, i64Ty, i64Ty},
                false);
            llvm::FunctionCallee argsMakeFn =
                lmod->getOrInsertFunction("__cajeta_args_make", argsMakeTy);
            llvm::Value* argcI64 = b.CreateSExt(mainFn->getArg(0), i64Ty);
            llvm::Value* argsArray = b.CreateCall(argsMakeFn,
                {argcI64, mainFn->getArg(1), vtableRef, strSize,
                 offBytes, offByteLen, offMode, offCpLen});
            callArgs.push_back(argsArray);
        }

        llvm::Value* ret = b.CreateCall(entryExtern, callArgs);
        // Translate the cajeta return into a C exit code.
        //   int32  → cast (identity) to i32 and return.
        //   void   → return 0.
        //   other  → return 0 (the caller can inspect side effects).
        if (entryFn->getReturnType()->isIntegerTy(32)) {
            b.CreateRet(ret);
        } else if (entryFn->getReturnType()->isVoidTy()) {
            b.CreateRet(llvm::ConstantInt::get(i32Ty, 0));
        } else if (entryFn->getReturnType()->isIntegerTy()) {
            // Wider/narrower int — truncate or zext to i32.
            llvm::Value* trunc = b.CreateIntCast(ret, i32Ty, /*isSigned=*/true);
            b.CreateRet(trunc);
        } else {
            b.CreateRet(llvm::ConstantInt::get(i32Ty, 0));
        }
    }

    void Compiler::collectLinkModules(std::vector<llvm::Module*>& lmods) {
        // Every llvm::Module that participates in the final link: user modules,
        // re-driven classpath deps, and the always-linked stdlib.
        auto addMod = [&](const CajetaModulePtr& m) {
            if (m && m->getLlvmModule()) lmods.push_back(m->getLlvmModule());
        };
        for (auto& m : modules) addMod(m);
        for (auto& m : externalModules) addMod(m);
        if (auto stdlib = CajetaModule::getStdlibModule()) addMod(stdlib);
        std::sort(lmods.begin(), lmods.end());
        lmods.erase(std::unique(lmods.begin(), lmods.end()), lmods.end());
    }

    std::unordered_set<std::string> Compiler::computeReachableSymbols(
            const std::vector<llvm::Module*>& lmods,
            std::unordered_map<std::string, llvm::GlobalValue*>& defs,
            bool excludeClinitRoots) {
        using namespace llvm;

        // Reachability is by SYMBOL NAME, not pointer: a callee is an extern decl
        // in the caller's module but a definition in another. Map each name to its
        // defining GlobalValue (function with a body / global with an initializer).
        for (auto* lm : lmods) {
            for (auto& f : lm->functions())
                if (!f.isDeclaration()) defs.emplace(f.getName().str(), &f);
            for (auto& g : lm->globals())
                if (g.hasInitializer()) defs.emplace(g.getName().str(), &g);
        }

        // Pull every GlobalValue name referenced inside a constant (initializers:
        // vtable slots, #ClassObject -> vtable, global_ctors array, constexpr
        // wrappers around a global).
        std::function<void(const Constant*, std::vector<std::string>&)> collectConst =
            [&](const Constant* c, std::vector<std::string>& out) {
                if (!c) return;
                if (auto* gv = dyn_cast<GlobalValue>(c)) {
                    out.push_back(gv->getName().str());
                    return;
                }
                for (auto& op : c->operands())
                    if (auto* sub = dyn_cast<Constant>(op.get())) collectConst(sub, out);
            };

        // Roots = what --gc-sections cannot drop for a final exe: the C `main`
        // shim, every llvm.global_ctors entry (run at startup), and llvm.used pins.
        std::vector<std::string> rootNames;
        for (auto* lm : lmods) {
            if (auto* mainFn = lm->getFunction("main"))
                if (!mainFn->isDeclaration()) rootNames.push_back("main");
            if (auto* gc = lm->getGlobalVariable("llvm.global_ctors")) {
                if (gc->hasInitializer())
                    if (auto* arr = dyn_cast<ConstantArray>(gc->getInitializer()))
                        for (auto& op : arr->operands())
                            if (auto* e = dyn_cast<ConstantStruct>(op.get()))
                                if (e->getNumOperands() >= 2)
                                    if (auto* fn = dyn_cast<Function>(
                                            e->getOperand(1)->stripPointerCasts())) {
                                        if (excludeClinitRoots &&
                                                fn->getName().starts_with(
                                                    "__cajeta_clinit_"))
                                            continue;
                                        rootNames.push_back(fn->getName().str());
                                    }
            }
            for (const char* un : {"llvm.used", "llvm.compiler.used"})
                if (auto* u = lm->getGlobalVariable(un))
                    if (u->hasInitializer())
                        collectConst(u->getInitializer(), rootNames);
        }

        // BFS the by-name reference graph. A function references the globals used
        // in its instructions (direct calls, function-pointer refs, global loads);
        // a global references the globals in its initializer. Conservative and
        // matches section-GC: referencing a vtable keeps all its slot functions.
        std::unordered_set<std::string> reached;
        std::vector<std::string> work;
        auto enqueue = [&](const std::string& n) {
            if (defs.count(n) && reached.insert(n).second) work.push_back(n);
        };
        for (auto& r : rootNames) enqueue(r);

        while (!work.empty()) {
            std::string name = std::move(work.back());
            work.pop_back();
            GlobalValue* gv = defs[name];
            std::vector<std::string> refs;
            if (auto* f = dyn_cast<Function>(gv)) {
                // Soundness edges NOT carried as instruction operands: the EH
                // personality function and prefix/prologue data. Missing these
                // would let pruning drop a function the ABI still calls.
                if (f->hasPersonalityFn())
                    if (auto* p = dyn_cast<GlobalValue>(
                            f->getPersonalityFn()->stripPointerCasts()))
                        refs.push_back(p->getName().str());
                if (f->hasPrefixData())
                    if (auto* c = dyn_cast<Constant>(f->getPrefixData()))
                        collectConst(c, refs);
                if (f->hasPrologueData())
                    if (auto* c = dyn_cast<Constant>(f->getPrologueData()))
                        collectConst(c, refs);
                for (auto& bb : *f)
                    for (auto& inst : bb)
                        for (auto& op : inst.operands()) {
                            Value* v = op.get();
                            if (auto* g = dyn_cast<GlobalValue>(v->stripPointerCasts()))
                                refs.push_back(g->getName().str());
                            else if (auto* c = dyn_cast<Constant>(v))
                                collectConst(c, refs);
                        }
            } else if (auto* g = dyn_cast<GlobalVariable>(gv)) {
                if (g->hasInitializer()) collectConst(g->getInitializer(), refs);
            }
            for (auto& r : refs) enqueue(r);
        }
        return reached;
    }

    void Compiler::reportTreeShake() {
        using namespace llvm;
        std::vector<Module*> lmods;
        collectLinkModules(lmods);
        std::unordered_map<std::string, GlobalValue*> defs;
        std::unordered_set<std::string> reached = computeReachableSymbols(lmods, defs);

        // Subsystem bucket for a per-area breakdown — strip the leading
        // `__cajeta_cajeta_` mangling and take the package head (e.g. net, gpu).
        auto subsystemOf = [](const std::string& s) -> std::string {
            static const char* P = "__cajeta_cajeta_";
            size_t i = (s.rfind(P, 0) == 0) ? std::strlen(P) : 0;
            size_t j = s.find('_', i);
            return (j == std::string::npos) ? std::string("<other>")
                                            : s.substr(i, j - i);
        };
        // The headline net/TLS/OpenSSL tail (incl. template instantiations over
        // net types). Used for the soundness signal (none should be reachable).
        auto looksNet = [](const std::string& s) {
            auto has = [&](const char* k){ return s.find(k) != std::string::npos; };
            return has("_net_") || has("Tls") || has("tls") || has("Ssl")
                || has("ssl") || has("crypto") || has("Crypto")
                || has("socket") || has("Socket");
        };

        // Diff: defined functions not reached are Phase-B strip candidates.
        size_t totalFns = 0, reachedFns = 0, netReached = 0;
        std::vector<std::string> strip, netStrip;
        std::map<std::string, size_t> stripBySubsystem;
        for (auto& [n, g] : defs) {
            if (!isa<Function>(g)) continue;
            totalFns++;
            bool net = looksNet(n);
            if (reached.count(n)) {
                reachedFns++;
                if (net) netReached++;
            } else {
                strip.push_back(n);
                stripBySubsystem[subsystemOf(n)]++;
                if (net) netStrip.push_back(n);
            }
        }
        std::sort(netStrip.begin(), netStrip.end());

        std::cout << "\n=== tree-shake (Tier-1 RTA Phase A — report only, no emission change) ===\n";
        std::cout << "modules analyzed   : " << lmods.size() << "\n";
        std::cout << "defined functions  : " << totalFns << "\n";
        std::cout << "  reachable        : " << reachedFns << "\n";
        std::cout << "  strippable       : " << strip.size();
        if (totalFns)
            std::cout << "  (" << (strip.size() * 100 / totalFns) << "%)";
        std::cout << "\n";
        // Soundness signal: in a non-net program no net/TLS symbol may be
        // reachable. A nonzero here means an unexpected edge kept the net tail.
        std::cout << "net/TLS reachable  : " << netReached
                  << "  (strippable: " << netStrip.size() << ")\n";

        std::cout << "strippable by subsystem (top 12):\n";
        std::vector<std::pair<std::string, size_t>> bySub(
            stripBySubsystem.begin(), stripBySubsystem.end());
        std::sort(bySub.begin(), bySub.end(),
            [](auto& a, auto& b){ return a.second > b.second; });
        for (size_t i = 0; i < bySub.size() && i < 12; ++i)
            std::cout << "  " << bySub[i].first << " : " << bySub[i].second << "\n";

        if (!netStrip.empty()) {
            std::cout << "net/TLS strippable sample (Phase B would not emit these):\n";
            size_t cap = std::min<size_t>(netStrip.size(), 12);
            for (size_t i = 0; i < cap; ++i)
                std::cout << "  " << netStrip[i] << "\n";
            if (netStrip.size() > cap)
                std::cout << "  ... and " << (netStrip.size() - cap) << " more\n";
        }
        std::cout << "=== end tree-shake report ===\n\n";
    }

    void Compiler::pruneUnreachable() {
        using namespace llvm;
        std::vector<Module*> lmods;
        collectLinkModules(lmods);
        std::unordered_map<std::string, GlobalValue*> defs;
        std::unordered_set<std::string> reached = computeReachableSymbols(lmods, defs);

        // The precise set of prunable functions = every cajeta METHOD body,
        // taken from Method->llvm::Function (NOT a name prefix). This covers all
        // packages (user + stdlib) and can never touch runtime/ABI helpers,
        // natives, ctors, clinits, or reflect adapters — none of those are
        // Methods. deleteBody turns an unreachable method into an extern
        // declaration: it emits no code, and (critically) its references to
        // native symbols vanish, so a non-TLS program no longer references
        // `__cajeta_tls_*` and the linker never pulls cajeta_tls.o / OpenSSL (the
        // win --gc-sections can't get on its own; see stdlib-tree-shaking.md). By
        // BFS soundness an unreachable method is referenced only by other
        // unreachable entities (a reachable vtable keeps all its slots), so
        // dropping its body strands no live ref; an unsound prune would fail loud
        // as an undefined symbol at link, not silently miscompile. ExternalLinkage
        // keeps an internal/private function valid IR once it is bodyless;
        // unreferenced, it emits nothing.
        size_t pruned = 0, keptFns = 0, totalFns = 0;
        std::unordered_set<llvm::Function*> seen;
        auto consider = [&](const CajetaModulePtr& mod) {
            if (!mod) return;
            for (auto& method : mod->getAllMethods()) {
                if (!method) continue;
                Function* f = method->getLlvmFunction();
                if (!f || f->isDeclaration()) continue;
                if (!seen.insert(f).second) continue;   // a method appears once
                totalFns++;
                if (reached.count(f->getName().str())) { keptFns++; continue; }
                f->deleteBody();
                f->setLinkage(GlobalValue::ExternalLinkage);
                pruned++;
            }
        };
        for (auto& m : modules) consider(m);
        for (auto& m : externalModules) consider(m);
        if (auto stdlib = CajetaModule::getStdlibModule()) consider(stdlib);
        std::cout << "tree-shake (--tree-shake=on): pruned " << pruned
                  << " of " << totalFns
                  << " unreachable cajeta method bodies (kept " << keptFns << ")\n";
    }

    // Rebuild a module's @llvm.global_ctors initializer, dropping every entry whose
    // ctor function name is in `excludeFnNames`. The array length changes, so the
    // global's type changes — we create a replacement global and rename it.
    static void rebuildGlobalCtorsExcluding(
            llvm::Module* lm,
            const std::unordered_set<std::string>& excludeFnNames) {
        using namespace llvm;
        auto* gc = lm->getGlobalVariable("llvm.global_ctors");
        if (!gc || !gc->hasInitializer()) return;
        auto* arr = dyn_cast<ConstantArray>(gc->getInitializer());
        if (!arr) return;
        SmallVector<Constant*, 32> kept;
        for (auto& op : arr->operands()) {
            bool drop = false;
            if (auto* e = dyn_cast<ConstantStruct>(op.get()))
                if (e->getNumOperands() >= 2)
                    if (auto* fn = dyn_cast<Function>(
                            e->getOperand(1)->stripPointerCasts()))
                        drop = excludeFnNames.count(fn->getName().str()) > 0;
            if (!drop) kept.push_back(cast<Constant>(op.get()));
        }
        if (kept.size() == arr->getNumOperands()) return;  // nothing removed here
        auto* elemTy = cast<StructType>(arr->getType()->getElementType());
        auto* newArrTy = ArrayType::get(elemTy, kept.size());
        Constant* newInit = ConstantArray::get(newArrTy, kept);
        auto* newGc = new GlobalVariable(*lm, newArrTy, /*isConstant=*/false,
            gc->getLinkage(), newInit, "");
        if (gc->hasSection()) newGc->setSection(gc->getSection());
        gc->eraseFromParent();             // global_ctors has no IR uses
        newGc->setName("llvm.global_ctors");
    }

    void Compiler::pruneDeadClinits() {
        using namespace llvm;
        std::vector<Module*> lmods;
        collectLinkModules(lmods);
        std::unordered_map<std::string, GlobalValue*> defs;
        // "What the program reaches if no static initializer ran" — the oracle for
        // whether a clinit's effect is observable.
        std::unordered_set<std::string> reachedSansClinits =
            computeReachableSymbols(lmods, defs, /*excludeClinitRoots=*/true);

        const StringRef CLP = "__cajeta_clinit_";
        std::vector<Function*> clinits;
        for (auto* lm : lmods)
            for (auto& f : lm->functions())
                if (!f.isDeclaration() && f.getName().starts_with(CLP))
                    clinits.push_back(&f);

        auto globalsTouched = [](Function* f, bool stores,
                                 std::unordered_set<std::string>& out) {
            for (auto& bb : *f)
                for (auto& inst : bb) {
                    Value* ptr = nullptr;
                    if (stores) {
                        if (auto* st = dyn_cast<StoreInst>(&inst))
                            ptr = st->getPointerOperand();
                    } else {
                        if (auto* ld = dyn_cast<LoadInst>(&inst))
                            ptr = ld->getPointerOperand();
                    }
                    if (ptr)
                        if (auto* g = dyn_cast<GlobalVariable>(
                                getUnderlyingObject(ptr)))
                            out.insert(g->getName().str());
                }
        };

        // Globals loaded by each clinit (for the "read by another clinit" check).
        std::unordered_map<Function*, std::unordered_set<std::string>> clinitLoads;
        for (auto* cl : clinits) globalsTouched(cl, /*stores=*/false, clinitLoads[cl]);

        // A native (declaration) callee with no OBSERVABLE program-level effect:
        // fiber scope-stack bookkeeping, frame/temp allocation, and memory
        // intrinsics. Deliberately EXCLUDES drop-chain registration and all I/O —
        // so any clinit that constructs an owned object (registers a drop) or does
        // I/O reaches a non-benign native and is conservatively KEPT. `scope_exit`
        // only does observable work (runs registered drops) when a drop was
        // registered, which already trips the non-benign path, so whitelisting the
        // scope ops is sound for the drop-free clinits this can actually strip.
        auto isBenignNative = [](StringRef n) {
            return n.starts_with("__cajeta_scope_")
                || n == "__cajeta_alloc" || n == "__cajeta_dbg_local_alloc"
                || n.starts_with("llvm.memcpy") || n.starts_with("llvm.memset")
                || n.starts_with("llvm.memmove") || n.starts_with("llvm.lifetime")
                || n.starts_with("llvm.dbg") || n.starts_with("llvm.assume");
        };

        // Analyze a clinit's transitive closure (clinit + body-having callees) for
        // EXTERNAL PURITY. Returns true ("impure", keep) on any indirect call or any
        // call to a NON-BENIGN declaration (I/O, drop-chain, registration, ...).
        // Collects every global the closure STOREs to, via the store's underlying
        // object so GEP/cast-derived global writes count.
        // NOTE soundness: it is NOT enough that callees are unreached-sans-clinits —
        // a dead callee can still store to a LIVE global. We check the stores
        // directly; the clinit is strippable only if the whole closure writes
        // nothing live and triggers no opaque effect.
        auto analyzeClosure = [&](Function* start,
                                  std::unordered_set<std::string>& storedGlobals) -> bool {
            bool impure = false;
            std::vector<Function*> stack{start};
            std::unordered_set<Function*> visited{start};
            while (!stack.empty()) {
                Function* f = stack.back(); stack.pop_back();
                for (auto& bb : *f)
                    for (auto& inst : bb) {
                        if (auto* st = dyn_cast<StoreInst>(&inst)) {
                            Value* base = getUnderlyingObject(st->getPointerOperand());
                            if (auto* g = dyn_cast<GlobalVariable>(base))
                                storedGlobals.insert(g->getName().str());
                        } else if (auto* call = dyn_cast<CallBase>(&inst)) {
                            Function* callee = call->getCalledFunction();
                            if (!callee) { impure = true; continue; }   // indirect
                            if (callee->isDeclaration()) {
                                if (!isBenignNative(callee->getName()))
                                    impure = true;   // opaque native effect
                                continue;            // native leaf — don't recurse
                            }
                            if (visited.insert(callee).second) stack.push_back(callee);
                        }
                    }
            }
            return impure;
        };

        std::vector<Function*> toStrip;
        for (auto* cl : clinits) {
            // Externally pure: the closure writes only DEAD globals (nothing live
            // reads them) and makes no indirect / native call.
            std::unordered_set<std::string> stored;
            if (analyzeClosure(cl, stored)) continue;        // native/indirect -> keep
            bool pure = true;
            for (auto& g : stored)
                if (reachedSansClinits.count(g)) { pure = false; break; }  // live write
            if (!pure) continue;
            // The clinit's own statics must not be read by ANOTHER clinit — if that
            // one is kept it would then read an uninitialized global. Conservative
            // (no inter-clinit fixpoint): any shared read keeps this clinit.
            std::unordered_set<std::string> W;
            globalsTouched(cl, /*stores=*/true, W);
            bool sharedWithClinit = false;
            for (auto& g : W) {
                for (auto& [other, loads] : clinitLoads)
                    if (other != cl && loads.count(g)) { sharedWithClinit = true; break; }
                if (sharedWithClinit) break;
            }
            if (sharedWithClinit) continue;
            toStrip.push_back(cl);
        }

        if (toStrip.empty()) {
            std::cout << "tree-shake (--tree-shake=on): 0 of " << clinits.size()
                      << " class clinits are dead\n";
            return;
        }
        std::unordered_set<std::string> stripNames;
        for (auto* cl : toStrip) stripNames.insert(cl->getName().str());
        for (auto* lm : lmods) rebuildGlobalCtorsExcluding(lm, stripNames);
        for (auto* cl : toStrip) {
            cl->deleteBody();
            cl->setLinkage(GlobalValue::ExternalLinkage);
        }
        std::cout << "tree-shake (--tree-shake=on): stripped " << toStrip.size()
                  << " of " << clinits.size() << " dead class clinits\n";
    }

    void Compiler::emitArchive(const std::string& archiveRootPath, bool uber) {
        // Build an archive name from the entry method's class when the user
        // didn't pass -o. Fall back to "cajeta.cja" if nothing else.
        std::string outPath;
        if (!outputPath.empty()) {
            outPath = outputPath;
        } else {
            std::string baseName = "cajeta";
            if (!entryMethod.empty()) {
                auto lastDot = entryMethod.rfind('.');
                if (lastDot != std::string::npos && lastDot > 0) {
                    // Use the class portion: pkg.Class.method → Class
                    auto classPart = entryMethod.substr(0, lastDot);
                    auto pkgDot = classPart.rfind('.');
                    baseName = (pkgDot == std::string::npos)
                        ? classPart
                        : classPart.substr(pkgDot + 1);
                }
            }
            std::string root = archiveRootPath;
            if (!root.empty() && root.back() != '/') root += '/';
            outPath = root + baseName + ".cja";
        }

        std::string archiveName = "cajeta-archive";
        if (!entryMethod.empty()) {
            auto lastDot = entryMethod.rfind('.');
            if (lastDot != std::string::npos && lastDot > 0) {
                archiveName = entryMethod.substr(0, lastDot);
            }
        }
        CajetaArchive arc(archiveName, "1.0.0",
            uber ? CajetaArchive::Kind::Uber : CajetaArchive::Kind::Cja);

        // Per-entry struct used as a staging buffer while we compute
        // reachability — entries land in the output archive only after
        // the pruner decides which deps to keep. Holds both the binary
        // entry shape (name + origin + kindTag + bytes) and the
        // derived canonical name we use for reachability lookups.
        // depIndex identifies which entry came from which classpath
        // archive — -1 for user / stdlib, otherwise an index into the
        // staged-deps vector below. The dep-keyed prune drops an
        // entire depIndex group when no canonical from it survives.
        struct StagingEntry {
            CajetaArchiveEntry entry;
            std::string canonical;   // pkg.subpkg.Class form, no '.bc' suffix
            int depIndex = -1;       // -1 for user/stdlib; otherwise index into stagedDeps
            bool included = true;    // pruning sets false for excluded entries
        };

        // ---- Stage user + stdlib modules ----
        // Cja mode: user code only — strip the parsed-stdlib module so the
        // archive is a true library (consumer brings its own stdlib).
        // Uber mode: include stdlib (the artifact is meant to run, not be
        // re-consumed). Stdlib canonicals start with "cajeta.".
        std::vector<StagingEntry> staged;
        for (auto& module : modules) {
            std::string canonical = module->getQName()
                ? module->getQName()->toCanonical()
                : std::string("anonymous");
            bool isStdlib = canonical.rfind("cajeta.", 0) == 0;
            if (!uber && isStdlib) {
                // Cja: project-only. Skip the stdlib module entirely.
                continue;
            }
            std::string entryName = canonical;
            std::replace(entryName.begin(), entryName.end(), '.', '/');
            entryName += ".bc";

            std::string bitcode;
            {
                llvm::raw_string_ostream os(bitcode);
                llvm::WriteBitcodeToFile(*module->getLlvmModule(), os);
                os.flush();
            }

            StagingEntry se;
            se.entry.name      = entryName;
            se.entry.originTag = (uint8_t) (isStdlib
                ? CajetaArchive::Origin::Stdlib
                : CajetaArchive::Origin::User);
            se.entry.kindTag   = CajetaArchive::EntryKind::ClassBitcode;
            se.entry.data.assign(bitcode.begin(), bitcode.end());
            se.canonical       = canonical;
            se.depIndex        = -1;
            // User + stdlib are always included (reachability prunes only
            // dep entries; the stdlib bundle is always reachable since
            // user code links it).
            se.included = true;
            staged.push_back(std::move(se));

            // Ship the original .cajeta source bytes alongside the
            // bitcode so a downstream compile can ingest this archive
            // via --classpath and re-parse our classes into its own
            // canonical-name registry. Only for modules backed by a
            // real source file (user modules); the stdlib module is
            // synthesized from many embedded files into one logical
            // module and has no single source path — skip.
            if (!isStdlib && !module->getSourcePath().empty()) {
                std::ifstream srcFile(
                    module->getSourcePath(), std::ios::binary);
                if (srcFile) {
                    std::string srcText(
                        (std::istreambuf_iterator<char>(srcFile)),
                        std::istreambuf_iterator<char>());
                    std::string sourceEntryName = canonical;
                    std::replace(sourceEntryName.begin(),
                        sourceEntryName.end(), '.', '/');
                    sourceEntryName += ".cajeta";

                    StagingEntry sse;
                    sse.entry.name      = sourceEntryName;
                    sse.entry.originTag = (uint8_t) CajetaArchive::Origin::User;
                    sse.entry.kindTag   = CajetaArchive::EntryKind::ClassSource;
                    sse.entry.data.assign(srcText.begin(), srcText.end());
                    sse.canonical       = canonical;
                    sse.depIndex        = -1;
                    sse.included        = true;
                    staged.push_back(std::move(sse));
                } else {
                    std::cerr << "cajeta: warning — could not read source "
                              << "for archive packaging: "
                              << module->getSourcePath() << std::endl;
                }
            }
        }

        // Per-dep summary captured during classpath staging — name +
        // version come from each loaded classpath archive's manifest;
        // included_entry_count is filled in after pruning.
        std::vector<CajetaArchive::DepSummary> stagedDeps;

        // ---- Stage classpath (uber only) ----
        if (uber) {
            // Reachability-prune semantics: dep entries default to
            // included=false; the closure pass below flips included=true
            // for each dep canonical that any already-included bitcode
            // references via substring match. When pruneUber is off,
            // every dep entry is included up front.
            for (const auto& cpPath : classpath) {
                try {
                    auto dep = CajetaArchive::readFrom(cpPath);
                    int thisDepIdx = (int) stagedDeps.size();
                    CajetaArchive::DepSummary summary;
                    summary.name    = dep.getName().empty()
                        ? std::string("anonymous")
                        : dep.getName();
                    summary.version = dep.getVersion().empty()
                        ? std::string("0.0.0")
                        : dep.getVersion();
                    summary.includedEntryCount = 0;  // patched post-prune
                    stagedDeps.push_back(summary);

                    // Nested layout: every dep entry lives under
                    // deps/<name>-<version>/<original-path>. Two deps
                    // sharing a class canonical no longer collide on
                    // entry path (each gets its own subtree); same-class
                    // duplicates are a manifest-level version conflict
                    // that the cja shade tool resolves, not a writer-
                    // level dedupe concern.
                    std::string depPrefix = "deps/"
                        + summary.name + "-" + summary.version + "/";

                    for (const auto& depEntry : dep.getEntries()) {
                        std::string nestedName = depPrefix + depEntry.name;
                        // Dedupe by nested path (same dep listed twice on
                        // classpath — first wins).
                        bool seen = false;
                        for (const auto& already : staged) {
                            if (already.entry.name == nestedName) {
                                seen = true;
                                break;
                            }
                        }
                        if (seen) continue;

                        // Canonical name for reachability lookup uses the
                        // ORIGINAL dep-internal path (no `deps/...` prefix),
                        // because that's what shows up in any cross-module
                        // reference's mangled symbol string.
                        std::string canonical = depEntry.name;
                        if (canonical.size() > 3
                                && canonical.compare(canonical.size() - 3, 3, ".bc") == 0) {
                            canonical.resize(canonical.size() - 3);
                        }
                        std::replace(canonical.begin(), canonical.end(), '/', '.');

                        StagingEntry se;
                        se.entry.name      = std::move(nestedName);
                        se.entry.originTag = (uint8_t) CajetaArchive::Origin::Dependency;
                        se.entry.kindTag   = depEntry.kindTag;
                        se.entry.data      = depEntry.data;
                        se.canonical       = std::move(canonical);
                        se.depIndex        = thisDepIdx;
                        // Default to "include" when prune is off; otherwise
                        // wait for the closure pass to vouch.
                        se.included        = !pruneUber;
                        staged.push_back(std::move(se));
                    }
                } catch (const std::exception& e) {
                    cerr << "cajeta: --classpath ingestion failed for `"
                         << cpPath << "`: " << e.what() << std::endl;
                    throw;
                }
            }

            if (pruneUber) {
                // Iterative substring-scan closure. Initial reachable set
                // is the user+stdlib entries (depIndex == -1). On each
                // pass, scan every reachable entry's bitcode for any
                // not-yet-reachable dep canonical's name as a substring;
                // promote matches and re-iterate until quiescent.
                //
                // Substring match works because cajeta's RTTI globals
                // embed the canonical name as a literal C string and
                // every method's LLVM function name carries the
                // canonical prefix. Substring is conservative — false-
                // positives keep over-large dep entries (acceptable),
                // false-negatives would drop needed ones (which the
                // closure's repeated passes avoid).
                bool changed = true;
                while (changed) {
                    changed = false;
                    for (std::size_t i = 0; i < staged.size(); ++i) {
                        if (staged[i].included) continue;
                        for (std::size_t j = 0; j < staged.size(); ++j) {
                            if (!staged[j].included) continue;
                            const auto& bc = staged[j].entry.data;
                            const auto& needle = staged[i].canonical;
                            if (needle.empty()) continue;
                            auto it = std::search(
                                bc.begin(), bc.end(),
                                needle.begin(), needle.end());
                            if (it != bc.end()) {
                                staged[i].included = true;
                                changed = true;
                                break;
                            }
                        }
                    }
                }
            }
        }

        // ---- Emit included entries in stage order, count per-dep survivors ----
        for (auto& se : staged) {
            if (!se.included) continue;
            if (se.depIndex >= 0
                    && (std::size_t) se.depIndex < stagedDeps.size()) {
                stagedDeps[se.depIndex].includedEntryCount++;
            }
            arc.addEntry(std::move(se.entry));
        }

        // Drop deps whose entire entry set was pruned — manifest deps
        // array reflects only deps that contributed at least one entry
        // to the final archive. Lets readers see at a glance what
        // actually got bundled.
        if (uber) {
            std::vector<CajetaArchive::DepSummary> kept;
            for (auto& d : stagedDeps) {
                if (d.includedEntryCount > 0) kept.push_back(std::move(d));
            }
            arc.setDeps(std::move(kept));
        }

        // Embed the package's hand-authored skills (skill-discovery D.3): a
        // no-op when there is no skills/ dir; throws a clear error on an invalid
        // skill. Uber archives carry the consuming project's own skills only —
        // each dependency already ships its skills inside its own .cja.
        buildtool::skill::addSkillMembersToArchiveOrThrow(arc, skillSourceRoot);

        // Native-deps unit 13: default packaging step — bundle the resolved
        // native artifacts for the live @Native libs into the archive's native/
        // tree (so a published .cja carries its redistributable natives; a
        // provisioned embargoed artifact rides along too). No-op when no
        // @Native lib is live. Bundles the host platform's artifact (the only
        // one a single build host has); a missing one is reported, not fatal.
        {
            std::set<std::string> liveLibs;
            for (auto& m : modules)
                if (m && m->getLlvmModule()) {
                    auto l = collectLiveNativeLibs(*m->getLlvmModule());
                    liveLibs.insert(l.begin(), l.end());
                }
            if (!liveLibs.empty()) {
                const std::string platform = hostNativePlatform();
                const auto dirs = nativeLinkSearchDirs();
                std::string reqsJson = "{\"requires\":[";
                bool first = true;
                for (const auto& lib : liveLibs) {
                    if (!first) reqsJson += ",";
                    reqsJson += "\"" + lib + "\"";
                    first = false;
                    auto art = findNativeJitArtifact(lib, platform, dirs);
                    if (!art) {
                        cerr << "cajeta: note: native lib '" << lib
                             << "' not bundled (not provisioned for "
                             << platform << ")" << std::endl;
                        continue;
                    }
                    std::ifstream in(art->path, std::ios::binary);
                    std::vector<uint8_t> bytes(
                        (std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
                    std::string fname =
                        std::filesystem::path(art->path).filename().string();
                    arc.addNativeArtifact(platform, fname, std::move(bytes));
                }
                reqsJson += "],\"libraries\":{}}";
                arc.setNativeLibrariesMeta(
                    std::vector<uint8_t>(reqsJson.begin(), reqsJson.end()));
            }
        }

        arc.writeTo(outPath);
    }
}