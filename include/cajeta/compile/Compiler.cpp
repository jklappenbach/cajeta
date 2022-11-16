//
// Created by James Klappenbach on 10/24/22.
//

#include "cajeta/compile/Compiler.h"
#include "CajetaModule.h"
#include "cajeta/compile/CajetaLlvmVisitor.h"
#include <cajeta/asn/AbstractSyntaxNode.h>
#include <cajeta/compile/CajetaModule.h>

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

    void parse(ifstream& stream, CajetaModule* compilationUnit) {
        stream.seekg(0);
        ANTLRInputStream input(stream);
        CajetaLexer lexer(&input);
        CommonTokenStream tokens(&lexer);
        tokens.fill();
        CajetaParser parser(&tokens);
        antlr4::tree::ParseTree* parseTree = parser.compilationUnit();
        CajetaLlvmVisitor* visitor = new CajetaLlvmVisitor(compilationUnit);

        parseTree->accept(visitor);
        cout << "\n\n";
        std::cout << parseTree->toStringTree(&parser, true) << std::endl;
        delete visitor;
    }

    void generateLlvm(CajetaModule* module) {
        for (auto structure : module->getStructureList()) {
            structure->generateCode(module);
        }
    }

    void Compiler::compile(string sourceRootPath, string archiveRootPath) {
        if (sourceRootPath[sourceRootPath.size() - 1] != '/') {
            sourceRootPath.append("/");
        }

        if (archiveRootPath[archiveRootPath.size() - 1] != '/') {
            archiveRootPath.append("/");
        }

        list<string>* modulePaths = listModulePaths(sourceRootPath);
        list<CajetaModule*> modules;

        for (string sourcePath : *modulePaths) {
            CajetaModule* module = new CajetaModule(&llvmContext,
                                                    std::move(sourcePath),
                                                    std::move(sourceRootPath),
                                                    std::move(archiveRootPath),
                                                    std::move(targetTriple),
                                                    targetMachine);
            ifstream stream;
            stream.open(module->getSourcePath());
            modules.push_back(module);
            parse(stream, module);
            generateLlvm(module);
            cout << "\n";
            module->writeIRFileTarget();
        }
        delete modulePaths;
    }
}