//
// Created by James Klappenbach on 10/24/22.
//

#include "Compiler.h"
#include "CajetaModule.h"
#include "CajetaLlvmVisitor.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "../asn/AbstractSyntaxNode.h"
#include "cajeta/error/CajetaExceptions.h"
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

    list<string>* listModulePaths(string rootPath) {
        list<string>* result = new list<string>;

        using recursive_directory_iterator = std::filesystem::recursive_directory_iterator;
        std::filesystem::path sourcePath(rootPath);

        for (const auto& dirEntry: recursive_directory_iterator(sourcePath)) {
            if (dirEntry.is_regular_file() && dirEntry.path().string().find(".code")) {
                result->push_back(dirEntry.path().string());
            }
        }

        return result;
    }

    // Stdlib prelude — the minimal class hierarchy every Cajeta compilation
    // unit gets implicitly. v1 just carries the error-model types from
    // ErrorModel.md; future stdlib growth (`String` class, collections,
    // `Task<T>` exposed as a user-typeable template, etc.) extends this
    // string. Kept inline rather than in a separate .caj file so the
    // compiler binary is self-contained — distribution doesn't need to
    // ship a stdlib directory alongside the executable.
    //
    // Constructors don't call super(...) — `super` is still
    // UnsupportedExpression. Inherited fields are written directly via
    // `this.message`. Each subclass repeats the field assignment.
    static const char* const STDLIB_SOURCE = R"CAJETA(
package cajeta.lang;
public class Throwable {
    public String message;
    public Throwable(String message) {
        this.message = message;
    }
}
public class Exception extends Throwable {
    public Throwable cause;
    public Exception(String message) {
        this.message = message;
        this.cause = 0;
    }
}
public class RecoverableException extends Exception {
    public RecoverableException(String message) {
        this.message = message;
        this.cause = 0;
    }
}
public class UnrecoverableException extends Exception {
    public UnrecoverableException(String message) {
        this.message = message;
        this.cause = 0;
    }
}
)CAJETA";

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
        if (label && label[0] != '\0') {
            cout << "\n\n";
            std::cout << parseTree->toStringTree(&parser, true) << std::endl;
        }
        delete visitor;
        CajetaModule::setActiveModule(prevActive);
    }

    void parse(CajetaModulePtr module) {
        // Stdlib first — its types must be in canonicalMap before user code
        // can reference them by simple name. The module's qName is path-
        // derived (e.g. `test.D` from `test/D.cajeta`), but stdlib classes
        // declare `package cajeta.lang;`, so we temporarily swap the
        // module's qName to one whose package is `cajeta.lang` for the
        // stdlib parse — that way visitClassDeclaration builds stdlib
        // classes with `cajeta.lang.Throwable` canonical names. Restored
        // before the user-source parse so user classes get their proper
        // path-derived package.
        QualifiedNamePtr originalQName = module->getQName();
        module->setQName(QualifiedName::getOrInsert("stdlib", "cajeta.lang"));
        antlr4::ANTLRInputStream stdlibInput(STDLIB_SOURCE);
        parseSource(module, stdlibInput, /*label=*/"");
        module->setQName(originalQName);

        ifstream stream;
        stream.open(module->getSourcePath());
        stream.seekg(0);
        antlr4::ANTLRInputStream userInput(stream);
        parseSource(module, userInput, /*label=*/"user");
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
        module->setBoundsCheckEnabled(boundsCheckEnabled);
        return module;
    }

    void Compiler::compile(CajetaModulePtr module) {
        modules.push_back(module);
        parse(module);
    }

    void Compiler::compile(string entryMethod, string sourceRootPath, string archiveRootPath) {
        if (sourceRootPath[sourceRootPath.size() - 1] != '/') {
            sourceRootPath.append("/");
        }

        if (archiveRootPath[archiveRootPath.size() - 1] != '/') {
            archiveRootPath.append("/");
        }

//        std::filesystem::path cwd = std::filesystem::current_path();

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

        // Phase 1: declaration/signature registration. Walk every method across every
        // module and register its prototype with the LLVM module. Cross-method and
        // cross-class references can resolve in Phase 2 because every name is already
        // in the symbol table.
        for (auto& module: modules) {
            for (auto& method: module->getAllMethods()) {
                method->getLlvmFunctionType();
            }
        }

        // Phase 2: body lowering. Codegen now operates against a fully-populated symbol
        // table so calls resolve in any order.
        for (auto& module: modules) {
            for (auto& method: module->getAllMethods()) {
                method->generateCode();
            }
            module->linkRuntime();
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
        cerr << "cajeta: --emit=exe requires lld libraries (install lld-18-dev and "
             << "reconfigure with CMake)." << std::endl;
        cerr << "Object files produced: ";
        for (auto& obj : objectFiles) cerr << obj << " ";
        cerr << "\nLink with: cc " ;
        for (auto& obj : objectFiles) cerr << obj << " ";
        cerr << "-o <executable>" << std::endl;
#endif
    }
}