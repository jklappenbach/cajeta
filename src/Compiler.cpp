//
// Created by James Klappenbach on 10/24/22.
//

#include "cajeta/compile/Compiler.h"
#include "cajeta/module/CompilationUnit.h"
#include "cajeta/compile/CajetaParserIRListener.h"

using namespace antlr4;
using namespace std;

namespace cajeta {
    list<string>* listModulePaths(string rootPath) {
        list<string>* result = new list<string>;

        using recursive_directory_iterator = __fs::filesystem::recursive_directory_iterator;

        for (const auto& dirEntry : recursive_directory_iterator(rootPath)) {
            if (dirEntry.is_regular_file() && dirEntry.path().string().find(".cajeta")) {
                result->push_back(dirEntry.path().string());
            }
        }

        return result;
    }

    void compileUnit(string srcPath,
                     llvm::LLVMContext& context,
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
        delete listener;
    }

    void Compiler::compile(string srcRootPath, string targetRootPath) {
        if (srcRootPath[srcRootPath.size() - 1] != '/') {
            srcRootPath.append("/");
        }

        if (targetRootPath[targetRootPath.size() - 1] != '/') {
            targetRootPath.append("/");
        }

        list<string>* modulePaths = listModulePaths(srcRootPath);

        for (string path : *modulePaths) {
            CompilationUnit* compilationUnit = CompilationUnit::create(context,
                                                                       path,
                                                                       srcRootPath,
                                                                       targetRootPath,
                                                                       targetMachine,
                                                                       targetTriple);
            compileUnit(path, context, compilationUnit, targetTriple, targetMachine);
            compilationUnit->writeIRFileTarget();
        }
        delete modulePaths;
    }
}