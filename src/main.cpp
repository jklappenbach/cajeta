#include <iostream>
#include <cajeta/CajetaParserIRListener.h>
#include <llvm/Support/TargetSelect.h>
#include <filesystem>
#include "antlr4-runtime.h"
#include "CajetaLexer.h"
#include "CajetaParser.h"
#include "Parser/Package.h"

using namespace std;
using namespace antlr4;
using namespace cajeta;

#define PATH_SEPARATOR_CHAR '/'

struct PackageEntry {
    map<string, PackageEntry*> entryMap;
    void addPackage(string package) {

    }
};

struct ModuleEntry : PackageEntry {
    TypeDefinition* typeDefinition;
};

list<string>* findModulePaths(string rootPath) {
    list<string>* result = new list<string>;

    using recursive_directory_iterator = __fs::filesystem::recursive_directory_iterator;

    for (const auto& dirEntry : recursive_directory_iterator(rootPath)) {
        if (dirEntry.is_regular_file() && dirEntry.path().string().find(".cajeta")) {
            result->push_back(dirEntry.path().string());
        }
    }

    return result;
}

string findPackagePathForModule(string rootSrcPath, string srcPath) {
    return srcPath.substr(rootSrcPath.length());
}

TypeDefinition* processModule(string srcPath, llvm::LLVMContext* context, string targetPath, string targetTriple,
                            llvm::TargetMachine* targetMachine) {
    ifstream stream;
    stream.open(srcPath);
    ANTLRInputStream input(stream);

    CajetaLexer lexer(&input);
    CommonTokenStream tokens(&lexer);

    tokens.fill();

    CajetaParser parser(&tokens);
    antlr4::tree::ParseTree* parseTree = parser.compilationUnit();

    //CajetaParserIRVisitor* visitor = new CajetaParserIRVisitor(srcPath, context, targetPath);
    CajetaParserIRListener* listener = new CajetaParserIRListener(srcPath, context, targetPath, targetTriple, targetMachine);
    parser.addParseListener(listener);
    antlr4::tree::ParseTreeWalker::DEFAULT.walk(listener, parseTree);
    std::cout << parseTree->toStringTree(&parser) << std::endl;
    return 0;
}

int main(int argc, const char* argv[]) {
    //cl::ParseCommandLineOptions(argc, argv, " Cajeta compiler, v1.0\n");

    auto targetTriple = llvm::sys::getDefaultTargetTriple();
    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();
    llvm::InitializeAllAsmPrinters();
    string error;
    auto target = llvm::TargetRegistry::lookupTarget(targetTriple, error);

    if (!target) {
        return -1;
    }

    auto cpu = "generic";
    auto features = "";

    llvm::TargetOptions opt;
    auto RM = llvm::Optional<llvm::Reloc::Model>();
    llvm::TargetMachine* targetMachine = target->createTargetMachine(targetTriple, cpu, features, opt, RM);
    string rootSrcPath = argv[1];
    string rootTargetPath = argv[2];

    if (rootSrcPath[rootSrcPath.size() - 1] != '/') {
        rootSrcPath.append("/");
    }

    if (rootTargetPath[rootTargetPath.size() - 1] != '/') {
        rootTargetPath.append("/");
    }

    llvm::LLVMContext* context = new llvm::LLVMContext;
    list<string>* modulePaths = findModulePaths(argv[1]);
    string buildRoot = argv[2];

    string pathBase;
    for (auto itr = modulePaths->begin(); itr != modulePaths->end(); itr++) {
        string packagePath = findPackagePathForModule(rootSrcPath, *itr);
        Package* package = Package::create(packagePath);
        TypeDefinition* typeDefinition = processModule(*itr, context, packagePath, targetTriple, targetMachine);
        // WriteBitcodeToFile(typeDefinition->module, *out);
    }

//    delete visitor;
//    delete llvmContext;
//    delete parseTree;

    return 0;
}
