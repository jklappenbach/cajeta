#include <iostream>
#include "cajeta/parse/CajetaParserIRListener.h"
#include <llvm/Support/TargetSelect.h>
#include <filesystem>
#include <utility>
#include "antlr4-runtime.h"
#include "CajetaLexer.h"
#include "CajetaParser.h"

using namespace std;
using namespace antlr4;
using namespace cajeta;

#define PATH_SEPARATOR_CHAR '/'

list<string>& findModulePaths(string rootPath) {
    list<string> result;

    using recursive_directory_iterator = __fs::filesystem::recursive_directory_iterator;

    for (const auto& dirEntry : recursive_directory_iterator(rootPath)) {
        if (dirEntry.is_regular_file() && dirEntry.path().string().find(".cajeta")) {
            result.push_back(dirEntry.path().string());
        }
    }

    return result;
}

string findPackagePathForModule(string rootSrcPath, string srcPath) {
    return srcPath.substr(rootSrcPath.length());
}

void processModule(string srcPath,
                   llvm::LLVMContext* context,
                   CompilationUnit* compilationUnit,
                   string targetTriple,
                   llvm::TargetMachine* targetMachine) {
    ifstream stream;
    stream.open(srcPath);
    ANTLRInputStream input(stream);

    CajetaLexer lexer(&input);
    CommonTokenStream tokens(&lexer);

    tokens.fill();

    CajetaParser parser(&tokens);
    antlr4::tree::ParseTree* parseTree = parser.compilationUnit();

    CajetaParserIRListener* listener = new CajetaParserIRListener(compilationUnit, context, std::move(targetTriple), targetMachine);
    parser.addParseListener(listener);
    antlr4::tree::ParseTreeWalker::DEFAULT.walk(listener, parseTree);
    std::cout << parseTree->toStringTree(&parser) << std::endl;
}

// TODO: Set up parsing stack to handle the following through callbacks:

// TODO (compilationUnit (packageDeclaration packageName (qualifiedName (identifier cajeta)) ;) (typeDeclaration (classOrInterfaceModifier public) (classDeclaration class (identifier System) (classBody { (classBodyDeclaration (modifier (classOrInterfaceModifier public)) (modifier (classOrInterfaceModifier static)) (memberDeclaration (methodDeclaration (typeTypeOrVoid void) (identifier main) (formalParameters ( (formalParameterList (formalParameter (typeType (classOrInterfaceType (identifier String)) [ ]) (variableDeclaratorId (identifier args)))) )) (methodBody (block { (blockStatement (statement (expression (expression (expression (primary (identifier System))) . (identifier out)) . (methodCall (identifier printf) ( (expressionList (expression (primary (literal "Hello World!\n")))) ))) ;)) }))))) }))))
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
    list<string> modulePaths = findModulePaths(argv[1]);
    string buildRoot = argv[2];

    string pathBase;
    for (string path : modulePaths) {
        CompilationUnit* compilationUnit = CompilationUnit::create(context,
                                                                   path,
                                                                   rootSrcPath,
                                                                   rootTargetPath,
                                                                   targetMachine,
                                                                   targetTriple);
        processModule(path, context, compilationUnit, targetTriple, targetMachine);
        // WriteBitcodeToFile(typeDefinition->module, *out);
    }

//    delete visitor;
//    delete ctxLlvm;
//    delete parseTree;

    return 0;
}
