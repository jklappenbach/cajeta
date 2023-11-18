#include "gtest/gtest.h"
#include "cajeta/compile/Compiler.h"
#include "cajeta/error/CajetaExceptions.h"
#include "../CajetaUnitTest.h"
#include "cajeta/compile/CajetaModule.h"

using namespace std;
using namespace antlr4;
using namespace cajeta;

TEST(CompilerTests, canThrowOnInvalidInput) {
    string inputPath = "./bad/path/source";
    string sourceRootPath = "./bad/path";
    string outputPath = "./bad/output/path";

    Compiler compiler;

    EXPECT_THROW(compiler.createModule(inputPath, sourceRootPath, outputPath), FileNotFoundException);
}

TEST(CompilerTests, canParseOnValidPath) {
    string inputPath = CAJETA_TEST_ROOT + string("/compile/cajeta/src/cajeta/System.cajeta");
    string sourceRootPath = CAJETA_TEST_ROOT + string("/compile/cajeta/src");
    string outputPath = CAJETA_TEST_ROOT + string("/compile/cajeta/build");
    Compiler compiler;
    CajetaModulePtr module = compiler.createModule(inputPath, sourceRootPath, outputPath);
    auto structures = module->getStructureStack();
    EXPECT_EQ(structures.size(), 1);
}