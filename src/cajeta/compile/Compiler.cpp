//
// Created by James Klappenbach on 10/24/22.
//

#include "Compiler.h"
#include "CajetaModule.h"
#include "CajetaLlvmVisitor.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "../asn/AbstractSyntaxNode.h"
#include "cajeta/error/CajetaExceptions.h"
#include <sys/stat.h>


using namespace antlr4;
using namespace std;

namespace cajeta {
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

    void parse(CajetaModulePtr module) {
        ifstream stream;
        stream.open(module->getSourcePath());
        stream.seekg(0);
        ANTLRInputStream input(stream);
        CajetaLexer lexer(&input);
        CommonTokenStream tokens(&lexer);
        tokens.fill();
        CajetaParser parser(&tokens);
        antlr4::tree::ParseTree* parseTree = parser.compilationUnit();
        auto visitor = new CajetaLlvmVisitor(module);
        parseTree->accept(visitor);
        cout << "\n\n";
        std::cout << parseTree->toStringTree(&parser, true) << std::endl;
        delete visitor;
    }

    bool fileExists(string& sourcePath) {
        struct stat buffer;
        return (stat(sourcePath.c_str(), &buffer) == 0);
    }

    CajetaModulePtr Compiler::createModule(string sourcePath, string sourceRootPath, string targetRootPath) {
        if (!fileExists(sourcePath))
            throw FileNotFoundException(sourcePath);

        return make_shared<CajetaModule>(&llvmContext,
            sourcePath,
            sourceRootPath,
            targetRootPath,
            targetTriple,
            targetMachine);
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

        for (auto& module: modules) {
            for (auto& method: module->getAllMethods()) {
                method->generateCode();
            }
            module->writeIRFileTarget();
        }

        delete modulePaths;
    }
}