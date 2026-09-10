//
// Created by James Klappenbach on 10/24/22.
//

#include "Compiler.h"

#include <chrono>
#include <cstdio>
#include "CajetaArchive.h"
#include "cajeta/buildtool/IrCache.h"
#include "cajeta/buildtool/Lockfile.h"   // sha256Hex
#include "cajeta/buildtool/PrimeCache.h"
#include "cajeta/buildtool/skill/SkillPackager.h"
#include "DropBackfill.h"
#include "ObligationReplay.h"
#include "ReflectionKeepSet.h"

// Stamped by CMake add_compile_definitions; fallbacks for editors/tools.
#ifndef CAJETA_VERSION
#define CAJETA_VERSION "0.0.0-unknown"
#endif
#ifndef CAJETA_GIT_HASH
#define CAJETA_GIT_HASH "unknown"
#endif
#include "CajetaModule.h"
#include "NativeLink.h"
#include "CajetaLlvmVisitor.h"
#include "ScriptUnitSynthesis.h"
#include "Optimizer.h"
#include "StdlibEmbedded.h"
#include "cajeta/dbg/DebugCodegen.h"
#include "cajeta/xref/XrefIndex.h"
#include "cajeta/xref/StaticReceiverCapture.h"
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
#include "cajeta/error/Diagnostics.h"
#include "cajeta/error/DiagnosticEngine.h"
#include "CajetaParserBaseVisitor.h"
#include "../xpu/core/XpuAttributes.h"
#include "../xpu/XpuTarget.h"
#include "../xpu/core/KernelManifest.h"
#include "../xpu/mir/XpuMirBuilder.h"
#include "../asn/expression/LiteralExpression.h"
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
#include <memory>
#include <unordered_map>
#include <unordered_set>

#ifdef CAJETA_HAS_LLD
#include "lld/Common/Driver.h"
LLD_HAS_DRIVER(elf)
#endif


using namespace antlr4;
using namespace std;

namespace cajeta {

    // --diag-format=json: re-emits ANTLR lexer/parser syntax errors as NDJSON
    // (emitJsonDiagnostic) instead of the console listener's free text.
    // Installed only for the user-source parse.
    class JsonSyntaxErrorListener : public antlr4::BaseErrorListener {
    public:
        explicit JsonSyntaxErrorListener(std::string file) : file(std::move(file)) {}
        void syntaxError(antlr4::Recognizer* /*recognizer*/,
                         antlr4::Token* /*offendingSymbol*/,
                         size_t line, size_t charPositionInLine,
                         const std::string& msg,
                         std::exception_ptr /*e*/) override {
            // ANTLR columns are 0-based; emitJsonDiagnostic wants 1-based.
            emitJsonDiagnostic("error", "syntax", msg, file,
                               static_cast<int>(line),
                               static_cast<int>(charPositionInLine) + 1);
        }
    private:
        std::string file;
    };

    // Serialize the lean keep-set (canonical -> the reason it was kept) to JSON
    // at `path`; forcesAll writes a null count with an empty class list.
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
            logLine("warn",
                    "warning: could not write keepset JSON to " + path + "\n");
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

    // Null in production; the test StdlibCache installs a shared context here so
    // a primed stdlib survives across Compiler instances (see the ctor).
    thread_local llvm::LLVMContext* Compiler::s_sharedContext = nullptr;
    thread_local bool Compiler::s_sharedInitialized = false;
    thread_local bool Compiler::s_reuseHazardArmed = false;

    // Rebuild `target` + `targetMachine` for the current triple/cpu/features.
    // Defaults to PIC with per-function/data sections and .init_array ctors;
    // `native` resolves to host cpu+features, degrading to generic when cross.
    void Compiler::rebuildTargetMachine() {
        string error;
        llvm::Triple triple(targetTriple);
        target = llvm::TargetRegistry::lookupTarget(triple, error);
        if (!target) {
            cerr << "cajeta: could not lookup target '" << targetTriple << "': " << error << std::endl;
            targetMachine = nullptr;
            return;
        }
        // Default to PIC: modern distros link PIE, and a non-PIC object trips
        // `relocation R_X86_64_32 ... can not be used when making a PIE object`.
        // setRelocationModel still overrides for embedded / kernel targets.
        auto effectiveRM = RM.has_value() ? RM : std::optional<llvm::Reloc::Model>(llvm::Reloc::PIC_);
        // One ELF section per function and per data global, so the linker's
        // --gc-sections can drop what nothing references.
        opt.FunctionSections = true;
        opt.DataSections     = true;
        // TargetOptions defaults this false, which emits the legacy `.ctors`
        // section modern glibc startup never runs — every AOT global ctor
        // (clinits, the vtable marker, XPU registration) then silently never fires.
        opt.UseInitArray     = true;
        std::string effectiveCpu = cpu;
        std::string effectiveFeatures = features;
        // `native` is the DEFAULT, so it has to degrade: getHostCPUName() answers
        // an x86 cpu name whatever the triple says, so a cross-compile falls back
        // to generic unless the caller named a cpu explicitly.
        if (cpu == "native"
                && targetTriple != llvm::sys::getDefaultTargetTriple()) {
            effectiveCpu = "generic";
        } else if (cpu == "native") {
            effectiveCpu = llvm::sys::getHostCPUName().str();
            llvm::SubtargetFeatures feats(features); // seed with any explicit --features
            for (const auto& f : llvm::sys::getHostCPUFeatures())
                feats.AddFeature(f.first(), f.second);
            effectiveFeatures = feats.getString();
        }
        targetMachine = target->createTargetMachine(triple, effectiveCpu, effectiveFeatures, opt, effectiveRM);
    }

    // Defined below, beside the lazy-stdlib bookkeeping it feeds.
    static void notePrescannedImport(const std::string& pkg);

    // Shallow parse-tree walk registering every declared class / interface /
    // record / view / enum as (canonical, shortName) in the archive, so the main
    // visitor pass creates placeholders only for names declared somewhere.
    class ArchivePrescanVisitor : public CajetaParserBaseVisitor {
    public:
        std::string package;
        std::vector<std::string> enclosingStack;
        // Declaring file for this unit's classes when the prescan came from an
        // on-disk source (empty for stdlib units); drives materializeUserClass.
        std::string sourcePath;

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

        // Record which on-demand stdlib packages the tree imports, so they can all
        // be made concrete before the first body walk (drainPrescannedLazyStdlib).
        std::any visitImportDeclaration(
                CajetaParser::ImportDeclarationContext* ctx) override {
            if (auto* qn = ctx->qualifiedName()) {
                const auto& ids = qn->identifier();
                // An import names a TYPE, so its package is everything up to the
                // last segment; a trailing `.*` parses as MUL, not an identifier.
                size_t take = ctx->MUL() ? ids.size() : (ids.size() - 1);
                std::string pkg;
                for (size_t i = 0; i < take; ++i) {
                    if (i) pkg += ".";
                    pkg += ids[i]->getText();
                }
                if (!pkg.empty()) notePrescannedImport(pkg);
            }
            return defaultResult();
        }

        std::any visitClassDeclaration(
                CajetaParser::ClassDeclarationContext* ctx) override {
            // @ValueType sits on the enclosing typeDeclaration's modifiers, not on
            // the class body, so the placeholder flag is detected from up there.
            registerAndRecurse(ctx->identifier()->getText(), ctx,
                                /*markEnum=*/false,
                                /*markValueType=*/classHasValueTypeAnnotation(ctx));
            captureTemplateMeta(ctx);
            // @GenerateMock synthesizes a sibling Mock<Name> with no source
            // declaration of its own; archive that name so a forward reference
            // resolves to a placeholder synthesizeMock later fills.
            if (classHasGenerateMockAnnotation(ctx)) {
                std::string mockShort =
                    std::string("Mock") + ctx->identifier()->getText();
                std::string canonical;
                if (!package.empty()) canonical = package;
                for (auto& e : enclosingStack) {
                    if (!canonical.empty()) canonical += ".";
                    canonical += e;
                }
                if (!canonical.empty()) canonical += ".";
                canonical += mockShort;
                CajetaType::registerArchive(canonical, mockShort);
            }
            return defaultResult();
        }

        std::any visitInterfaceDeclaration(
                CajetaParser::InterfaceDeclarationContext* ctx) override {
            // markInterface so a forward-referenced interface-typed field gets a
            // FAT 24-byte placeholder; a thin class pointer silently drops dispatch.
            registerAndRecurse(ctx->identifier()->getText(), ctx,
                                /*markEnum=*/false, /*markValueType=*/false,
                                /*markInterface=*/true);
            captureTemplateMeta(ctx);
            return defaultResult();
        }

        std::any visitViewDeclaration(
                CajetaParser::ViewDeclarationContext* ctx) override {
            // markView=true so a cross-file forward reference synthesizes a
            // CajetaView placeholder (see fromContext), not a class shell.
            registerAndRecurse(ctx->identifier()->getText(), ctx,
                                /*markEnum=*/false, /*markValueType=*/false,
                                /*markInterface=*/false, /*markView=*/true);
            return defaultResult();
        }

        std::any visitRecordDeclaration(
                CajetaParser::RecordDeclarationContext* ctx) override {
            // Records are value types from birth — cross-file placeholders
            // must carry VALUE_TYPE_FLAG | BY_VALUE_FLAG (same as @ValueType).
            registerAndRecurse(ctx->identifier()->getText(), ctx,
                                /*markEnum=*/false, /*markValueType=*/true);
            captureTemplateMeta(ctx);
            // Every record MAY become a Table<R> schema, whose instantiation
            // synthesizes the `<R>Cols` / `<R>Rows` companions; archive those names
            // now so a reference before the instantiation gets a placeholder.
            {
                std::string prefix;
                if (!package.empty()) prefix = package;
                for (auto& e : enclosingStack) {
                    if (!prefix.empty()) prefix += ".";
                    prefix += e;
                }
                if (!prefix.empty()) prefix += ".";
                for (const char* suffix : {"Cols", "Rows"}) {
                    std::string shortName =
                        ctx->identifier()->getText() + std::string(suffix);
                    CajetaType::registerArchive(prefix + shortName, shortName);
                }
            }
            return defaultResult();
        }

        std::any visitEnumDeclaration(
                CajetaParser::EnumDeclarationContext* ctx) override {
            // markEnum so a cross-file field declaration gets an i32 enum
            // placeholder; a class-shaped one is the wrong layout at codegen.
            registerAndRecurse(ctx->identifier()->getText(), ctx,
                                /*markEnum=*/true);
            return defaultResult();
        }

    private:
        // True if the class declaration is annotated @ValueType. The annotation
        // sits on the enclosing typeDeclaration's modifier list, so reach up.
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

        // True if the class declaration is annotated @GenerateMock (same shape and
        // location as @ValueType, on the enclosing typeDeclaration).
        static bool classHasGenerateMockAnnotation(
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
                if (!ids.empty() && ids.back()->getText() == "GenerateMock") {
                    return true;
                }
            }
            return false;
        }

        // Register `shortName` under the canonical composed from package +
        // enclosing class stack, apply the requested archive marks, then recurse
        // with the name pushed on that stack.
        void registerAndRecurse(const std::string& shortName,
                                 antlr4::tree::ParseTree* tree,
                                 bool markEnum = false,
                                 bool markValueType = false,
                                 bool markInterface = false,
                                 bool markView = false) {
            std::string canonical;
            if (!package.empty()) canonical = package;
            for (auto& e : enclosingStack) {
                if (!canonical.empty()) canonical += ".";
                canonical += e;
            }
            if (!canonical.empty()) canonical += ".";
            canonical += shortName;
            CajetaType::registerArchive(canonical, shortName);
            if (!sourcePath.empty()) {
                CajetaType::registerArchiveSourcePath(canonical, sourcePath);
            }
            if (markEnum) CajetaType::markArchiveEnum(canonical);
            if (markValueType) CajetaType::markArchiveValueType(canonical);
            if (markInterface) CajetaType::markArchiveInterface(canonical);
            if (markView) CajetaType::markArchiveView(canonical);
            lastCanonical = canonical;
            enclosingStack.push_back(shortName);
            visitChildren(tree);
            enclosingStack.pop_back();
        }

