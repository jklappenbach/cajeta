#include <iostream>
#include <CajetaParserBaseVisitor.h>
#include <cajeta/CajetaParserIRVisitor.h>
#include "antlr4-runtime.h"
#include "CajetaLexer.h"
#include "CajetaParser.h"

using namespace std;
using namespace antlr4;
using namespace cajeta;

int main(int argc, const char* argv[]) {
    ifstream stream;
    string path = "/Users/julian/code/cpp/cajeta/test/TestClass.cajeta";
    stream.open(path);
    ANTLRInputStream input(stream);

    CajetaLexer lexer(&input);
    CommonTokenStream tokens(&lexer);

    tokens.fill();

    //    for (auto token : tokens.getTokens()) {
    //        std::cout << token->toString() << std::endl;
    //    }

    CajetaParser parser(&tokens);
    antlr4::tree::ParseTree* parseTree = parser.compilationUnit();

    LLVMContext* llvmContext = new LLVMContext;
    CajetaParserIRVisitor* visitor = new CajetaParserIRVisitor(path, llvmContext);
    parseTree->accept(visitor);
    std::cout << parseTree->toStringTree(&parser) << std::endl;

//    delete visitor;
//    delete llvmContext;
//    delete parseTree;

    return 0;    return 0;
}