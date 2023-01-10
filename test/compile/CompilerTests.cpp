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
    string inputPath = CAJETA_TEST_ROOT + string("/compile/code/src/cajeta/Test.cajeta");
    string sourceRootPath = CAJETA_TEST_ROOT + string("/compile/code/src");
    string outputPath = CAJETA_TEST_ROOT + string("/compile/code/build");
    Compiler compiler;
    CajetaModulePtr pModule = compiler.createModule(inputPath, sourceRootPath, outputPath);
    compiler.compile(pModule);
    auto modules = CajetaModule::getStructureToModule();
    EXPECT_EQ(modules.size(), 1);
}

TEST(CompilerTests, canParseOnValidLongPackage) {
    string inputPath = CAJETA_TEST_ROOT + string("/compile/code/src/foo/bar/baz/Test.cajeta");
    string sourceRootPath = CAJETA_TEST_ROOT + string("/compile/code/src");
    string outputPath = CAJETA_TEST_ROOT + string("/compile/code/build");
    Compiler compiler;
    CajetaModulePtr pModule = compiler.createModule(inputPath, sourceRootPath, outputPath);
    compiler.compile(pModule);
    auto modules = CajetaModule::getStructureToModule();
    auto structure = pModule->getStructures()["foo.bar.baz.Test"];
    EXPECT_EQ(modules.size(), 1);
    EXPECT_EQ(structure->getProperties().size(), 2);
    EXPECT_EQ(structure->getMethods().size(), 3);
}

TEST(CompilerTests, canWriteAndReadClassMetadata) {
    string inputPath = CAJETA_TEST_ROOT + string("/compile/code/src/foo/bar/baz/Test.cajeta");
    string sourceRootPath = CAJETA_TEST_ROOT + string("/compile/code/src");
    string outputPath = CAJETA_TEST_ROOT + string("/compile/code/build");
    Compiler compiler;
    CajetaModulePtr pModule = compiler.createModule(inputPath, sourceRootPath, outputPath);
    compiler.compile(pModule);
    // Compilation should have written the metadata, now read and verify the data from the module
}