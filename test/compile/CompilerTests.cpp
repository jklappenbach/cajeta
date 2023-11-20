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

TEST(CompilerTests, canParseOnValidShortPackage) {
    string inputPath = CAJETA_TEST_ROOT + string("/compile/code/src/cajeta/System.cajeta");
    string sourceRootPath = CAJETA_TEST_ROOT + string("/compile/code/src");
    string outputPath = CAJETA_TEST_ROOT + string("/compile/code/build");
    Compiler compiler;
    CajetaModulePtr pModule = compiler.createModule(inputPath, sourceRootPath, outputPath);
    compiler.compile(pModule);
    auto modules = CajetaModule::getGlobalStructures();
    EXPECT_EQ(modules.size(), 1);
}

TEST(CompilerTests, canParseOnValidLongPackage) {
    string inputPath = CAJETA_TEST_ROOT + string("/compile/code/src/foo/bar/baz/System.cajeta");
    string sourceRootPath = CAJETA_TEST_ROOT + string("/compile/code/src");
    string outputPath = CAJETA_TEST_ROOT + string("/compile/code/build");
    Compiler compiler;
    CajetaModulePtr pModule = compiler.createModule(inputPath, sourceRootPath, outputPath);
    compiler.compile(pModule);
    auto modules = CajetaModule::getGlobalStructures();
    EXPECT_EQ(modules.size(), 1);
    //pModule->getStructures()
}