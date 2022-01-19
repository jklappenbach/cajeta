#include <iostream>
#include <CajetaParserBaseVisitor.h>
#include "antlr4-runtime.h"
#include "CajetaLexer.h"
#include "CajetaParser.h"
#include "CajetaParserVisitor.h"

using namespace std;
using namespace antlr4;
using namespace cajeta;

int main(int argc, const char* argv[]) {
    std::ifstream stream;
    stream.open("/Users/julian/code/cpp/cajeta/test/TestClass.java");
    ANTLRInputStream input(stream);

    CajetaLexer lexer(&input);
    CommonTokenStream tokens(&lexer);

    tokens.fill();
//    for (auto token : tokens.getTokens()) {
//        std::cout << token->toString() << std::endl;
//    }

    CajetaParser parser(&tokens);
    antlr4::tree::ParseTree* parseTree = parser.compilationUnit();
    std::cout << parseTree->toStringTree(&parser) << std::endl;

    return 0;    return 0;
}