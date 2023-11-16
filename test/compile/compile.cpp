#include "gtest/gtest.h"
#include <iostream>
#include <llvm/Support/InitLLVM.h>
#include "cajeta/compile/Compiler.h"

using namespace std;
using namespace antlr4;
using namespace cajeta;

TEST(blaTest, test1) {
    int argc = 3;
    const char* argv[] = { "cajeta.System::main()", "/Users/julian/code/cpp/cajeta/test/code/src", "/Users/julian/code/cpp/cajeta/test/build/" };
    llvm::InitLLVM initLlvm(argc, (char**&) argv, false);

    Compiler compiler;
//    compiler.compile(argv[1], argv[2], argv[3]);

    //act
    //assert
//    EXPECT_EQ (Compiler:: (0),  0);
//    EXPECT_EQ (Formula::bla (10), 20);
//    EXPECT_EQ (Formula::bla (50), 100);
}