        // Capture template metadata (type parameters plus the declaration's literal
        // source text) for a class / interface carrying a `typeParameters` clause,
        // so a use-site `T<args>` in a file that parses earlier can instantiate.
        template <typename ClassOrInterfaceCtx>
        void captureTemplateMeta(ClassOrInterfaceCtx* ctx) {
            auto* tps = ctx->typeParameters();
            if (!tps) return;
            std::vector<cajeta::TypeParameter> params;
            for (auto* tp : tps->typeParameter()) {
                cajeta::TypeParameter param(tp->identifier()->getText());
                if (tp->REFERENCE() != nullptr) {
                    throw Exception(
                        "`#` on a type parameter declaration is retired: "
                        "ownership is per-call under title-tracking "
                        "(specs/title-tracking-spec.md §8.1) — drop the `#` "
                        "from `<#" + param.name + ">` (a must-own edge is "
                        "spelled on the FORMAL, `f(#" + param.name + " x)`)",
                        "CAJETA_ERROR_TYPE_TRANSFER_RETIRED");
                }
                param.owningRequired = false;
                if (auto* pt = tp->primitiveType()) {
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
                    if (auto* dflt = tp->typeType()) {
                        param.defaultType = dflt->getText();
                    }
                }
                params.push_back(std::move(param));
            }
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

    // ---- Two-stage parsing (SLL, then full LL on failure) ---------------
    // Stage 1 parses under SLL with BailErrorStrategy and no listeners; stage 2
    // re-parses under full LL with the caller's real listeners when SLL bails.
    struct TwoStageStats {
        long long sll = 0;
        long long fallback = 0;
        ~TwoStageStats() {
            if (!std::getenv("CAJETA_PRIME_TIMING") || (sll + fallback) == 0) {
                return;
            }
            std::fprintf(stderr,
                "[two-stage] %lld parses: %lld SLL, %lld fell back to LL\n",
                sll + fallback, sll, fallback);
        }
    };
    static TwoStageStats g_twoStage;

    // On unless CAJETA_TWO_STAGE_PARSE=0, whose off state is exactly what stage 2
    // already does: full-LL-only parsing.
    static bool twoStageParseEnabled() {
        static const bool on = [] {
            const char* v = std::getenv("CAJETA_TWO_STAGE_PARSE");
            return !(v && std::string(v) == "0");
        }();
        return on;
    }

    // Parse a compilationUnit under SLL, falling back to a full-LL re-parse when
    // SLL bails. `installListeners` runs only on the path that reports diagnostics
    // — never for stage 1, whose failure is a signal to re-parse, not an error.
    static antlr4::tree::ParseTree* parseCompilationUnitTwoStage(
            CajetaParser& parser, antlr4::CommonTokenStream& tokens,
            const std::function<void()>& installListeners,
            const std::string& what = std::string()) {
        if (!twoStageParseEnabled()) {
            installListeners();
            return parser.compilationUnit();
        }
        parser.removeErrorListeners();
        parser.setErrorHandler(std::make_shared<antlr4::BailErrorStrategy>());
        parser.getInterpreter<antlr4::atn::ParserATNSimulator>()
            ->setPredictionMode(antlr4::atn::PredictionMode::SLL);
        try {
            antlr4::tree::ParseTree* tree = parser.compilationUnit();
            ++g_twoStage.sll;
            return tree;
        } catch (const antlr4::ParseCancellationException&) {
            // expected: SLL could not decide, or the input is malformed
        } catch (const antlr4::RecognitionException&) {
        }
        ++g_twoStage.fallback;
        if (std::getenv("CAJETA_PRIME_TIMING")) {
            std::fprintf(stderr, "[two-stage] fell back to LL: %s\n",
                         what.empty() ? "<unnamed>" : what.c_str());
        }
        tokens.seek(0);
        parser.reset();
        parser.setErrorHandler(std::make_shared<antlr4::DefaultErrorStrategy>());
        parser.getInterpreter<antlr4::atn::ParserATNSimulator>()
            ->setPredictionMode(antlr4::atn::PredictionMode::LL);
        installListeners();
        return parser.compilationUnit();
    }

    // CAJETA_PRIME_TIMING=1 aggregates lex / parse / visit across every prescan in
    // the process; which of the three dominates decides the lever.
    struct PrescanCost {
        long long lexNs = 0, parseNs = 0, visitNs = 0;
        int files = 0;
        ~PrescanCost() {
            if (!std::getenv("CAJETA_PRIME_TIMING") || files == 0) return;
            std::fprintf(stderr,
                "[prescan] %d files: lex %lld ms, parse %lld ms, visit %lld ms\n",
                files, lexNs / 1000000, parseNs / 1000000, visitNs / 1000000);
        }
    };
    static PrescanCost g_prescanCost;

    // Pre-scan one source: lex, two-stage parse, then walk it with
    // ArchivePrescanVisitor. suppressConsole drops ANTLR's console listener
    // (--diag-format=json); sourcePath is recorded against each declared canonical.
    static void prescanSource(antlr4::ANTLRInputStream& input,
                              bool suppressConsole = false,
                              const std::string& sourcePath = std::string()) {
        using PClock = std::chrono::steady_clock;
        auto t0 = PClock::now();
        CajetaLexer lexer(&input);
        CommonTokenStream tokens(&lexer);
        tokens.fill();
        auto t1 = PClock::now();
        CajetaParser parser(&tokens);
        if (suppressConsole) {
            lexer.removeErrorListeners();
            parser.removeErrorListeners();
        }
        antlr4::tree::ParseTree* tree = parseCompilationUnitTwoStage(
            parser, tokens, [&] {
                if (suppressConsole) {
                    parser.removeErrorListeners();
                } else {
                    parser.removeErrorListeners();
                    parser.addErrorListener(
                        &antlr4::ConsoleErrorListener::INSTANCE);
                }
            }, sourcePath);
        auto t2 = PClock::now();
        ArchivePrescanVisitor v;
        v.sourcePath = sourcePath;
        tree->accept(&v);
        auto t3 = PClock::now();
        using NS = std::chrono::nanoseconds;
        g_prescanCost.lexNs   += std::chrono::duration_cast<NS>(t1 - t0).count();
        g_prescanCost.parseNs += std::chrono::duration_cast<NS>(t2 - t1).count();
        g_prescanCost.visitNs += std::chrono::duration_cast<NS>(t3 - t2).count();
        ++g_prescanCost.files;
    }

    void prescanSourceRoot(const std::string& rootPath, bool suppressConsole) {
        using recursive_directory_iterator = std::filesystem::recursive_directory_iterator;
        std::filesystem::path root(rootPath);
        // Sorted: the archive registry is first-write-wins, so an unsorted walk lets
        // two same-short-name declarations bind differently per checkout.
        std::vector<std::filesystem::path> sources;
        for (const auto& entry : recursive_directory_iterator(root)) {
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() != CAJETA_EXTENSION) continue;
            sources.push_back(entry.path());
        }
        std::sort(sources.begin(), sources.end());
        for (const auto& path : sources) {
            std::ifstream in(path);
            if (!in) continue;
            antlr4::ANTLRInputStream input(in);
            prescanSource(input, suppressConsole,
                          path.lexically_normal().string());
        }
    }

    list<string>* listModulePaths(string rootPath) {
        list<string>* result = new list<string>;

        using recursive_directory_iterator = std::filesystem::recursive_directory_iterator;
        std::filesystem::path sourcePath(rootPath);

        for (const auto& dirEntry: recursive_directory_iterator(sourcePath)) {
            if (dirEntry.is_regular_file()
                    && dirEntry.path().extension() == CAJETA_EXTENSION) {
                result->push_back(dirEntry.path().string());
            }
        }

        // Sorted for determinism: parse order decides synthesized-name tie-breaks,
        // first-write-wins archive keys, and when a lazy package becomes concrete.
        result->sort();

        return result;
    }


    // ANTLR's own compilationUnit() time, so a per-file cost splits into adaptive
    // prediction vs the semantic walk that follows it.
    static long long g_antlrParseNs = 0;

    // The same split for SYNTHETIC units (template instantiation and friends).
    static long long g_syntheticParseNs = 0;
    static long long g_syntheticParses = 0;

