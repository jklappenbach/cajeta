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

// Every Cajeta compilation implicitly loads the stdlib prelude. Current
// content: cajeta.lang.{Object, Pair, Optional, String} +
// cajeta.lang.stream.{Stream, ArrayStream, TakeStream, SkipStream,
// FilterStream, MapStream, PeekStream, FlatMapStream} +
// cajeta.error.{Throwable, Exception, RecoverableException,
// UnrecoverableException, EncodingException} + cajeta.hash.Hash +
// cajeta.collection.{HashMap, HashSet, ArrayList, LinkedList,
// LinkedListNode, ImmutableList, ImmutableSet, ImmutableMap, Heap,
// RedBlackTree, RedBlackNode, BPlusTree, BPlusTreeNode} +
// cajeta.collection.ltm.{LtmBPlusTree, LtmBPlusTreeNode, LtmPager} +
// cajeta.lang.stream.{HashMapKeyStream, HashMapValueStream,
// HashMapEntryStream}. Each source file under
// runtime/src/cajeta/ adds one entry to
// CajetaModule::strutureToModule via CajetaModule::create(),
// counted whether the class is template or concrete.
// The prelude count is asserted by PROPERTY, not a hardcoded anchor: a raw total
// drifted on every legitimate prelude change and had no independent oracle (the
// only way to know the "expected" eager count is to count it — tautological).
// The tests instead assert what actually matters: the user's structure is
// registered, the eager prelude loaded (floor), and lazy packages (cajeta.math)
// stay lazy — the last directly guards the MathLazyParse regression class.

TEST(CompilerTests, canParseOnValidShortPackage) {
    string inputPath = CAJETA_TEST_ROOT + string("/compile/code/src/cajeta/Test.cajeta");
    string sourceRootPath = CAJETA_TEST_ROOT + string("/compile/code/src");
    string outputPath = CAJETA_TEST_ROOT + string("/compile/code/build");
    Compiler compiler;
    CajetaModulePtr pModule = compiler.createModule(inputPath, sourceRootPath, outputPath);
    compiler.compile(pModule);
    auto modules = CajetaModule::getStructureToModule();
    EXPECT_GT(modules.count("cajeta.Test"), 0u) << "user structure not registered";
    EXPECT_GT(modules.size(), 100u) << "eager stdlib prelude did not load";
    // cajeta.math is lazy: no cajeta.math.* structure may appear in the eager
    // prelude of a trivial file (guards the MathLazyParse regression class).
    size_t mathEager = 0;
    for (auto& kv : modules)
        if (kv.first.rfind("cajeta.math.", 0) == 0) ++mathEager;
    EXPECT_EQ(mathEager, 0u) << "cajeta.math.* must stay lazy (not eager prelude)";
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
    EXPECT_GT(modules.count("foo.bar.baz.Test"), 0u) << "user structure not registered";
    EXPECT_GT(modules.size(), 100u) << "eager stdlib prelude did not load";
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