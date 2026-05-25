//
// Created by James Klappenbach on 10/24/22.
//

#include "Compiler.h"
#include "CajetaModule.h"
#include "CajetaLlvmVisitor.h"
#include "StdlibEmbedded.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "../asn/AbstractSyntaxNode.h"
#include "../type/CajetaType.h"
#include "cajeta/error/CajetaExceptions.h"
#include "CajetaParserBaseVisitor.h"
#include <sys/stat.h>

#ifdef CAJETA_HAS_LLD
#include "lld/Common/Driver.h"
LLD_HAS_DRIVER(elf)
#endif


using namespace antlr4;
using namespace std;

namespace cajeta {

    void Compiler::rebuildTargetMachine() {
        string error;
        target = llvm::TargetRegistry::lookupTarget(targetTriple, error);
        if (!target) {
            cerr << "cajeta: could not lookup target '" << targetTriple << "': " << error << std::endl;
            targetMachine = nullptr;
            return;
        }
        targetMachine = target->createTargetMachine(targetTriple, cpu, features, opt, RM);
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
            registerAndRecurse(ctx->identifier()->getText(), ctx);
            return defaultResult();
        }

        std::any visitInterfaceDeclaration(
                CajetaParser::InterfaceDeclarationContext* ctx) override {
            registerAndRecurse(ctx->identifier()->getText(), ctx);
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
        void registerAndRecurse(const std::string& shortName,
                                 antlr4::tree::ParseTree* tree,
                                 bool markEnum = false) {
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
            enclosingStack.push_back(shortName);
            visitChildren(tree);
            enclosingStack.pop_back();
        }
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
        for (size_t i = 0; i < cajeta::stdlib::g_fileCount; ++i) {
            const auto& f = cajeta::stdlib::g_files[i];
            antlr4::ANTLRInputStream prescanIn(
                std::string(f.content, f.contentBytes));
            prescanSource(prescanIn);
        }

        QualifiedNamePtr originalQName = module->getQName();
        for (size_t i = 0; i < cajeta::stdlib::g_fileCount; ++i) {
            const auto& f = cajeta::stdlib::g_files[i];
            std::string relPath = f.relativePath;
            auto lastSlash = relPath.find_last_of('/');
            std::string pkg = (lastSlash == std::string::npos)
                ? std::string()
                : relPath.substr(0, lastSlash);
            std::replace(pkg.begin(), pkg.end(), '/', '.');
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
    }

    // Error-model #210: emit the marker global the runtime uses to
    // detect Unrecoverable throws. UnrecoverableException's vtable
    // address is published as `__cajeta_unrecoverable_vtable_marker`;
    // the runtime helper `__cajeta_is_unrecoverable` walks a thrown
    // instance's vtable chain and matches against this address to
    // decide whether the throw should bypass user catch handlers.
    //
    // Must run AFTER CajetaModule::buildPendingPrototypes — the
    // vtable global is created during UnrecoverableException's
    // generatePrototype, which the deferred-prototype machinery may
    // delay until after the module's parse finishes.
    void emitUnrecoverableMarker(CajetaModulePtr module) {
        auto& structures = module->getStructures();
        auto it = structures.find("cajeta.error.UnrecoverableException");
        if (it == structures.end()) return;
        CajetaClassPtr unrecClass = it->second;
        llvm::GlobalVariable* unrecVT = unrecClass->getVirtualTableGlobal();
        if (!unrecVT) return;
        llvm::Module* lmod = module->getLlvmModule();
        // The runtime bitcode declares the marker as `extern void*` so
        // its `__cajeta_is_unrecoverable` helper can read it.
        // linkRuntime brings that declaration in; we then upgrade it
        // to a definition by setting an initializer + external
        // linkage. If there is no pre-existing entry (no runtime
        // linked in this module) we create the global outright. The
        // already-has-an-initializer case is the second-call no-op.
        auto& llvmCtx = *module->getLlvmContext();
        llvm::PointerType* ptrTy = llvm::PointerType::get(llvmCtx, 0);
        if (auto* existing = lmod->getGlobalVariable(
                "__cajeta_unrecoverable_vtable_marker", true)) {
            if (existing->hasInitializer()) return;
            existing->setInitializer(unrecVT);
            existing->setLinkage(llvm::GlobalValue::ExternalLinkage);
            return;
        }
        new llvm::GlobalVariable(
            *lmod, ptrTy, /*isConstant=*/false,
            llvm::GlobalValue::ExternalLinkage,
            unrecVT,
            "__cajeta_unrecoverable_vtable_marker");
    }

    bool fileExists(string& sourcePath) {
        struct stat buffer;
        return (stat(sourcePath.c_str(), &buffer) == 0);
    }

    CajetaModulePtr Compiler::createModule(string sourcePath, string sourceRootPath, string targetRootPath) {
        if (!fileExists(sourcePath))
            throw FileNotFoundException(sourcePath);

        auto module = make_shared<CajetaModule>(&llvmContext,
            sourcePath,
            sourceRootPath,
            targetRootPath,
            targetTriple,
            targetMachine);
        // Propagate compiler-level flags so codegen for this module respects them.
        module->setFlags(flags);
        return module;
    }

    CajetaModulePtr Compiler::ensureStdlibModule() {
        auto existing = CajetaModule::getStdlibModule();
        if (existing) return existing;

        auto stdlibQName = QualifiedName::getOrInsert(
            "__stdlib__", "cajeta.runtime");
        auto stdlib = make_shared<CajetaModule>(
            &llvmContext, stdlibQName, targetTriple, targetMachine);
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
        emitUnrecoverableMarker(stdlib);
        return stdlib;
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
        if (sourceRootPath[sourceRootPath.size() - 1] != '/') {
            sourceRootPath.append("/");
        }

        if (archiveRootPath[archiveRootPath.size() - 1] != '/') {
            archiveRootPath.append("/");
        }

//        std::filesystem::path cwd = std::filesystem::current_path();

        ensureStdlibModule();

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
        size_t prevMethodCount = 0;
        while (true) {
            size_t methodCount = 0;
            for (auto& module: modules) {
                methodCount += module->getAllMethods().size();
            }
            for (auto& module: modules) {
                for (auto& method: module->getAllMethods()) {
                    method->getLlvmFunctionType();
                }
            }
            for (auto& module: modules) {
                for (auto& method: module->getAllMethods()) {
                    method->generateCode();
                }
            }
            size_t after = 0;
            for (auto& module: modules) {
                after += module->getAllMethods().size();
            }
            if (after == methodCount && after == prevMethodCount) break;
            prevMethodCount = after;
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
        for (auto& module: modules) {
            // Runtime is linked once into the stdlib module (see
            // parseStdlibInto); user modules carry only extern decls
            // for runtime helpers, resolved by the JIT/AOT link step.
            emitForModule(module);
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

    // For --emit=exe: link the per-module .o files into a single executable. Uses lld
    // when it was found at CMake-configure time (see CAJETA_HAS_LLD in CMakeLists.txt);
    // otherwise emits a clear diagnostic so the user can link with their toolchain.
    void Compiler::linkExecutable(const string& archiveRootPath) {
#ifdef CAJETA_HAS_LLD
        std::vector<const char*> args;
        args.push_back("ld.lld");
        for (auto& obj : objectFiles) {
            args.push_back(obj.c_str());
        }
        string outArg = "-o";
        string outPath = outputPath.empty()
            ? (archiveRootPath + "a.out")
            : outputPath;
        args.push_back(outArg.c_str());
        args.push_back(outPath.c_str());
        // Caller is responsible for any libc / sysroot flags via environment or a
        // future --linker-arg passthrough. For pure-Cajeta programs that don't pull
        // libc symbols this minimal arg list is enough.
        lld::Result r = lld::lldMain(args, llvm::outs(), llvm::errs(),
            {{lld::Gnu, &lld::elf::link}});
        if (r.retCode != 0) {
            cerr << "cajeta: lld link failed (exit " << r.retCode << ")" << std::endl;
        }
#else
        cerr << "cajeta: --emit=exe requires lld libraries (install lld-"
             << LLVM_VERSION_MAJOR << "-dev and reconfigure with CMake)."
             << std::endl;
        cerr << "Object files produced: ";
        for (auto& obj : objectFiles) cerr << obj << " ";
        cerr << "\nLink with: cc " ;
        for (auto& obj : objectFiles) cerr << obj << " ";
        cerr << "-o <executable>" << std::endl;
#endif
    }
}