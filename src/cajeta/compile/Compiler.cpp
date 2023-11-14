//
// Created by James Klappenbach on 10/24/22.
//

#include "Compiler.h"
#include "CajetaModule.h"
#include "CajetaLlvmVisitor.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "../asn/AbstractSyntaxNode.h"

using namespace antlr4;
using namespace std;

namespace cajeta {
    list<string>* listModulePaths(string rootPath) {
        list<string>* result = new list<string>;

        using recursive_directory_iterator = std::filesystem::recursive_directory_iterator;
        std::filesystem::path sourcePath(rootPath);

        for (const auto& dirEntry: recursive_directory_iterator(sourcePath)) {
            if (dirEntry.is_regular_file() && dirEntry.path().string().find(".cajeta")) {
                result->push_back(dirEntry.path().string());
            }
        }

        return result;
    }

    void parse(ifstream& stream, CajetaModulePtr compilationUnit) {
        stream.seekg(0);
        ANTLRInputStream input(stream);
        CajetaLexer lexer(&input);
        CommonTokenStream tokens(&lexer);
        tokens.fill();
        CajetaParser parser(&tokens);
        antlr4::tree::ParseTree* parseTree = parser.compilationUnit();
        auto visitor = new CajetaLlvmVisitor(compilationUnit);
        parseTree->accept(visitor);
        cout << "\n\n";
        std::cout << parseTree->toStringTree(&parser, true) << std::endl;
        delete visitor;
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
        list<CajetaModulePtr> modules;

        for (string sourcePath: *modulePaths) {
            CajetaModulePtr module = make_shared<CajetaModule>(&llvmContext,
                sourcePath,
                sourceRootPath,
                archiveRootPath,
                targetTriple,
                targetMachine);
            ifstream stream;
            stream.open(module->getSourcePath());
            modules.push_back(module);
            parse(stream, module);
            cout << "\n";
        }

//        Method* method = Method::getArchive()[entryMethod];
//        if (method == nullptr) {
//            return;
//        }

        for (auto& module: modules) {
            for (auto& method: module->getAllMethods()) {
                method->generateCode();
            }
            module->writeIRFileTarget();
        }

        delete modulePaths;
    }
}