    antlr4::tree::ParseTree* parseSyntheticCompilationUnit(
            CajetaParser& parser, antlr4::CommonTokenStream& tokens,
            const std::string& what) {
        auto t0 = std::chrono::steady_clock::now();
        auto* tree = parseCompilationUnitTwoStage(
            parser, tokens,
            [&] { parser.addErrorListener(&antlr4::ConsoleErrorListener::INSTANCE); },
            what);
        g_syntheticParseNs += std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - t0).count();
        ++g_syntheticParses;
        return tree;
    }

    // Lex, two-stage parse and semantically visit one real source into `module`.
    // `label` names the parse ("user", "context", a stdlib relative path); quiet
    // suppresses ALL diagnostics. Throws SyntaxErrorException before visiting.
    static void parseSource(CajetaModulePtr module,
                            antlr4::ANTLRInputStream& input,
                            const char* label,
                            bool quiet = false) {
        // Stamp the stream with this unit's file so every AST node records its TRUE
        // origin. Synthetic re-parses build their own streams and never pass through
        // here: their positions are relative to the snippet and must not be exported.
        input.name = module->currentSourceFile();

        CajetaLexer lexer(&input);
        CommonTokenStream tokens(&lexer);
        tokens.fill();
        CajetaParser parser(&tokens);
        std::unique_ptr<JsonSyntaxErrorListener> jsonSyntax;
        // Installed by the two-stage parse on the LL retry ONLY: an SLL failure is
        // not a diagnostic, so stage 1 must report nothing.
        std::function<void()> installListeners = [&] {
            parser.removeErrorListeners();
            if (!quiet && !jsonSyntax) {
                parser.addErrorListener(&antlr4::ConsoleErrorListener::INSTANCE);
            } else if (jsonSyntax) {
                parser.addErrorListener(jsonSyntax.get());
            }
        };
        if (quiet) {
            lexer.removeErrorListeners();
            parser.removeErrorListeners();
        } else if (label && std::string(label) == "user" &&
            module->getFlags().diagFormat == DiagFormat::Json) {
            const std::string& host = module->getScriptHostName();
            jsonSyntax = std::make_unique<JsonSyntaxErrorListener>(
                host.empty() ? module->getSourcePath() : host);
            lexer.removeErrorListeners();
            parser.removeErrorListeners();
            lexer.addErrorListener(jsonSyntax.get());
            parser.addErrorListener(jsonSyntax.get());
        }
        auto psT0 = std::chrono::steady_clock::now();
        antlr4::tree::ParseTree* parseTree = parseCompilationUnitTwoStage(
            parser, tokens, installListeners, module->currentSourceFile());
        g_antlrParseNs += std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - psT0).count();
        // A syntax error leaves ANTLR's recovery tree malformed and the semantic
        // visitor segfaults on some such trees, so abort before visiting.
        size_t syntaxErrors = lexer.getNumberOfSyntaxErrors()
                            + parser.getNumberOfSyntaxErrors();
        if (syntaxErrors > 0) {
            throw SyntaxErrorException(static_cast<int>(syntaxErrors));
        }
        // A script unit is rewritten into the implicit-class form and re-parsed; the
        // wrapper is an ordinary unit, so the recursion stops after one level.
        {
            auto* unitCtx = dynamic_cast<CajetaParser::CompilationUnitContext*>(parseTree);
            if (isScriptUnit(unitCtx)) {
                std::string stem = scriptClassStem(module->getSourcePath());
                std::string canonical;
                std::vector<std::string> bindings;
                ScriptLineMap lineMap;
                bool syntheticTail = false;
                std::string wrapper =
                    synthesizeScriptUnit(tokens, unitCtx, stem, &canonical,
                                         &bindings, &lineMap, &syntheticTail);
                module->setScriptUnit(true);
                module->setScriptBindingNames(std::move(bindings));
                module->setScriptLineMap(std::move(lineMap));
                module->setScriptSyntheticTail(syntheticTail);
                antlr4::ANTLRInputStream wrapperInput(wrapper);
                parseSource(module, wrapperInput, label, quiet);
                auto& structures = module->getStructures();
                auto it = structures.find(canonical);
                if (it != structures.end() && it->second != nullptr) {
                    it->second->setScriptSynthesized(true);
                }
                return;
            }
        }
        auto prevActive = CajetaModule::getActiveModule();
        CajetaModule::setActiveModule(module);
        // RAII: the semantic visitor throws on many inputs, so the active-module
        // global is restored and the visitor freed on every exit.
        struct ActiveModuleRestore {
            decltype(prevActive) prev;
            ~ActiveModuleRestore() { CajetaModule::setActiveModule(prev); }
        } activeRestore{prevActive};
        std::unique_ptr<CajetaLlvmVisitor> visitor(
            new CajetaLlvmVisitor(module));
        parseTree->accept(visitor.get());
        if (label && label[0] != '\0') {
            const char* env = std::getenv("OUTPUT_PARSE_TREE");
            if (env && std::string(env) == "true") {
                cout << "\n\n";
                std::cout << parseTree->toStringTree(&parser, true) << std::endl;
            }
        }
    }

    // ───────────────────────────────────────────────────────────────────
    // Lazy stdlib package loading: the import hook prescans an on-demand package
    // into the archive and enqueues it; drainLazyStdlib() parses and lays it out.

    // dotted package -> indices into cajeta::stdlib::g_files. Built once;
    // g_files is constant for the process, so this survives resetGlobals.
    static const std::map<std::string, std::vector<size_t>>& stdlibPackageIndex() {
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

    // True for the stdlib packages parsed on demand rather than at prime:
    // cajeta.math and its submodules, plus the eager-prelude consumers that would
    // otherwise drag math in (cajeta.xpu.mesh, nucleo.column / .sparse / .frame).
    static bool isLazyStdlibPackage(const std::string& pkg) {
        // CAJETA_NO_LAZY_STDLIB=1 forces every package eager.
        static const bool noLazy = std::getenv("CAJETA_NO_LAZY_STDLIB") != nullptr;
        if (noLazy) return false;
        if (pkg == "cajeta.math" || pkg.rfind("cajeta.math.", 0) == 0) return true;
        if (pkg == "cajeta.xpu.mesh" || pkg.rfind("cajeta.xpu.mesh.", 0) == 0) {
            return true;
        }
        if (pkg == "cajeta.nucleo.column"
                || pkg.rfind("cajeta.nucleo.column.", 0) == 0) {
            return true;
        }
        if (pkg == "cajeta.nucleo.sparse"
                || pkg.rfind("cajeta.nucleo.sparse.", 0) == 0) {
            return true;
        }
        return pkg == "cajeta.nucleo.frame"
            || pkg.rfind("cajeta.nucleo.frame.", 0) == 0;
    }

    // The persistent stdlib-prime cache key. The digest folds every embedded stdlib
    // file plus the eager/lazy prelude split; the discriminator folds the compiler
    // version + git hash and the stdlib-codegen-affecting flag set.
    Compiler::PrimeCacheKey Compiler::stdlibPrimeCacheKey(
            const CompilerFlags& flags) {
        std::vector<std::pair<std::string, std::string>> files;
        files.reserve(cajeta::stdlib::g_fileCount);
        std::vector<std::string> lazyPkgs;
        for (size_t i = 0; i < cajeta::stdlib::g_fileCount; ++i) {
            const auto& f = cajeta::stdlib::g_files[i];
            files.emplace_back(f.relativePath,
                               std::string(f.content, f.contentBytes));
            std::string rel = f.relativePath;
            auto slash = rel.find_last_of('/');
            std::string pkg = (slash == std::string::npos)
                ? std::string() : rel.substr(0, slash);
            std::replace(pkg.begin(), pkg.end(), '/', '.');
            if (isLazyStdlibPackage(pkg)) lazyPkgs.push_back(pkg);
        }
        std::sort(lazyPkgs.begin(), lazyPkgs.end());
        lazyPkgs.erase(std::unique(lazyPkgs.begin(), lazyPkgs.end()),
                       lazyPkgs.end());
        std::string preludeTag = "lazy:";
        for (auto& p : lazyPkgs) { preludeTag += p; preludeTag += ','; }

        // The flag subset that changes the stdlib module's IR; runtime-only toggles
        // are excluded, the same set stdlibReusable() ignores.
        std::vector<std::pair<std::string, std::string>> flagSet = {
            {"bounds", std::to_string((int) flags.bounds)},
            {"nullChecks", std::to_string((int) flags.nullChecks)},
            {"overflowChecks", std::to_string((int) flags.overflowChecks)},
            {"sourceTags", flags.sourceTags ? "1" : "0"},
            {"liveSet", std::to_string((int) flags.liveSet)},
            {"ubTraps", flags.ubTraps ? "1" : "0"},
            {"useAfterMoveRt", flags.useAfterMoveRt ? "1" : "0"},
            {"opt", std::to_string((int) flags.opt)},
            {"debugInfo", flags.debugInfo ? "1" : "0"},
            {"lineInfo", flags.lineInfo ? "1" : "0"},
            {"lazyScope", flags.lazyScope ? "1" : "0"},
            {"profileCounters", flags.profileCounters ? "1" : "0"},
            {"profiler", std::to_string((int) flags.profiler)},
            {"profilerSelect", flags.profilerSelect},
        };
        PrimeCacheKey key;
        key.discriminator = std::string("jitprime-")
            + buildtool::computeCacheDiscriminator(
                  std::string(CAJETA_VERSION) + "+" + CAJETA_GIT_HASH,
                  std::move(flagSet));
        key.digest = buildtool::primeDigestOver(std::move(files), preludeTag);
        return key;
    }

    // Lazy bookkeeping, thread_local: each thread tracks the lazy packages IT has
    // parsed into its own registries.
    static thread_local std::set<std::string> g_stdlibParsedPackages;  // every pkg parsed (eager + lazy)
    static thread_local std::set<std::string> g_stdlibEagerBaseline;   // eager pkgs parsed at prime (reuse restore floor)
    static thread_local std::set<std::string> g_lazyPrescanned;        // lazy pkgs prescanned into the archive
    static thread_local std::set<std::string> g_lazyParsed;            // lazy pkgs fully parsed
    static thread_local std::vector<std::string> g_lazyQueue;          // lazy pkgs awaiting full parse
    // Lazy pkgs named by an `import` anywhere under the source root, collected
    // during the prescan sweep. See drainPrescannedLazyStdlib.
    static thread_local std::set<std::string> g_prescanLazyImports;

    // Clear every lazy-stdlib registry for a fresh Compiler.
    static void resetLazyStdlibStateImpl() {
        g_prescanLazyImports.clear();
        g_stdlibParsedPackages.clear();
        g_lazyPrescanned.clear();
        g_lazyParsed.clear();
        g_lazyQueue.clear();
    }

    // Prescan every embedded file of `pkg` into the archive. Idempotent.
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
        // Lazy-package DEPENDENCIES that must be concrete BEFORE this package parses.
        // The drain pops LIFO, so a dep pushed after drains first: nucleo.column
        // declares FIELDS of cajeta.math types, and field types resolve at parse.
        if (pkg == "cajeta.nucleo.column"
                || pkg.rfind("cajeta.nucleo.column.", 0) == 0) {
            noteStdlibImportImpl("cajeta.math");
        }
    }

    // Re-entrancy guard: the drain parses stdlib sources, whose own parses fire the
    // drain hook. The outer loop consumes the whole queue regardless, so an inner
    // call is always safe to skip.
    static thread_local bool g_draining = false;

    // Fully parse every enqueued lazy package into the stdlib module, then lay them
    // out. Queue-guarded and idempotent — a cheap no-op when the queue is empty.
    static void drainLazyStdlib() {
        if (g_draining) return;
        if (g_lazyQueue.empty()) return;
        auto stdlib = CajetaModule::getStdlibModule();
        if (!stdlib) { g_lazyQueue.clear(); return; }
        g_draining = true;
        struct DrainGuard {
            ~DrainGuard() { g_draining = false; }
        } drainGuard;
        auto prevActive = CajetaModule::getActiveModule();
        CajetaModule::setActiveModule(stdlib);
        QualifiedNamePtr originalQName = stdlib->getQName();
        bool parsedAny = false;
        using DClock = std::chrono::steady_clock;
        const bool dTiming = std::getenv("CAJETA_PRIME_TIMING") != nullptr;
        long long dPrescanNs = 0, dParseNs = 0, dProtoNs = 0;
        int dPkgs = 0, dFiles = 0;
        std::vector<std::pair<long long, std::string>> dPerFile;
        auto dAdd = [](long long& acc, DClock::time_point a) {
            acc += std::chrono::duration_cast<std::chrono::nanoseconds>(
                DClock::now() - a).count();
        };
        while (!g_lazyQueue.empty()) {
            std::string pkg = g_lazyQueue.back();
            g_lazyQueue.pop_back();
            if (g_lazyParsed.count(pkg)) continue;
            // Reuse-cache hazard gate (test-only): parsing novel lazy content appends
            // classes to the cached stdlib module that the reuse path's codegen loop
            // excludes, so abort BEFORE parsing and let the harness retry fresh.
            if (Compiler::isReuseHazardArmed()) {
                throw cajeta::ReuseHazardAbort{};
            }
            // Marked parsed BEFORE the walk: a body referencing one of its own lazy
            // types re-fires the import hook, which the guard above then dedupes.
            g_lazyParsed.insert(pkg);
            auto dT = DClock::now();
            prescanStdlibPackage(pkg);   // whole-package archive (idempotent)
            dAdd(dPrescanNs, dT);
            ++dPkgs;
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
                stdlib->setCurrentSourceFile(rel);   // see parseStdlibInto
                antlr4::ANTLRInputStream in(
                    std::string(f.content, f.contentBytes));
                auto dP = DClock::now();
                const long long antlrBefore = g_antlrParseNs;
                const long long synthBefore = g_syntheticParseNs;
                const long long synthCountBefore = g_syntheticParses;
                parseSource(stdlib, in, /*label=*/rel.c_str());
                if (dTiming) {
                    long long totMs =
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            DClock::now() - dP).count();
                    long long antlrMs = (g_antlrParseNs - antlrBefore) / 1000000;
                    long long synthMs = (g_syntheticParseNs - synthBefore) / 1000000;
                    dPerFile.emplace_back(
                        totMs, rel + "  (antlr " + std::to_string(antlrMs)
                                   + " ms, synth " + std::to_string(synthMs)
                                   + " ms x" + std::to_string(
                                         g_syntheticParses - synthCountBefore)
                                   + ", walk " + std::to_string(totMs - antlrMs - synthMs)
                                   + " ms)");
                }
                dAdd(dParseNs, dP);
                ++dFiles;
            }
            g_stdlibParsedPackages.insert(pkg);
            parsedAny = true;
        }
        stdlib->setCurrentSourceFile("");
        stdlib->setQName(originalQName);
        if (parsedAny) {
            auto dB = DClock::now();
            CajetaModule::buildPendingPrototypes();
            dAdd(dProtoNs, dB);
        }
        if (dTiming) {
            std::fprintf(stderr,
                "[drain] %d packages / %d files: prescan %lld ms, "
                "parse %lld ms, buildPendingPrototypes %lld ms\n",
                dPkgs, dFiles, dPrescanNs / 1000000, dParseNs / 1000000,
                dProtoNs / 1000000);
            std::sort(dPerFile.begin(), dPerFile.end(),
                      [](const auto& a, const auto& b) { return b < a; });
            for (size_t i = 0; i < dPerFile.size() && i < 12; ++i) {
                std::fprintf(stderr, "[drain] %9lld ms  %s\n",
                             dPerFile[i].first, dPerFile[i].second.c_str());
            }
        }
        CajetaModule::setActiveModule(prevActive);
    }

    // Note a lazy stdlib package named by an import seen during the prescan sweep.
    static void notePrescannedImport(const std::string& pkg) {
        if (isLazyStdlibPackage(pkg)) g_prescanLazyImports.insert(pkg);
    }

    // Make every on-demand stdlib package the compile unit set imports CONCRETE
    // before any body walk begins. The import hook alone drains at the END of the
    // importing unit's parse — too late for a field typed `ArrayList<Tensor<E>>`.
    static void drainPrescannedLazyStdlib() {
        if (g_prescanLazyImports.empty()) return;
        for (const auto& pkg : g_prescanLazyImports) {
            noteStdlibImportImpl(pkg);
        }
        drainLazyStdlib();
    }

    // Parse every eagerly-loaded embedded stdlib file into `module`, once per
    // Compiler: prescan all names into the archive first, then walk each file with
    // the module's qName swapped to that file's package. Links the runtime bitcode.
    void parseStdlibInto(CajetaModulePtr module) {
        // Lazy-load bookkeeping is process-global; reset it for this Compiler.
        resetLazyStdlibStateImpl();

        auto packageOf = [](size_t i) {
            std::string rel = cajeta::stdlib::g_files[i].relativePath;
            auto slash = rel.find_last_of('/');
            std::string pkg = (slash == std::string::npos)
                ? std::string() : rel.substr(0, slash);
            std::replace(pkg.begin(), pkg.end(), '/', '.');
            return pkg;
        };

        const bool primeTiming = std::getenv("CAJETA_PRIME_TIMING") != nullptr;
        using PrimeClock = std::chrono::steady_clock;
        std::vector<std::pair<long long, std::string>> prescanMs, parseMs;
        auto stamp = [&](PrimeClock::time_point t0,
                         std::vector<std::pair<long long, std::string>>& into,
                         const char* path) {
            if (!primeTiming) return;
            into.emplace_back(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    PrimeClock::now() - t0).count(), path);
        };
        auto report = [&](const char* label,
                          std::vector<std::pair<long long, std::string>>& v) {
            if (!primeTiming) return;
            long long total = 0;
            for (auto& e : v) total += e.first;
            std::sort(v.begin(), v.end(),
                      [](auto& a, auto& b) { return a.first > b.first; });
            std::fprintf(stderr, "[prime] %s: %lld ms over %zu files\n",
                         label, total, v.size());
            for (size_t i = 0; i < v.size() && i < 10; ++i) {
                std::fprintf(stderr, "[prime]     %6lld ms  %s\n",
                             v[i].first, v[i].second.c_str());
            }
        };

        for (size_t i = 0; i < cajeta::stdlib::g_fileCount; ++i) {
            if (isLazyStdlibPackage(packageOf(i))) continue;  // on-demand only
            const auto& f = cajeta::stdlib::g_files[i];
            auto t0 = PrimeClock::now();
            antlr4::ANTLRInputStream prescanIn(
                std::string(f.content, f.contentBytes));
            prescanSource(prescanIn);
            stamp(t0, prescanMs, f.relativePath);
        }
        report("prescan loop", prescanMs);

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
            // Each class records its declaring file; the module's source path is empty
            // (the whole stdlib is one module), so stamp it per file instead.
            module->setCurrentSourceFile(relPath);
            auto t0 = PrimeClock::now();
            antlr4::ANTLRInputStream stdlibInput(
                std::string(f.content, f.contentBytes));
            parseSource(module, stdlibInput, /*label=*/"");
            stamp(t0, parseMs, f.relativePath);
            g_stdlibParsedPackages.insert(pkg);
        }
        report("parse loop", parseMs);
        module->setCurrentSourceFile("");
        module->setQName(originalQName);

        // Snapshot the eager set: a reuse shard rolls back only lazy packages, and
        // resetLazyStdlibState restores the record to this floor.
        g_stdlibEagerBaseline = g_stdlibParsedPackages;

        // Stdlib code calls runtime helpers, so embed the runtime bitcode here.
        module->linkRuntime();
    }

    // Parse the user source of `module`, then fully parse any on-demand stdlib
    // package it pulled in. Stdlib classes are already in canonicalMap from the
    // Compiler's one-shot stdlib parse, so references resolve to real classes.
    void parse(CajetaModulePtr module) {
        ifstream stream;
        stream.open(module->getSourcePath());
        stream.seekg(0);
        antlr4::ANTLRInputStream userInput(stream);
        parseSource(module, userInput, /*label=*/"user");
        drainLazyStdlib();
    }

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

    // Emit a global ctor publishing flags.stackTraceCapture to the runtime. Per
    // USER module, not the stdlib: ensureStdlibModule early-returns on the reuse
    // path, so a flag-dependent ctor there bakes in the first compile's value.
    void emitStackTraceCaptureInit(CajetaModulePtr module) {
        llvm::Module* lmod = module->getLlvmModule();
        if (!lmod) return;
        const char* ctorName = "__cajeta_init_stack_trace_capture";
        if (lmod->getFunction(ctorName)) return;   // re-entry guard

        auto& llvmCtx = *module->getLlvmContext();
        llvm::Type* i32Ty = llvm::Type::getInt32Ty(llvmCtx);
        llvm::FunctionType* setterTy = llvm::FunctionType::get(
            llvm::Type::getVoidTy(llvmCtx), {i32Ty}, /*isVarArg=*/false);
        llvm::FunctionCallee setter = lmod->getOrInsertFunction(
            "__cajeta_set_stack_trace_capture", setterTy);

        llvm::FunctionType* ctorTy = llvm::FunctionType::get(
            llvm::Type::getVoidTy(llvmCtx), /*isVarArg=*/false);
        llvm::Function* ctor = llvm::Function::Create(
            ctorTy, llvm::Function::PrivateLinkage, ctorName, lmod);
        llvm::BasicBlock* bb = llvm::BasicBlock::Create(llvmCtx, "entry", ctor);
        llvm::IRBuilder<> b(bb);
        b.CreateCall(setter, {llvm::ConstantInt::get(
            i32Ty, module->getFlags().stackTraceCapture ? 1 : 0)});
        b.CreateRetVoid();

        llvm::appendToGlobalCtors(*lmod, ctor, /*Priority=*/65535);
    }

    // Create a CajetaModule for `sourcePath` under the given source / target roots,
    // carrying this Compiler's flags and session state. Throws FileNotFoundException
    // when the source does not exist.
    CajetaModulePtr Compiler::createModule(string sourcePath, string sourceRootPath, string targetRootPath) {
        if (!fileExists(sourcePath))
            throw FileNotFoundException(sourcePath);

        // Remember the roots so materializeUserClass can create sibling modules with
        // the same layout mapping.
        lastSourceRoot = sourceRootPath;
        lastTargetRoot = targetRootPath;

        auto module = make_shared<CajetaModule>(activeContext,
            sourcePath,
            sourceRootPath,
            targetRootPath,
            targetTriple,
            targetMachine);
        module->setFlags(flags);
        module->setSessionState(sessionState);
        module->setScriptHostName(sessionHostName);
        // Reproducible builds: scrub the absolute source path the constructor
        // embedded, now that flags (with --debug-prefix-map) are set.
        module->canonicalizeSourceFileName();
        emitStackTraceCaptureInit(module);
        return module;
    }

    // Read every classpath archive and re-parse each ClassSource entry into a fresh
    // module registered in the canonical-name map. Those modules live in
    // `externalModules`, and the emitter never writes their IR out.
    void Compiler::ingestClasspath() {
        if (classpath.empty()) return;
        const bool cpTiming = std::getenv("CAJETA_PRIME_TIMING") != nullptr;
        auto cpStart = std::chrono::steady_clock::now();
        auto cpMark = cpStart;
        auto cpPhase = [&](const char* name) {
            if (!cpTiming) return;
            auto now = std::chrono::steady_clock::now();
            auto ms = [](auto d) {
                return std::chrono::duration_cast<std::chrono::milliseconds>(d)
                    .count();
            };
            std::fprintf(stderr, "[ingest] %-26s %7lld ms   (cumulative %lld ms)\n",
                         name, (long long) ms(now - cpMark),
                         (long long) ms(now - cpStart));
            cpMark = now;
        };

        // Phase 1 — prescan every classpath class's canonical name, so forward refs
        // from one archive into another work as in the user-source prescan.
        for (const auto& cpPath : classpath) {
            try {
                auto arc = CajetaArchive::readFrom(cpPath);
                // Merge the dep's reflection-keep summary: its sites live in bodies
                // this compile never re-codegens, so without the merge a lean link
                // silently strips the classes the dep enumerates at runtime.
                if (const auto* sum =
                        arc.findEntry("meta/reflection-keep.v1")) {
                    std::string text(
                        (const char*) sum->data.data(), sum->data.size());
                    std::istringstream lines(text);
                    std::string line;
                    auto& keep = CajetaModule::reflectionKeep();
                    using RS = CajetaModule::ReflSite;
                    while (std::getline(lines, line)) {
                        auto sp = line.find(' ');
                        if (sp == std::string::npos) continue;
                        std::string tag = line.substr(0, sp);
                        std::string sel = line.substr(sp + 1);
                        if (tag == "forceall") {
                            CajetaModule::noteForceAll(
                                "dependency " + arc.getName() + ": " + sel);
                        } else if (tag == "bound") {
                            keep.sites.push_back({RS::BoundClosure, sel});
                        } else if (tag == "forname") {
                            keep.sites.push_back({RS::ForNameLiteral, sel});
                        } else if (tag == "package") {
                            keep.sites.push_back({RS::PackageLiteral, sel});
                        } else if (tag == "annotated") {
                            keep.sites.push_back({RS::Annotated, sel});
                        } else if (tag == "methodannotated") {
                            keep.sites.push_back({RS::MethodAnnotated, sel});
                        }
                    }
                }
                for (const auto& entry : arc.getEntries()) {
                    if (entry.kindTag != CajetaArchive::EntryKind::ClassSource)
                        continue;
                    std::string text(
                        (const char*) entry.data.data(), entry.data.size());
                    antlr4::ANTLRInputStream input(text);
                    prescanSource(input);
                }
            } catch (const std::exception& e) {
                logLine("error", "cajeta: --classpath read failed for `"
                                 + cpPath + "`: " + e.what() + "\n");
                throw;
            }
        }

        cpPhase("prescan dep sources");
        drainPrescannedLazyStdlib();
        cpPhase("drain lazy stdlib");

        // Phase 2 - full parse. Each ClassSource entry becomes a standalone module
        // whose LLVM module is a throwaway: the archive's own `.bc` is authoritative.
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
                extMod->setCurrentSourceFile(entryName);
                // ...and its IDENTITY: the JIT's debug loc-id ranges key on
                // remappedSourcePath(), which reads sourcePath — left EMPTY by the
                // synthetic ctor, so every dependency hashed to the same slot.
                extMod->setSourcePath(entryName);
                extMod->setClasspathOrigin(true);
                // A dependency's objects get their own `deps/<module>/` subtree.
                // Keyed on the FILE STEM with a trailing `-<version>` trimmed: a
                // library archive's internal name is the placeholder "cajeta-archive".
                {
                    std::string depModule =
                        std::filesystem::path(cpPath).stem().string();
                    auto dash = depModule.rfind('-');
                    if (dash != std::string::npos && dash + 1 < depModule.size()
                        && std::isdigit(
                               static_cast<unsigned char>(depModule[dash + 1]))) {
                        depModule.resize(dash);
                    }
                    if (depModule.empty()) depModule = "unnamed";
                    extMod->setArchivePath("deps/" + depModule + "/"
                                           + qName->toCanonical()
                                           + CAJETA_IR_EXTENSION);
                }
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

        // Lay out every parsed classpath class so its methods become resolvable
        // Functions user codegen can wire to; without this, calls to them come back
        // as a null llvm::Value and the return is lowered to null.
        cpPhase("parse dep sources");
        CajetaModule::buildPendingPrototypes();
        cpPhase("buildPendingPrototypes");
    }

    CajetaModulePtr Compiler::ensureStdlibModule() {
        // Install the on-demand import hook unconditionally: the reuse path
        // early-returns below and still needs it bound.
        CajetaModule::stdlibImportHook = [](const std::string& pkg) {
            noteStdlibImportImpl(pkg);
        };
        // On-demand USER-class materialization: lets a synthesizer force a cross-file
        // class's declaring module to compile. Rebound per Compiler, like the hook above.
        CajetaModule::userMaterializeHook = [this](const std::string& canonical) {
            return this->materializeUserClass(canonical);
        };

        auto existing = CajetaModule::getStdlibModule();
        if (existing) return existing;

        const bool primeTiming = std::getenv("CAJETA_PRIME_TIMING") != nullptr;
        auto primeClock = std::chrono::steady_clock::now();
        auto primeStart = primeClock;
        auto primePhase = [&](const char* name) {
            if (!primeTiming) return;
            auto now = std::chrono::steady_clock::now();
            auto ms = [](auto d) {
                return std::chrono::duration_cast<std::chrono::milliseconds>(d)
                    .count();
            };
            std::fprintf(stderr, "[prime] %-28s %7lld ms   (cumulative %lld ms)\n",
                         name, (long long) ms(now - primeClock),
                         (long long) ms(now - primeStart));
            primeClock = now;
        };

        auto stdlibQName = QualifiedName::getOrInsert(
            "__stdlib__", "cajeta.runtime");
        auto stdlib = make_shared<CajetaModule>(
            activeContext, stdlibQName, targetTriple, targetMachine);
        stdlib->setFlags(flags);
        CajetaModule::setStdlibModule(stdlib);
        modules.push_back(stdlib);

        auto prevActive = CajetaModule::getActiveModule();
        CajetaModule::setActiveModule(stdlib);
        primePhase("setup");
        parseStdlibInto(stdlib);
        primePhase("parse (444 sources)");
        CajetaModule::setActiveModule(prevActive);

        // Lay out every parsed stdlib class so its vtable / RTTI globals live here.
        CajetaModule::buildPendingPrototypes();
        primePhase("buildPendingPrototypes");

        // cajeta.reflect.Class is a template: force the canonical Class<?> here, before
        // any user module parses - nothing else pulls the reflect types into the map.
        CajetaModule::setActiveModule(stdlib);
        CajetaClass::ensureClassWildcardInstantiated();
        CajetaModule::buildPendingPrototypes();
        primePhase("Class<?> instantiation");
        CajetaModule::setActiveModule(prevActive);

        // An EAGER stdlib class can reference a LAZY type, which the import hook only
        // prescans + enqueues. The stdlib-reuse prime goes straight to codegen, so
        // drain here or validatePlaceholders throws. No-op when the queue is empty.
        drainLazyStdlib();
        primePhase("drainLazyStdlib");

        emitUnrecoverableMarker(stdlib);
        primePhase("emitUnrecoverableMarker");
        return stdlib;
    }

    bool Compiler::stdlibPackageParsed(const std::string& pkg) {
        return g_stdlibParsedPackages.count(pkg) != 0;
    }

    const std::set<std::string>& Compiler::stdlibParsedPackages() {
        return g_stdlibParsedPackages;
    }

    void Compiler::resetLazyStdlibState() {
        // Lazy packages were rolled back from the cached stdlib module, but eager
        // ones remain parsed: restore the record to the eager floor rather than
        // clearing it, so the parsed-package probe stays truthful.
        g_lazyPrescanned.clear();
        g_lazyParsed.clear();
        g_lazyQueue.clear();
        g_stdlibParsedPackages = g_stdlibEagerBaseline;
    }

    Compiler::LazyStdlibState Compiler::captureLazyStdlibState() {
        LazyStdlibState s;
        s.parsedPackages = g_stdlibParsedPackages;
        s.prescanned = g_lazyPrescanned;
        s.parsed = g_lazyParsed;
        s.queue = g_lazyQueue;
        return s;
    }

    void Compiler::restoreLazyStdlibState(const LazyStdlibState& s) {
        g_stdlibParsedPackages = s.parsedPackages;
        g_lazyPrescanned = s.prescanned;
        g_lazyParsed = s.parsed;
        g_lazyQueue = s.queue;
    }

    // Parse and prototype ONE module: ensure the stdlib exists, parse the source,
    // then drive the deferred-prototype sweep. Skips a file materializeUserClass
    // already force-compiled, and defers the sweep while one is in flight.
    void Compiler::compile(CajetaModulePtr module) {
        // Skip ONLY files materializeUserClass already force-compiled: the driver
        // loop's later visit would redeclare their classes. Scoped to materialized
        // paths — the lint server's warm resweep legitimately re-compiles siblings.
        std::string normPath = std::filesystem::path(
            module->getSourcePath()).lexically_normal().string();
        if (getenv("CAJETA_DBG_MATERIALIZE")) {
            std::cerr << "[compile] " << normPath
                      << " (inflight=" << materializeInFlight.size()
                      << ", materialized=" << materializedSourcePaths.size()
                      << ")\n";
        }
        if (materializedSourcePaths.count(normPath)
                && !materializeInFlight.count(normPath)) {
            if (getenv("CAJETA_DBG_MATERIALIZE")) {
                std::cerr << "[compile] skip (already materialized)\n";
            }
            return;
        }
        ensureStdlibModule();
        modules.push_back(module);
        parse(module);
        // Idempotent sweep, so callers using this entry directly need not remember
        // it. NOT during an on-demand materialization: the nested compile runs
        // mid-walk and would prototype the requesting class while it is still EMPTY.
        if (materializeInFlight.empty()) {
            CajetaModule::buildPendingPrototypes();
        }
    }

    bool Compiler::materializeUserClass(const std::string& canonical) {
        std::string path = CajetaType::lookupArchiveSourcePath(canonical);
        const bool dbg = getenv("CAJETA_DBG_MATERIALIZE") != nullptr;
        if (path.empty()) {
            if (dbg) {
                std::cerr << "[materialize-user] " << canonical
                          << ": no recorded source path — skip\n";
            }
            return false;                        // not an on-disk user class
        }
        std::string normPath =
            std::filesystem::path(path).lexically_normal().string();
        if (materializedSourcePaths.count(normPath)) {
            if (dbg) {
                std::cerr << "[materialize-user] " << canonical
                          << ": already materialized — skip\n";
            }
            return false;
        }
        // Cycle guard: two records whose Tables reference each other degrade
        // to the placeholder behavior instead of recursing forever.
        if (!materializeInFlight.insert(normPath).second) return false;
        if (getenv("CAJETA_DBG_MATERIALIZE")) {
            std::cerr << "[materialize-user] " << canonical
                      << " <- " << normPath << "\n";
        }
        try {
            CajetaModulePtr m = createModule(normPath, lastSourceRoot,
                                             lastTargetRoot);
            compile(m);
        } catch (...) {
            materializeInFlight.erase(normPath);
            throw;   // the declaring file's own errors are real — report them
        }
        materializeInFlight.erase(normPath);
        materializedSourcePaths.insert(normPath);
        return true;
    }

    void Compiler::lint(const string& file, const string& sourceRoot,
                        const string& shadow, bool skipContextRegistration,
                        const std::function<void()>& afterContextRegistration) {

        // xref: armed BEFORE the stdlib parse so every AST node interns its source.
        // Call edges and field references are captured for the TARGET's bodies only.
        xref::resetCapture();
        xref::setCaptureEnabled(!flags.emitXref.empty());

        ensureStdlibModule();

        // Ingest --classpath so a single-file lint resolves dependency types exactly
        // as the whole-root export does. Gated by skipContextRegistration: a warm
        // restore already holds these, and re-ingest makes the dep's @Inject ambiguous.
        if (!skipContextRegistration) ingestClasspath();

        const bool json = getFlags().diagFormat == DiagFormat::Json;

        std::filesystem::path targetPath(file);
        string dir = targetPath.has_parent_path()
            ? targetPath.parent_path().string() : ".";
        if (dir.empty() || dir.back() != '/') dir.append("/");

        if (!sourceRoot.empty() && !skipContextRegistration) {
            registerLintContext(sourceRoot, file, shadow, json);
            if (afterContextRegistration) afterContextRegistration();
        }

        {
            std::ifstream in(file);
            if (in) {
                antlr4::ANTLRInputStream input(in);
                prescanSource(input, json);
            }
        }

        CajetaModulePtr module = createModule(file, dir, dir);

        // FQNs derive from the module's PATH-derived package, so a module rooted at
        // its own directory would declare `Target` where a real compile declares
        // `demo.Target`. Re-derive from the ORIGINAL path relative to --source-root.
        if (!sourceRoot.empty()) {
            string rootSlash = sourceRoot;
            if (rootSlash.back() != '/') rootSlash.append("/");
            string original = !shadow.empty() ? shadow : file;
            if (original.rfind(rootSlash, 0) == 0) {
                string rel = original.substr(rootSlash.size());
                auto dot = rel.rfind(CAJETA_EXTENSION);
                if (dot != string::npos) rel = rel.substr(0, dot);
                auto slash = rel.find_last_of('/');
                string cls = slash == string::npos ? rel : rel.substr(slash + 1);
                string pkg = slash == string::npos ? "" : rel.substr(0, slash);
                std::replace(pkg.begin(), pkg.end(), '/', '.');
                module->setQName(QualifiedName::getOrInsert(cls, pkg));
            }
        }

        compile(module);  // parse (target diagnostics) + buildPendingPrototypes

        CajetaModule::validatePlaceholders();
        CajetaModule::buildPendingPrototypes();
        CajetaModule::resolveAdviceMatches();
        CajetaModule::resolveDependencyGraph();

        // Resolve the TARGET module's bodies so a per-edit shard carries the same
        // records as the whole-root export. Per method, best-effort: mid-edit is normal.
        for (auto& [_, klass] : module->getStructures()) {
            if (!klass) continue;
            for (auto& [__, method] : klass->getMethods()) {
                if (!method) continue;
                try {
                    method->resolveBodyForLint(module);
                } catch (cajeta::Exception& e) {
                    if (json)
                        emitJsonDiagnostic("error", e.getErrorId(), e.getMessage());
                    else
                        std::cerr << "cajeta: body-resolve: " << e.getMessage() << "\n";
                } catch (const std::exception& e) {
                    if (json)
                        emitJsonDiagnostic("error", "", e.what());
                    else
                        std::cerr << "cajeta: body-resolve: " << e.what() << "\n";
                }
            }
        }

        xref::captureStaticReceivers(module);
    }

    // --source-root: register every sibling `.cajeta` under `root` (except the
    // target and any --shadow twin) for its SIGNATURES only — parsed quietly into
    // `externalModules`, each in its own try/catch, and never emitted.
    void Compiler::registerLintContext(const string& root, const string& file,
                                       const string& shadow, bool json) {
        namespace fs = std::filesystem;
        // Context files are parsed for signatures only: install a suppressed engine
        // so a sibling's semantic error never reports into the target's.
        struct EngineGuard {
            DiagnosticEngine* prev;
            explicit EngineGuard(DiagnosticEngine* e)
                : prev(DiagnosticEngine::active()) { DiagnosticEngine::setActive(e); }
            ~EngineGuard() { DiagnosticEngine::setActive(prev); }
        };
        DiagnosticEngine suppressed(/*suppressed=*/true);
        EngineGuard guard(&suppressed);

        prescanSourceRoot(root, json);

        auto canon = [](const string& p) {
            std::error_code ec; auto c = fs::weakly_canonical(p, ec);
            return ec ? fs::path(p) : c;
        };
        fs::path targetCanon = canon(file);
        fs::path shadowCanon = shadow.empty() ? fs::path() : canon(shadow);

        string rootSlash = root;
        if (rootSlash.empty() || rootSlash.back() != '/') rootSlash.append("/");

        std::error_code ec;
        for (fs::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file()) continue;
            if (it->path().extension() != CAJETA_EXTENSION) continue;
            fs::path here = canon(it->path().string());
            if (here == targetCanon) continue;                 // parsed as the target
            if (!shadow.empty() && here == shadowCanon) continue;  // replaced by the buffer
            try {
                std::ifstream in(it->path());
                if (!in) continue;
                antlr4::ANTLRInputStream input(in);
                CajetaModulePtr sib = createModule(it->path().string(), rootSlash, rootSlash);
                externalModules.push_back(sib);
                auto prev = CajetaModule::getActiveModule();
                parseSource(sib, input, /*label=*/"context", /*quiet=*/true);
                CajetaModule::setActiveModule(prev);
            } catch (...) {
                // Broken sibling: contributes no (or partial) signatures. Skip.
            }
        }
        CajetaModule::buildPendingPrototypes();
    }

    string Compiler::computeOwnCacheDiscriminator() const {
        const char* emitName =
            emitMode == EmitMode::IR ? "ir"
            : emitMode == EmitMode::Obj ? "obj"
            : emitMode == EmitMode::Cja ? "cja"
            : emitMode == EmitMode::Uber ? "uber" : "exe";
        auto pairs = cacheFlagPairs(flags, emitName, targetTriple);
        // @Profile gating changes which components codegen at all.
        pairs.emplace_back("profile", CajetaModule::getActiveProfile());
        pairs.emplace_back("cpu", cpu);
        pairs.emplace_back("features", features);
        // --xpu-backend and --xpu-arch decide which device kernels are compiled and
        // embedded, and neither reached this key. Order-normalised: the flag takes a set.
        {
            std::vector<std::string> names;
            names.reserve(xpuBackends.size());
            for (XpuBackend b : xpuBackends) {
                switch (b) {
                    case XpuBackend::Nvptx:  names.emplace_back("nvptx");  break;
                    case XpuBackend::Amdgpu: names.emplace_back("amdgpu"); break;
                    case XpuBackend::Vulkan: names.emplace_back("vulkan"); break;
                    case XpuBackend::Cpu:    names.emplace_back("cpu");    break;
                    case XpuBackend::None:   break;
                }
            }
            std::sort(names.begin(), names.end());
            names.erase(std::unique(names.begin(), names.end()), names.end());
            std::string joined;
            for (const auto& n : names) {
                if (!joined.empty()) joined += ',';
                joined += n;
            }
            // §2.3 — absent is NOT "cpu". A host-only build embeds no device
            // code at all; a cpu build embeds CPU kernels. Spelling the empty
            // case as the cpu string would keep exactly the alias this fixes.
            pairs.emplace_back("xpu-backend", joined.empty() ? "<none>" : joined);
            // The arch, keyed the way codegen reads it: emitXpuKernels honors
            // --xpu-arch only when ONE backend is selected, so with a bundle the flag
            // changes nothing and keying it would force needless rebuilds.
            pairs.emplace_back("xpu-arch",
                names.size() == 1 ? xpuArch : "<per-backend-defaults>");
        }
        // A changed classpath archive changes user-module IR without touching any
        // source digest — fold each archive's CONTENT in (coarse, sound).
        for (const auto& cp : classpath) {
            std::ifstream in(cp, std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            pairs.emplace_back("classpath:" + cp,
                               buildtool::sha256Hex(ss.str()));
        }
        // Fold the git hash in: two dev builds of the same VERSION can differ
        // in codegen, and reusing IR across them would be a silent miscompile.
        return buildtool::computeCacheDiscriminator(
            CAJETA_VERSION "+" CAJETA_GIT_HASH, pairs);
    }

    bool Compiler::setupCacheManifest() {
        cacheManifest.reset();
        if (cacheManifestPath.empty()) return true;

        auto loaded = CacheManifest::load(cacheManifestPath);
        if (!loaded) {
            // Malformed manifest = the caller's contract is broken; silently
            // proceeding would produce an artifact the caller mis-keys.
            throw std::runtime_error(llvm::toString(loaded.takeError()));
        }

        // Tree-shake RTA and the lean keep-set both derive from LIVE codegen, which
        // clean modules do not produce, so force them off. The forced values feed the
        // discriminator, so manifest builds key their own consistent cache tree.
        flags.treeShake = TreeShake::Off;
        flags.linkMode = LinkMode::Full;

        string own = computeOwnCacheDiscriminator();
        cout << "[incremental] discriminator " << own << "\n";

        if (!loaded->populateMode() && loaded->discriminator != own) {
            cerr << "cajeta: [incremental] cache-manifest discriminator "
                    "mismatch (manifest " << loaded->discriminator
                 << " != compiler " << own
                 << ") — ignoring cache, full rebuild\n";
            return true;   // manifest dropped; plain full build
        }
        cacheManifest = std::move(*loaded);
        return true;
    }

    // Collect every declaration, inheritance edge, call and reference the compiler
    // holds resolved and write the xref index to `path`. Emitted BEFORE tree-shaking:
    // a developer still navigates code this particular build happens not to call.
    static void writeXrefIndex(const std::string& path, const std::string& sourceRootPath) {
        xref::XrefIndex index;
        xref::collectDeclarationsAndInheritance(index, sourceRootPath);
        xref::drainCalls(index, sourceRootPath);
        xref::drainReferences(index, sourceRootPath);
        // Invariant: every edge endpoint names a declaration this index carries.
        // Anything else is a Ctrl-click into the void.
        index.pruneDanglingEdges();
        if (!index.writeToFile(path)) {
            std::cerr << "cajeta: warning — could not write xref index to "
                      << path << std::endl;
        }
    }

    int Compiler::lintRoot(const string& root) {
        xref::resetCapture();
        xref::setCaptureEnabled(!flags.emitXref.empty());

        ensureStdlibModule();
        ingestClasspath();
        const bool json = getFlags().diagFormat == DiagFormat::Json;
        prescanSourceRoot(root, json);

        string rootSlash = root;
        if (rootSlash.empty() || rootSlash.back() != '/') rootSlash.append("/");

        // Sorted for determinism: parse order decides synthesized-name tie-breaks,
        // so the same input must mean the same PARSE, not just the same sort.
        namespace fs = std::filesystem;
        std::vector<std::string> files;
        std::error_code ec;
        for (fs::recursive_directory_iterator it(root, ec), end; it != end;
             it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file()) continue;
            if (it->path().extension() != CAJETA_EXTENSION) continue;
            files.push_back(it->path().string());
        }
        std::sort(files.begin(), files.end());

        // One broken file must not sink the other N-1; errors still surface on the
        // diagnostic channel, the export just is not hostage to them.
        int failed = 0;
        for (const auto& path : files) {
            try {
                CajetaModulePtr module = createModule(path, rootSlash, rootSlash);
                compile(module);
            } catch (SyntaxErrorException&) {
                ++failed;              // diagnostics already emitted by the parse
            } catch (cajeta::Exception& e) {
                ++failed;
                if (json) emitJsonDiagnostic("error", e.getErrorId(),
                                             e.getMessage(), path);
                else std::cerr << "cajeta: " << path << ": " << e.getMessage()
                               << "\n";
            } catch (const std::exception& e) {
                ++failed;
                if (json) emitJsonDiagnostic("error", "", e.what(), path);
                else std::cerr << "cajeta: " << path << ": " << e.what() << "\n";
            }
        }

        auto guarded = [&](const char* pass, auto fn) {
            try {
                fn();
            } catch (cajeta::Exception& e) {
                ++failed;
                if (json) emitJsonDiagnostic("error", e.getErrorId(), e.getMessage());
                else std::cerr << "cajeta: " << pass << ": " << e.getMessage() << "\n";
            } catch (const std::exception& e) {
                ++failed;
                if (json) emitJsonDiagnostic("error", "", e.what());
                else std::cerr << "cajeta: " << pass << ": " << e.what() << "\n";
            }
        };
        guarded("validate", [] { CajetaModule::validatePlaceholders(); });
        guarded("prototypes", [] { CajetaModule::buildPendingPrototypes(); });
        guarded("advice", [] { CajetaModule::resolveAdviceMatches(); });
        guarded("dependencies", [] { CajetaModule::resolveDependencyGraph(); });

        // Resolve method BODIES: field references and call edges are recorded there, and
        // used to be reachable only from codegen. After the four passes above.
        auto stdlib = CajetaModule::getStdlibModule();
        for (auto& m : modules) {
            if (!m || m == stdlib) continue;
            for (auto& [_, klass] : m->getStructures()) {
                if (!klass) continue;
                for (auto& [__, method] : klass->getMethods()) {
                    if (!method) continue;
                    guarded("body-resolve", [&] { method->resolveBodyForLint(m); });
                }
            }
        }

        // Static method-call / field-access receivers resolve in codegen, which the
        // export stops before - record the receiver's TYPE reference here, scope-aware.
        for (auto& m : modules) {
            if (m && m != stdlib) guarded("static-receivers",
                [&] { xref::captureStaticReceivers(m); });
        }

        writeXrefIndex(flags.emitXref, rootSlash);
        return failed;
    }

    void Compiler::emitLintXrefStream(const string& file, const string& sourceRoot,
                                      const string& shadow) {
        if (flags.emitXref != "-") return;   // stream not requested

        // The linted file's records carry the name its MODULE gave them, computed here
        // exactly as lint()'s createModule did, so the filter cannot drift from capture.
        std::filesystem::path targetPath(file);
        string dir = targetPath.has_parent_path()
            ? targetPath.parent_path().string() : ".";
        if (dir.empty() || dir.back() != '/') dir.append("/");
        const string targetKey = CajetaModule::remapSourcePath(
            file, dir, flags.debugPrefixMap);

        // Report against the ORIGINAL path (--shadow), root-relative when under the
        // source root - the form the whole-root document uses, so index keys agree.
        string reportAs = shadow.empty() ? file : shadow;
        if (!sourceRoot.empty()) {
            string rootSlash = sourceRoot;
            if (rootSlash.back() != '/') rootSlash.append("/");
            if (reportAs.rfind(rootSlash, 0) == 0) {
                reportAs = reportAs.substr(rootSlash.size());
            }
        }

        xref::XrefIndex index;
        xref::collectDeclarationsAndInheritance(index, sourceRoot);
        xref::drainCalls(index, sourceRoot);
        xref::drainReferences(index, sourceRoot);
        // The prune runs against the FULL index, so a target-file reference to a
        // sibling or stdlib declaration survives even though only the target streams.
        index.pruneDanglingEdges();
        std::cerr << index.toNdjson(targetKey, reportAs) << std::flush;
    }

    // Whole-project drive: prescan, parse every source under sourceRootPath, resolve
    // (placeholders, prototypes, advice, DI), run Phase 1+2 codegen to quiescence,
    // then emit per --emit and link when the mode is Exe.
    void Compiler::compile(string entryMethod, string sourceRootPath, string archiveRootPath) {
        this->entryMethod = entryMethod;

        xref::resetCapture();
        xref::setCaptureEnabled(!flags.emitXref.empty());

        // Emit the index on the way OUT — normal return or exception unwind alike.
        // A syntax error throws from deep inside the walk, and the plugin re-runs the
        // compiler on a buffer that is broken most of the time.
        struct XrefEmitGuard {
            const std::string& path;
            const std::string& root;
            ~XrefEmitGuard() {
                if (path.empty()) return;
                // A throwing destructor during unwind is std::terminate. The index
                // is a convenience; it must never take the compile down with it.
                try {
                    writeXrefIndex(path, root);
                } catch (...) {
                }
            }
        } xrefGuard{flags.emitXref, sourceRootPath};

        // The compiler process is reused across compiles, so a flag left set by a
        // prior reflection-using build would wrongly force keep-all here.
        CajetaModule::resetReflectionKeep();

        // Load + gate the cache manifest BEFORE any module is created, so the flag
        // guards it forces are what every module's codegen sees.
        setupCacheManifest();

        if (sourceRootPath[sourceRootPath.size() - 1] != '/') {
            sourceRootPath.append("/");
        }

        if (archiveRootPath[archiveRootPath.size() - 1] != '/') {
            archiveRootPath.append("/");
        }

        // Remember where the package's hand-authored skills/ lives so emitArchive can
        // embed them; the low-level compile form falls back to the source root.
        this->skillSourceRoot =
            this->skillRootOverride.empty() ? sourceRootPath
                                            : this->skillRootOverride;


        ensureStdlibModule();

        // The stdlib module was constructed without an archiveRoot; binary emit needs
        // its .o beside the user modules', so retro-fit it now. Harmless for IR emit.
        if (auto stdlib = CajetaModule::getStdlibModule()) {
            stdlib->setArchiveRoot(archiveRootPath);
        }

        // Register every classpath class in the canonical-name map BEFORE the
        // user-source prescan. Their own bitcode is never emitted from here.
        ingestClasspath();

        {
            ProgressPhase phase("prescan", "Scanning sources");
            prescanSourceRoot(sourceRootPath, getFlags().diagFormat == DiagFormat::Json);
            drainPrescannedLazyStdlib();
        }

        // unique_ptr: the emit/link calls below throw, and a trailing delete leaks.
        std::unique_ptr<list<string>> modulePaths(listModulePaths(sourceRootPath));

        {
            ProgressPhase phase("parse", "Parsing");

            // One file's syntax error must not stop the files after it. Parse
            // everything, then rethrow the first error: parseSource throws BEFORE
            // visiting, so a file that fails leaves nothing half-registered behind it.
            std::exception_ptr firstSyntaxError;
            for (string sourcePath: *modulePaths) {
                try {
                    CajetaModulePtr module =
                        createModule(sourcePath, sourceRootPath, archiveRootPath);
                    compile(module);
                } catch (SyntaxErrorException&) {
                    // Diagnostics are already on the error listener's channel.
                    if (!firstSyntaxError) {
                        firstSyntaxError = std::current_exception();
                    }
                }
            }
            if (firstSyntaxError) {
                std::rethrow_exception(firstSyntaxError);
            }
        }

        // Bind manifest entries to their parsed modules. Clean designation is honored
        // only when both cache slots exist — eviction degrades that source to dirty,
        // never to a build failure.
        if (cacheManifest) {
            for (auto& module : modules) {
                const string& sp = module->getSourcePath();
                string rel = sp.rfind(sourceRootPath, 0) == 0
                    ? sp.substr(sourceRootPath.size()) : sp;
                const CacheManifestEntry* entry = cacheManifest->find(rel);
                if (!entry) continue;
                module->setCacheSlots(entry->bcPath, entry->obligationsPath,
                                      entry->objPath);
                if (!entry->clean) continue;
                if (!std::filesystem::exists(entry->bcPath)
                    || !std::filesystem::exists(entry->obligationsPath)) {
                    cerr << "cajeta: [incremental] cache slot missing for "
                         << rel << " — recompiling\n";
                    continue;
                }
                module->setIncrementalClean(true);
            }
        }


        std::unique_ptr<ProgressPhase> resolvePhase =
            std::make_unique<ProgressPhase>("resolve", "Resolving types");

        CajetaModule::validatePlaceholders();

        CajetaModule::buildPendingPrototypes();

        if (auto stdlib = CajetaModule::getStdlibModule()) {
            emitUnrecoverableMarker(stdlib);
        }

        CajetaModule::resolveAdviceMatches();

        CajetaModule::resolveDependencyGraph();

        // Stop before codegen when the resolution passes reported recoverable errors:
        // a broken program produces no artifact, and error types must not reach
        // codegen, which is not yet in collect mode.
        if (DiagnosticEngine* eng = DiagnosticEngine::active()) {
            if (eng->hasErrors()) return;
        }

        // The engine's scope ends here. Codegen-time advisory reporters gate on
        // DiagnosticEngine::active(), so leaving it active would surface
        // stdlib-internal advisories on a clean compile. RAII restores the caller's.
        struct CodegenEngineOff {
            DiagnosticEngine* prev;
            CodegenEngineOff() : prev(DiagnosticEngine::active()) {
                DiagnosticEngine::setActive(nullptr);
            }
            ~CodegenEngineOff() { DiagnosticEngine::setActive(prev); }
        } codegenEngineOff;

        // Force the canonical Class<?> instantiation after every module is prototyped
        // and before codegen, so its bodies are emitted by the loop below.
        CajetaClass::ensureClassWildcardInstantiated();

        // Replay clean modules' recorded obligations so this build re-contains every
        // instantiation their cached .bc references. Before the codegen loop and
        // outside any codegen frame; any failure degrades that module to dirty.
        if (cacheManifest) {
            for (auto& module : modules) {
                if (!module->isIncrementalClean()) continue;
                std::ifstream obligations(module->getCacheObligationsSlot());
                std::string line;
                while (module->isIncrementalClean()
                       && std::getline(obligations, line)) {
                    // Replay must NEVER abort the build: recompiling the module is
                    // always the sound fallback.
                    std::string err;
                    bool ok = false;
                    try {
                        ok = replayObligation(line, err);
                    } catch (cajeta::Exception& e) {
                        err = e.getErrorId() + ": " + e.getMessage();
                    } catch (const std::exception& e) {
                        err = e.what();
                    } catch (...) {
                        err = "instantiation threw during replay";
                    }
                    if (!ok) {
                        cerr << "cajeta: [incremental] obligation replay"
                                " failed for " << module->getSourcePath()
                             << ": " << err << " — recompiling\n";
                        module->setIncrementalClean(false);
                    }
                }
            }
            for (auto& module : modules) {
                if (!module->isIncrementalClean()) continue;
                const string& sp = module->getSourcePath();
                cout << "[incremental] skip "
                     << (sp.rfind(sourceRootPath, 0) == 0
                         ? sp.substr(sourceRootPath.size()) : sp)
                     << "\n";
            }
        }

        resolvePhase.reset();
        std::unique_ptr<ProgressPhase> codegenPhase =
            std::make_unique<ProgressPhase>("codegen", "Generating code");

        // Binary emit must LINK the classpath deps, not just resolve them: a dep's
        // published bitcode is stdlib-stripped, so re-driving its bodies through this
        // codegen emits them target-correct with the instantiations they need.
        const bool linkClasspathDeps =
            (emitMode == EmitMode::Obj || emitMode == EmitMode::Exe)
            && !externalModules.empty();

        size_t prevMethodCount = 0;
        // Phase 1 (signatures) + Phase 2 (bodies), looped until quiescent: a user
        // body can trigger a stdlib template instantiation mid-codegen. Both phases
        // are idempotent, so revisiting an already-emitted method costs nothing.
        while (true) {
            // Rebuilt each iteration so modules added mid-codegen are picked up next.
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
            // Late interface-vtable completion for classes that prototyped while an
            // implemented interface was still a lazy-package placeholder.
            for (auto& module: codegenModules) {
                module->completePendingInterfaceVTables();
            }
            for (auto& module: codegenModules) {
                // Clean modules keep Phase-1 prototypes but skip Phase-2 bodies.
                if (module->isIncrementalClean()) continue;
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
        // A debugger must decode ANY local's dynamic type from its field metadata.
        // RTTI emission is demand-driven, so without this a --debug-info=full build
        // could carry no field metadata at all.
        if (flags.debugInfo) {
            CajetaModule::noteForceAll("--debug-info=full");
        }

        // Lean-link keep-set: codegen has quiesced (reflectionKeep() is final) and
        // finalizeClassObject below reads keepsClass(). forcesAll leaves the keep set
        // NULL, which means keep-all.
        if (flags.linkMode == LinkMode::Lean) {
            auto& rk = CajetaModule::reflectionKeep();
            for (auto& reason : rk.forceAllReasons) {
                if (reason == "--debug-info=full") continue;
                logLine("warn",
                        "warning: [reflection-forces-keep-all] " + reason +
                        " — the lean linker retains the whole class registry"
                        " for this build.\n");
            }
            if (!rk.forcesAll) {
                std::map<std::string, std::string> keptBy;
                auto keep = resolveReflectionKeepSet(&keptBy);
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

        // Emit the reflective adapter bodies now that Phase 1/2 has quiesced and every
        // method's LLVM function exists. Runs once, before clinit and emit.
        for (auto& [key, type] : CajetaType::getCanonicalMap()) {
            if (auto klass = std::dynamic_pointer_cast<CajetaClass>(type)) {
                // A clean module's cached .bc already carries these; re-emitting would
                // mutate the parse-module the emit-time swap discards.
                if (auto mod = klass->getModule();
                    mod && mod->isIncrementalClean()) continue;
                klass->emitReflectInvokeBody();
                klass->emitReflectNewBody();
                klass->finalizeClassObject();
            }
        }

        // Per-class clinit for static initializers that did not constant-fold. Once,
        // here, so the expression codegen sees the final method set.
        for (auto& module: modules) {
            if (module->isIncrementalClean()) continue;  // clinits ride the cached .bc
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
        emitXpuKernels(archiveRootPath);

        // Binary emit needs a C-ABI `main`. Uber archives carry the shim too; plain
        // `cja` libraries do not, so a consumer's own `main` cannot collide.
        if ((emitMode == EmitMode::Obj || emitMode == EmitMode::Exe
                || emitMode == EmitMode::Uber)
                && !entryMethod.empty()) {
            emitCMainShim(entryMethod);
        }


        // Every safepoint has claimed its loc_id, so the table is final: serialize it
        // into the stdlib module with a ctor that registers it — the only way an
        // external debugger maps a loc_id to a source line. Before tree-shaking.
        if (auto stdlibMod = CajetaModule::getStdlibModule()) {
            dbg::emitDbgLocTable(stdlibMod);
        }

        if (emitMode == EmitMode::Exe) {
            if (flags.treeShake == TreeShake::Report) {
                reportTreeShake();
            } else if (flags.treeShake == TreeShake::On) {
                pruneDeadClinits();   // before the method prune so dead clinit
                pruneUnreachable();   // callees cascade into it
            }
        }

        codegenPhase.reset();
        std::unique_ptr<ProgressPhase> emitPhase =
            std::make_unique<ProgressPhase>("emit", "Emitting objects");

        // Settle every module's IR before the emit branch: clean modules swap in their
        // cached .bc wholesale (a load failure is unrecoverable — fail loud), dirty
        // ones snapshot post-codegen, pre-lowering IR + obligations into their slots.
        if (cacheManifest) {
            for (auto& module: modules) {
                if (module->isIncrementalClean()) {
                    if (!module->loadBitcodeFromSlot()) {
                        throw std::runtime_error(
                            "[incremental] unusable cached bitcode "
                            + module->getCacheBcSlot()
                            + " — rebuild without --cache-manifest");
                    }
                } else {
                    if (!module->writeBitcodeToSlot()) {
                        cerr << "cajeta: [incremental] failed writing .bc"
                                " slot " << module->getCacheBcSlot() << "\n";
                    }
                    if (!module->writeObligationsToSlot()) {
                        cerr << "cajeta: [incremental] failed writing"
                                " obligations slot "
                             << module->getCacheObligationsSlot() << "\n";
                    }
                }
            }
            // Drop-function backfill: a skipped consumer's cached .bc arrives with
            // dangling extern drop declarations, including for instantiations created
            // only INDIRECTLY, which the obligation set cannot enumerate.
            std::vector<CajetaModulePtr> cacheLoaded;
            for (auto& module: modules) {
                if (module->isIncrementalClean()) cacheLoaded.push_back(module);
            }
            backfillDropFunctions(cacheLoaded,
                std::vector<CajetaModulePtr>(modules.begin(), modules.end()));
        }

        if (emitMode == EmitMode::Cja || emitMode == EmitMode::Uber) {
            emitArchive(archiveRootPath, emitMode == EmitMode::Uber);
        } else {
            // Phase 6-alt: a clean module whose native object is cached skips target
            // lowering entirely. Its .bc was still loaded above — the drop-fn backfill
            // scan and cja emit both need it.
            const bool objEmit =
                emitMode == EmitMode::Obj || emitMode == EmitMode::Exe;
            auto objPathFor = [](const CajetaModulePtr& m) {
                string p = m->getArchiveRoot() + m->getArchivePath();
                if (p.size() >= 3 && p.substr(p.size() - 3) == ".ll") {
                    p.replace(p.size() - 3, 3, ".o");
                }
                return p;
            };
            int reusedObjects = 0;
            for (auto& module: modules) {
                if (objEmit && module->isIncrementalClean()
                    && !module->getCacheObjSlot().empty()
                    && std::filesystem::exists(module->getCacheObjSlot())) {
                    string objPath = objPathFor(module);
                    std::error_code ec;
                    std::filesystem::create_directories(
                        std::filesystem::path(objPath).parent_path(), ec);
                    std::filesystem::copy_file(
                        module->getCacheObjSlot(), objPath,
                        std::filesystem::copy_options::overwrite_existing,
                        ec);
                    if (!ec) {
                        objectFiles.push_back(objPath);
                        reusedObjects++;
                        continue;
                    }
                    // Unreadable slot → fall through to a real lowering.
                }
                // Runtime is linked once into the stdlib module; users carry externs.
                emitForModule(module);
                if (!module->isIncrementalClean()) {
                    module->writeObligationsSidecar();
                    if (objEmit && cacheManifest
                        && !module->getCacheObjSlot().empty()) {
                        string objPath = objPathFor(module);
                        std::error_code ec;
                        if (std::filesystem::exists(objPath, ec)) {
                            const string& slot = module->getCacheObjSlot();
                            std::filesystem::create_directories(
                                std::filesystem::path(slot).parent_path(),
                                ec);
                            string tmp = slot + ".tmp";
                            std::filesystem::copy_file(objPath, tmp,
                                std::filesystem::copy_options::
                                    overwrite_existing, ec);
                            if (!ec) std::filesystem::rename(tmp, slot, ec);
                        }
                    }
                }
            }
            if (reusedObjects > 0) {
                cout << "[incremental] reused " << reusedObjects
                     << " cached object(s)\n";
            }
            if (linkClasspathDeps) {
                for (auto& module: externalModules) {
                    module->setArchiveRoot(archiveRootPath);
                    emitForModule(module);
                }
            }
        }

        emitPhase.reset();
        if (emitMode == EmitMode::Obj) {
            writeAotStubs(archiveRootPath);
        }
        if (emitMode == EmitMode::Exe) {
            ProgressPhase phase("link", "Linking");
            linkExecutable(archiveRootPath);
        }
    }

    // Per-module emission driven by --emit: IR writes the .ll the archive path names,
    // Obj/Exe lower a native object through TargetMachine. Exe defers linking until
    // every module's object has been written (see linkExecutable).
    void Compiler::emitForModule(CajetaModulePtr module) {
        // Attach !tbaa tags to array-element / object-field accesses before any
        // optimization runs, so LICM/GVN can hoist field loads across element stores.
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

                // ThinLTO: emit bitcode + module summary instead of a native object,
                // so the link-time backend can import and inline ACROSS modules. Obj
                // emit stays native — there is no link step there to run the backend.
                if (flags.lto == LtoMode::Thin && emitMode == EmitMode::Exe) {
                    llvm::Module& M = *module->getLlvmModule();
                    // Stamp host CPU + features as per-function attributes: the lld
                    // ThinLTO backend builds its OWN TargetMachine and reads them off
                    // each function, never from this compiler's.
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

                optimizeModule(*module->getLlvmModule(), targetMachine, flags.opt);

                // CAJETA_DUMP_CODEGEN_BC=<dir>: write each module exactly as it goes
                // into host codegen (after --opt), so a backend crash reproduces under
                // standalone `llc` and `opt -passes=verify` can name the bad block.
    if (const char* dumpDir = std::getenv("CAJETA_DUMP_CODEGEN_BC")) {
                    std::string mid = module->getLlvmModule()->getModuleIdentifier();
                    for (char& ch : mid) if (ch == '/' || ch == ' ') ch = '_';
                    std::error_code dec;
                    llvm::raw_fd_ostream bcOut(
                        std::string(dumpDir) + "/" + mid + ".codegen.bc", dec);
                    if (!dec) llvm::WriteBitcodeToFile(*module->getLlvmModule(), bcOut);
                    // And the text form: an ill-typed instruction makes the bitcode
                    // unreadable while the .ll still parses far enough for `opt`.
                    std::error_code lec;
                    llvm::raw_fd_ostream llOut(
                        std::string(dumpDir) + "/" + mid + ".codegen.ll", lec);
                    if (!lec) module->getLlvmModule()->print(llOut, nullptr);
                }

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

    // XPU device codegen for the AOT path: for each parsed module, embed each
    // @Kernel's device image + a registration ctor into that module's own host LLVM
    // module, and with --xpu-emit also drop a per-kernel artifact. Never throws.
    void Compiler::emitXpuKernels(const std::string& archiveRootPath) {
        if (xpuBackends.empty()) {
            return;
        }
        // Map a compiler-level backend onto the xpu-layer dispatch enum and supply its
        // default arch: with several backends only a lone one can honor --xpu-arch.
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

        // Scan every launch site for the constant block size each @Kernel is
        // dispatched with (the largest across sites): the AMDGPU registration sets it
        // as amdgpu-flat-work-group-size, so registers are budgeted for real occupancy.
        std::unordered_map<std::string, unsigned> kernelMaxThreads;
        std::unordered_set<std::string> kernelUnboundedBlock;
        auto constBlockThreads =
            [](const std::vector<ExpressionPtr>& dims) -> unsigned {
            if (dims.empty()) return 0;
            unsigned prod = 1;
            for (auto& d : dims) {
                auto lit = std::dynamic_pointer_cast<IntegerLiteralExpression>(d);
                if (!lit) return 0;   // non-constant block dim -> unknown
                unsigned long v = 0;
                try { v = std::stoul(lit->getRawValue()); } catch (...) { return 0; }
                if (v == 0) return 0;
                prod *= static_cast<unsigned>(v);
            }
            return prod;
        };
        // @Kernel methods DEFINED in classpath deps must register like the primary
        // compilation's own: at Obj/Exe emit their bodies are re-driven through this
        // codegen, so walk them here too or a library kernel gets no device code.
        std::vector<CajetaModulePtr> xpuModules(modules.begin(), modules.end());
        if ((emitMode == EmitMode::Obj || emitMode == EmitMode::Exe)
                && !externalModules.empty()) {
            xpuModules.insert(xpuModules.end(),
                              externalModules.begin(), externalModules.end());
        }
        for (auto& module : xpuModules) {
            auto mir = cajeta::xpu::mir::XpuMirBuilder::buildForModule(module);
            if (!mir) continue;
            for (auto& site : mir->launchSites) {
                if (!site) continue;
                std::string simple = site->kernelCanonicalName;
                if (auto dot = simple.rfind('.'); dot != std::string::npos)
                    simple = simple.substr(dot + 1);
                unsigned threads = constBlockThreads(site->block);
                // A non-constant block at ANY site leaves the backend default (sound).
                if (threads == 0) { kernelUnboundedBlock.insert(simple); continue; }
                unsigned& cur = kernelMaxThreads[simple];
                cur = std::max(cur, threads);
            }
        }
        for (const auto& k : kernelUnboundedBlock) kernelMaxThreads.erase(k);

        for (auto& module : xpuModules) {
            std::vector<MethodPtr> kernels;
            std::vector<MethodPtr> shaders;
            for (auto& method : module->getAllMethods()) {
                if (!method) continue;
                if (cajeta::xpu::isKernel(*method)) {
                    kernels.push_back(method);
                } else if (cajeta::xpu::isGraphicsShader(*method)) {
                    shaders.push_back(method);
                }
            }
            if (kernels.empty() && shaders.empty()) {
                continue;
            }
            std::string base = module->getArchiveRoot() + module->getArchivePath();
            if (base.size() >= 3 && base.substr(base.size() - 3) == ".ll") {
                base.resize(base.size() - 3);
            }

            // The bundled-backend manifest the runtime dispatcher reads: one ctor per
            // selected backend calling __cajeta_xpu_register_backend.
            std::vector<cajeta::xpu::Backend> layerBackends;
            for (XpuBackend cb : xpuBackends) layerBackends.push_back(toLayer(cb));
            cajeta::xpu::emitBackendManifest(layerBackends,
                                             *module->getLlvmModule());

            for (XpuBackend cb : xpuBackends) {
                cajeta::xpu::Backend backend = toLayer(cb);
                std::string arch = singleBackend ? xpuArch : defaultArch(cb);
                std::vector<cajeta::xpu::KernelManifest> manifests;
                cajeta::xpu::emitKernelRegistration(
                    backend, kernels, *module->getLlvmModule(), arch,
                    kernelMaxThreads, &manifests);
                if (!manifests.empty()) {
                    std::error_code ec;
                    std::filesystem::create_directories(
                        std::filesystem::path(base).parent_path(), ec);
                    for (const auto& m : manifests) {
                        std::string json = cajeta::xpu::toJson(m);
                        std::ofstream out(base + "." + cajeta::xpu::manifestFileName(m),
                                          std::ios::binary);
                        out << json;
                        xpuManifestMembers.emplace_back(
                            cajeta::xpu::manifestArchiveMemberName(m), std::move(json));
                    }
                }
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

    // Write the weak stub translation units every AOT link needs, beside the objects:
    // `--emit=exe` links them itself, and a hand-rolled link over `--emit=obj` output
    // picks them up from the same directory.
    void Compiler::writeAotStubs(const string& archiveRootPath) {
        // OptiX AS stubs: the runtime references `cajeta_xpu_optix_*`, which the JIT
        // resolves through the process-symbol generator but an --emit=exe binary
        // cannot. Weak, so a future GPU-enabled AOT link can override them.
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

        // Session-install stubs, same shape and cause: cajeta_rt_session.c declares
        // these extern because a JIT host DEFINES them. Weak, so the host's strong
        // definitions still win wherever a host exists.
        std::string sessionStubPath =
            archiveRootPath + "__cajeta_session_stub.c";
        {
            std::ofstream s(sessionStubPath, std::ios::binary);
            s << "#include <stdint.h>\n"
                 "#define W __attribute__((weak))\n"
                 "W int32_t (*__cajeta_install_hook)(const char*,int32_t,"
                 "const char*,int32_t,int32_t,char*,int32_t,void*);\n"
                 "W void* __cajeta_install_ctx;\n"
                 "W char  __cajeta_install_out[2048];\n";
        }
    }

    // Link every collected object into `outputPath` (or `<archiveRoot>/a.out`)
    // through the system C driver, adding the TLS object, the AOT stubs, the live
    // @Native archives and the per-platform runtime libraries.
    void Compiler::linkExecutable(const string& archiveRootPath) {
        string outPath = outputPath.empty()
            ? (archiveRootPath + "a.out")
            : outputPath;

        // Link through the system C compiler/driver rather than a raw linker: the
        // driver locates the platform CRT, libc and search paths and selects the right
        // object format. Honor $CC, then fall back to the usual driver names.
        std::vector<std::string> drivers;
        if (const char* envCc = std::getenv("CC")) {
            if (*envCc) drivers.emplace_back(envCc);
        }
        drivers.emplace_back("cc");
        drivers.emplace_back("clang");
        drivers.emplace_back("gcc");

        // Prefer LLD when it is locatable: GNU ld's section GC keeps unreferenced
        // external COMDAT on COFF and dead-strips less everywhere. Passed as
        // `-fuse-ld=lld` to the driver so it still supplies CRT/libc.
        bool haveLld = (bool) llvm::sys::findProgramByName("ld.lld")
                    || (bool) llvm::sys::findProgramByName("lld");
#if defined(_WIN32)
        if (!haveLld) haveLld = (bool) llvm::sys::findProgramByName("lld-link");
#endif

        // Materialize the embedded TLS native object beside the output: the exe
        // references `__cajeta_tls_*` from the always-linked stdlib thunks, and those
        // natives are kept out of the embedded JIT bitcode.
        std::string tlsObjPath = archiveRootPath + "__cajeta_tls.o";
        {
            std::ofstream tlsOut(tlsObjPath, std::ios::binary);
            tlsOut.write(reinterpret_cast<const char*>(cajeta_tls_o),
                         (std::streamsize) cajeta_tls_o_len);
        }

        writeAotStubs(archiveRootPath);
        const std::string optixStubPath =
            archiveRootPath + "__cajeta_xpu_optix_stub.c";
        const std::string sessionStubPath =
            archiveRootPath + "__cajeta_session_stub.c";

        // Native-dependency link inputs, DCE-aware: only libs whose @Native symbol is
        // still live after tree-shaking, linked as static archives so --gc-sections
        // and archive-member semantics still strip unused members.
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

        // ThinLTO link: drive the C compiler for CRT/libc but force the FORK's
        // version-matched ld.lld, which can read this compiler's bitcode summary.
        // `-B<dir>` because gcc rejects an absolute `-fuse-ld=` path; clang honors both.
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
            // Drive the ThinLTO backend at O3 to match the non-LTO O3 pipeline: lld
            // defaults to O2, which vectorizes the SIMD hot loops far less
            // aggressively. Only the backend opt level needed lifting.
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
            opt.argv.push_back(tlsObjPath);
            opt.argv.push_back(optixStubPath);
            opt.argv.push_back(sessionStubPath);
            // Native-dep archives AFTER the objects that reference them (single-
            // pass linkers pull only the referenced members; --gc-sections drops
            // the rest). DCE-aware + gated above, so empty for non-@Native progs.
            for (const auto& a : nativeArchives) opt.argv.push_back(a);
            opt.argv.push_back("-o");
            opt.argv.push_back(outPath);
            // Dead-strip unreferenced sections: codegen emits one section per function
            // and global, so the linker drops everything the entry never reaches —
            // including the TLS natives when the program never touches TLS.
#if defined(__APPLE__)
            opt.argv.push_back("-Wl,-dead_strip");
#else
            opt.argv.push_back("-Wl,--gc-sections");
#endif
#if defined(_WIN32)
            // An `--emit=exe` is a deliverable, so it STATICALLY links the mingw
            // runtime and OpenSSL: the binary then depends only on Windows system DLLs.
            // cja / uber artifacts stay dynamic — they run inside cajeta's JIT host.
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
            opt.argv.push_back("-lssl");
            opt.argv.push_back("-lcrypto");
            opt.argv.push_back("-lpthread");
#else
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
            // A failed link must fail the build: a stale executable at the
            // output path otherwise masquerades as a successful compile.
            throw std::runtime_error("--emit=exe link failed");
        }
    }

    void Compiler::emitCMainShim(const std::string& entryMethod) {
        // Accept the canonical `package.Class::method` form (what the build tool
        // emits) as well as the legacy all-dotted `package.Class.method`.
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

        // Static, returning int32 or void; either a no-arg `main()` or
        // `main(String[] args)`, whose array the shim materializes from C argv.
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

        auto stdlib = CajetaModule::getStdlibModule();
        if (!stdlib) return;
        llvm::LLVMContext& ctx = *stdlib->getLlvmContext();
        llvm::Module* lmod = stdlib->getLlvmModule();
        llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);

        llvm::Function* entryFn = entry->getLlvmFunction();
        llvm::Function* entryExtern = llvm::Function::Create(
            entryFn->getFunctionType(),
            llvm::GlobalValue::ExternalLinkage,
            entryFn->getName(),
            lmod);

        // `int main(int argc, char** argv)`: argv both populates the ambient argument
        // store behind `System.args` and is walked for `-Dkey=value` tokens, each
        // installed as a system property before user code runs.
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
        llvm::FunctionType* mainTy = llvm::FunctionType::get(
            i32Ty, {i32Ty, ptrTy}, false);
        llvm::Function* mainFn = llvm::Function::Create(
            mainTy, llvm::GlobalValue::ExternalLinkage, "main", lmod);
        llvm::BasicBlock* bb = llvm::BasicBlock::Create(ctx, "entry", mainFn);
        llvm::IRBuilder<> b(bb);

        // Install the ambient argv store FIRST, so `System.args` answers for every
        // entry shape. argv[0] is sliced off HERE, which is what keeps the exe and the
        // JIT agreeing: both stores then hold user arguments only.
        {
            llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
            llvm::FunctionType* argsInstallTy = llvm::FunctionType::get(
                llvm::Type::getVoidTy(ctx), {i64Ty, ptrTy}, false);
            llvm::FunctionCallee argsInstallFn =
                lmod->getOrInsertFunction("__cajeta_args_install", argsInstallTy);
            llvm::Value* argcAll = b.CreateSExt(mainFn->getArg(0), i64Ty);
            llvm::Value* argcLess = b.CreateSub(
                argcAll, llvm::ConstantInt::get(i64Ty, 1));
            llvm::Value* argcNeg = b.CreateICmpSLT(
                argcLess, llvm::ConstantInt::get(i64Ty, 0));
            llvm::Value* argcUser = b.CreateSelect(argcNeg,
                llvm::ConstantInt::get(i64Ty, 0), argcLess);
            llvm::Value* argvUser = b.CreateGEP(
                ptrTy, mainFn->getArg(1),
                llvm::ConstantInt::get(i64Ty, 1));
            b.CreateCall(argsInstallFn, {argcUser, argvUser});
        }

        // Walk argv for `-Dkey=value` (or `-Dkey`) tokens. A `-D` token is still a
        // user argument to `System.args`; this pass only additionally publishes it.
        {
            llvm::Value* argcVal = mainFn->getArg(0);
            llvm::Value* argvVal = mainFn->getArg(1);

            llvm::FunctionType* strncmpTy = llvm::FunctionType::get(
                i32Ty, {ptrTy, ptrTy, llvm::Type::getInt64Ty(ctx)}, false);
            llvm::FunctionCallee strncmpFn =
                lmod->getOrInsertFunction("strncmp", strncmpTy);
            llvm::FunctionType* installerTy = llvm::FunctionType::get(
                llvm::Type::getVoidTy(ctx), {ptrTy}, false);
            llvm::FunctionCallee installerFn =
                lmod->getOrInsertFunction(
                    "__cajeta_property_install", installerTy);

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
            llvm::Value* cmpResult = b.CreateCall(strncmpFn,
                {tokenPtr, dashD, llvm::ConstantInt::get(
                    llvm::Type::getInt64Ty(ctx), 2)});
            llvm::Value* isDash = b.CreateICmpEQ(cmpResult,
                llvm::ConstantInt::get(i32Ty, 0));
            b.CreateCondBr(isDash, installD, loopStep);

            b.SetInsertPoint(installD);
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

        // Arm the profiler here and drain before main returns: `__cajeta_prof_arm` is
        // a no-op when CAJETA_PROFILER is unset. In main rather than a global ctor,
        // which has no exit hook that is safe in a JIT'd run. System.exit is separate.
        {
            llvm::FunctionType* armTy =
                llvm::FunctionType::get(i32Ty, {}, false);
            b.CreateCall(lmod->getOrInsertFunction("__cajeta_prof_arm", armTy));
        }

        // For `main(String[] args)`, materialize the cajeta String[] from the store.
        // __cajeta_args_make takes the String class's total size, field byte offsets
        // and vtable, so the runtime writes each instance with no hardcoded ABI.
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
            // Field order matches the literal materialization (6.2.2
            // tagged core): 0 vtable, 1 lenTag, 2 aux, 3 base,
            // 4 cachedCpLength.
            llvm::Value* strSize    = i64c(dl.getTypeAllocSize(strStructTy));
            llvm::Value* offBytes   = i64c(sl->getElementOffset(1));   // lenTag
            llvm::Value* offByteLen = i64c(sl->getElementOffset(2));   // aux
            llvm::Value* offMode    = i64c(sl->getElementOffset(3));   // base
            llvm::Value* offCpLen   = i64c(sl->getElementOffset(4));

            llvm::Constant* vtableRef =
                llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx, 0));
            if (auto* vt = klass->getVirtualTableGlobal()) {
                vtableRef = CajetaModule::ensureGlobalInModule(lmod, vt);
            }

            llvm::FunctionType* argsMakeTy = llvm::FunctionType::get(
                ptrTy, {ptrTy, i64Ty, i64Ty, i64Ty, i64Ty, i64Ty}, false);
            llvm::FunctionCallee argsMakeFn =
                lmod->getOrInsertFunction("__cajeta_args_make", argsMakeTy);
            // Reads the ambient store above, so it cannot disagree with `System.args`.
            llvm::Value* argsArray = b.CreateCall(argsMakeFn,
                {vtableRef, strSize, offBytes, offByteLen, offMode, offCpLen});
            callArgs.push_back(argsArray);
        }

        llvm::Value* ret = b.CreateCall(entryExtern, callArgs);
        {
            llvm::FunctionType* shutdownTy = llvm::FunctionType::get(
                llvm::Type::getInt64Ty(ctx), {}, false);
            b.CreateCall(lmod->getOrInsertFunction("__cajeta_prof_shutdown",
                                                   shutdownTy));
        }
        // Translate the cajeta return into a C exit code: int32 casts through, void
        // returns 0, and any other type returns 0 (the caller inspects side effects).
        if (entryFn->getReturnType()->isIntegerTy(32)) {
            b.CreateRet(ret);
        } else if (entryFn->getReturnType()->isVoidTy()) {
            b.CreateRet(llvm::ConstantInt::get(i32Ty, 0));
        } else if (entryFn->getReturnType()->isIntegerTy()) {
            llvm::Value* trunc = b.CreateIntCast(ret, i32Ty, /*isSigned=*/true);
            b.CreateRet(trunc);
        } else {
            b.CreateRet(llvm::ConstantInt::get(i32Ty, 0));
        }
    }

    // Collect every llvm::Module that participates in the final link — user modules,
    // re-driven classpath deps, and the always-linked stdlib — into `lmods`, sorted
    // and deduplicated.
    void Compiler::collectLinkModules(std::vector<llvm::Module*>& lmods) {
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
            bool excludeClinitRoots, bool rootAllLocals) {
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

        // COFF: every local-linkage defined symbol is a root, because pruneUnreachable
        // keeps all locals there and a surviving local must pin its transitive callees.
        // Off on ELF/Mach-O, where section-GC drops the local and its callees together.
        if (rootAllLocals)
            for (auto* lm : lmods) {
                for (auto& f : lm->functions())
                    if (!f.isDeclaration() && f.hasLocalLinkage())
                        rootNames.push_back(f.getName().str());
                for (auto& g : lm->globals())
                    if (g.hasInitializer() && g.hasLocalLinkage())
                        rootNames.push_back(g.getName().str());
            }

        // BFS the by-name reference graph: a function references the globals its
        // instructions use, a global those in its initializer. Conservative and
        // matched to section-GC — referencing a vtable keeps all its slot functions.
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

        auto subsystemOf = [](const std::string& s) -> std::string {
            static const char* P = "__cajeta_cajeta_";
            size_t i = (s.rfind(P, 0) == 0) ? std::strlen(P) : 0;
            size_t j = s.find('_', i);
            return (j == std::string::npos) ? std::string("<other>")
                                            : s.substr(i, j - i);
        };
        auto looksNet = [](const std::string& s) {
            auto has = [&](const char* k){ return s.find(k) != std::string::npos; };
            return has("_net_") || has("Tls") || has("tls") || has("Ssl")
                || has("ssl") || has("crypto") || has("Crypto")
                || has("socket") || has("Socket");
        };

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
        // COFF keeps all local symbols (see the erase gate below), so there the
        // reachability walk must root every local too. Off on ELF/Mach-O, where POSIX
        // reachability is byte-identical.
        const bool isCoff = llvm::Triple(targetTriple).isOSBinFormatCOFF();
        std::unordered_set<std::string> reached = computeReachableSymbols(
            lmods, defs, /*excludeClinitRoots=*/false, /*rootAllLocals=*/isCoff);

        // Prunable = every cajeta METHOD body, from Method->llvm::Function and never a
        // name prefix, so runtime/ABI helpers, ctors, clinits and reflect adapters are
        // untouchable. deleteBody makes a method extern, so its native refs vanish too.
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

        // IR-level --gc-sections, COFF ONLY (elsewhere the linker already does it):
        // erase every defined global the reach walk missed. NEVER touch `llvm.*` or
        // appending-linkage globals — nulling llvm.global_ctors disables every clinit.
        size_t erased = 0, internalized = 0;
        if (isCoff) {
        auto isReservedGlobal = [](const GlobalValue& gv) {
            return gv.hasAppendingLinkage() || gv.getName().starts_with("llvm.");
        };
        // Only EXTERNAL-linkage symbols are eligible: reachability here is by symbol
        // NAME, and two modules each carry their own private RTTI globals under the
        // same name, so one module's name must never decide another's fate.
        auto ineligible = [&](const GlobalValue& gv) {
            return isReservedGlobal(gv) || gv.hasLocalLinkage();
        };
        std::vector<GlobalVariable*> deadGlobals;
        std::vector<Function*>       deadFns;
        std::vector<GlobalAlias*>    deadAliases;
        for (auto* lm : lmods) {
            for (auto& f : lm->functions())
                if (!f.isDeclaration() && !ineligible(f)
                        && !reached.count(f.getName().str()))
                    deadFns.push_back(&f);
            for (auto& g : lm->globals())
                if (g.hasInitializer() && !ineligible(g)
                        && !reached.count(g.getName().str()))
                    deadGlobals.push_back(&g);
            for (auto& a : lm->aliases())
                if (!ineligible(a) && !reached.count(a.getName().str()))
                    deadAliases.push_back(&a);
        }
        // Drop every dead entity's outgoing references FIRST: on COFF a symbol that
        // still references a deleted-body method becomes undefined at link and is
        // never GC'd. Dead aliases are erased outright (an alias needs an aliasee).
        for (auto* f : deadFns)     f->deleteBody();
        for (auto* g : deadGlobals) g->setInitializer(nullptr);
        // Nulling a dead global's initializer orphans the old Constant, which lingers
        // as a phantom user and keeps use_empty() false; removeDeadConstantUsers
        // collapses exactly those chains.
        auto disposeOf = [&](GlobalValue* gv) {
            gv->removeDeadConstantUsers();
            if (gv->use_empty()) { gv->eraseFromParent(); erased++; }
            else { gv->setLinkage(GlobalValue::InternalLinkage); internalized++; }
        };
        for (auto* a : deadAliases) disposeOf(a);
        for (auto* g : deadGlobals) disposeOf(g);
        for (auto* f : deadFns)     disposeOf(f);
        }   // end COFF gate

        std::cout << "tree-shake (--tree-shake=on): pruned " << pruned
                  << " of " << totalFns
                  << " unreachable cajeta method bodies (kept " << keptFns
                  << ", erased " << erased << " dead globals, internalized "
                  << internalized << ")\n";
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

        // A native callee with no OBSERVABLE program-level effect. Deliberately
        // EXCLUDES drop-chain registration and all I/O, so a clinit that constructs an
        // owned object or does I/O reaches a non-benign native and is KEPT.
        auto isBenignNative = [](StringRef n) {
            return n.starts_with("__cajeta_scope_")
                || n == "__cajeta_alloc" || n == "__cajeta_dbg_local_alloc"
                || n.starts_with("llvm.memcpy") || n.starts_with("llvm.memset")
                || n.starts_with("llvm.memmove") || n.starts_with("llvm.lifetime")
                || n.starts_with("llvm.dbg") || n.starts_with("llvm.assume");
        };

        // Analyze a clinit's transitive closure for EXTERNAL PURITY: true ("impure",
        // keep) on any indirect call or non-benign declaration. Collects stored
        // globals via each store's underlying object, so GEP-derived writes count.
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
        std::string outPath;
        if (!outputPath.empty()) {
            outPath = outputPath;
        } else {
            std::string baseName = "cajeta";
            if (!entryMethod.empty()) {
                auto lastDot = entryMethod.rfind('.');
                if (lastDot != std::string::npos && lastDot > 0) {
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

        // Staging buffer while reachability is computed: the binary entry shape plus
        // the canonical used for lookups. depIndex is -1 for user / stdlib, else an
        // index into stagedDeps — the prune drops a whole depIndex group at once.
        struct StagingEntry {
            CajetaArchiveEntry entry;
            std::string canonical;   // pkg.subpkg.Class form, no '.bc' suffix
            int depIndex = -1;       // -1 for user/stdlib; otherwise index into stagedDeps
            bool included = true;    // pruning sets false for excluded entries
        };

        // ---- Stage user + stdlib modules ----
        // Cja is a true library (the consumer brings its own stdlib); uber includes it.
        std::vector<StagingEntry> staged;
        for (auto& module : modules) {
            std::string canonical = module->getQName()
                ? module->getQName()->toCanonical()
                : std::string("anonymous");
            // Stdlib is identified by the embedded manifest's package set, NOT by a
            // "cajeta." prefix: first-party tools live in cajeta.* without being
            // stdlib. A template instantiation classifies by its template's package.
            std::string pkgOf = canonical;
            auto ltPos = pkgOf.find('<');
            if (ltPos != std::string::npos) pkgOf.resize(ltPos);
            auto lastDotPos = pkgOf.find_last_of('.');
            pkgOf = (lastDotPos == std::string::npos)
                ? std::string() : pkgOf.substr(0, lastDotPos);
            // The fused parsed-stdlib module's package has no entry in the embedded
            // file table, so it is identified by module identity instead.
            bool isStdlib = module == CajetaModule::getStdlibModule()
                || stdlibPackageIndex().count(pkgOf) > 0;
            if (!uber && isStdlib) {
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
            se.included = true;
            staged.push_back(std::move(se));

            // Ship the original .cajeta source alongside the bitcode so a downstream
            // compile can ingest this archive via --classpath. Only for modules backed
            // by a real file — the stdlib module is fused from many and has none.
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

        std::vector<CajetaArchive::DepSummary> stagedDeps;

        // ---- Stage classpath (uber only) ----
        if (uber) {
            // Dep entries default to included=false; the closure pass below flips each
            // one an included bitcode references. With pruneUber off, every dep entry
            // is included up front.
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
                    // deps/<name>-<version>/<original-path>, so two deps sharing a
                    // class canonical no longer collide on entry path.
                    std::string depPrefix = "deps/"
                        + summary.name + "-" + summary.version + "/";

                    for (const auto& depEntry : dep.getEntries()) {
                        std::string nestedName = depPrefix + depEntry.name;
                        bool seen = false;
                        for (const auto& already : staged) {
                            if (already.entry.name == nestedName) {
                                seen = true;
                                break;
                            }
                        }
                        if (seen) continue;

                        // The canonical for reachability uses the ORIGINAL dep-internal
                        // path (no `deps/` prefix), because that is what appears in a
                        // cross-module reference's mangled symbol.
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
                // Iterative substring-scan closure seeded with the user+stdlib entries.
                // Substring works because RTTI globals embed the canonical as a literal
                // and method symbols carry it as a prefix; it is conservative.
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

        if (uber) {
            std::vector<CajetaArchive::DepSummary> kept;
            for (auto& d : stagedDeps) {
                if (d.includedEntryCount > 0) kept.push_back(std::move(d));
            }
            arc.setDeps(std::move(kept));
        }

        // Embed the package's hand-authored skills: a no-op when there is no skills/
        // dir, and a clear error on an invalid one. Uber archives carry the consuming
        // project's own skills only — each dependency ships its own inside its .cja.
        buildtool::skill::addSkillMembersToArchiveOrThrow(arc, skillSourceRoot);

        // Bundle the resolved native artifacts for the live @Native libs into the
        // archive's native/ tree. No-op when none is live; bundles the host platform's
        // artifact, and a missing one is reported rather than fatal.
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

        // Reflection-keep summary: a dependency's reflection sites live in bodies the
        // CONSUMER never re-codegens, so without this entry a lean link would silently
        // strip the classes the dep enumerates at runtime. Written only when non-empty.
        {
            auto& rk = CajetaModule::reflectionKeep();
            // Only CODE-driven demands travel: `--debug-info=full` forces keep-all for
            // THIS build's debuggability, and exporting it would force keep-all on
            // every consumer of any debug-built archive.
            std::vector<std::string> codeReasons;
            for (auto& reason : rk.forceAllReasons) {
                if (reason != "--debug-info=full") codeReasons.push_back(reason);
            }
            bool codeForcesAll = !codeReasons.empty()
                || (rk.forcesAll && rk.forceAllReasons.empty());
            if (codeForcesAll || !rk.sites.empty()) {
                std::string body;
                for (auto& reason : codeReasons) {
                    body += "forceall " + reason + "\n";
                }
                if (codeForcesAll && codeReasons.empty()) {
                    body += "forceall (unattributed)\n";
                }
                using RS = CajetaModule::ReflSite;
                for (auto& site : rk.sites) {
                    const char* tag = nullptr;
                    switch (site.kind) {
                        case RS::BoundClosure:    tag = "bound"; break;
                        case RS::ForNameLiteral:  tag = "forname"; break;
                        case RS::PackageLiteral:  tag = "package"; break;
                        case RS::Annotated:       tag = "annotated"; break;
                        case RS::MethodAnnotated: tag = "methodannotated"; break;
                    }
                    if (tag) body += std::string(tag) + " " + site.selector + "\n";
                }
                CajetaArchiveEntry e;
                e.name = "meta/reflection-keep.v1";
                e.originTag = (uint8_t) CajetaArchive::Origin::User;
                e.kindTag = CajetaArchive::EntryKind::Resource;
                e.data.assign(body.begin(), body.end());
                arc.addEntry(std::move(e));
            }
        }

        // Every kernel manifest this build produced rides in the archive beside the
        // class bitcode that carries its device code — the same bytes the registration
        // ctor embeds. A tool reads it without loading bitcode; the linker ignores it.
        for (const auto& [memberName, json] : xpuManifestMembers) {
            CajetaArchiveEntry e;
            e.name = memberName;
            e.originTag = (uint8_t) CajetaArchive::Origin::User;
            e.kindTag = CajetaArchive::EntryKind::KernelManifest;
            e.data.assign(json.begin(), json.end());
            arc.addEntry(std::move(e));
        }

        arc.writeTo(outPath);
    }
}