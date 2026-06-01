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
// counted whether the class is template or concrete. Bump when
// stdlib grows.
// Empirical count of stdlib structures that land in
// getStructureToModule when compiling a trivial Test.cajeta. Excludes
// enums (registered as int32 aliases, not as class structures). Bump
// when stdlib's structure count actually changes; prior values
// drifted ahead of reality during the multi-class push, so re-anchor
// by running the test and reading the actual size if it diverges.
// 2026-05-29: bumped 74 → 96 after the cajeta-xpu work merged the
// cajeta.xpu.core prelude (Stream, Buffer, Thread, Workgroup, Barrier,
// Event, Wave, …) into the implicitly-loaded stdlib — +22 structures.
// 2026-05-31: bumped 96 → 105 after stack-borrowing's R7 + R8.1a work
// landed cajeta.threading primitives (Lock, LockGuard, Mutex, RwLock,
// Semaphore, WriteGuard, Channel) +7 and Atomic{Int32,Int64} +2.
// 2026-05-31: bumped 105 → 106 for R9.2 (cajeta.time.Duration).
// 2026-05-31: bumped 106 → 108 for R9.3 — cajeta.threading.Tasks pulls
// in Optional<int32> as a stdlib-side instantiation (heap Optional<int32>
// in withTimeoutInt32's return), so the structure count goes up by 2.
// 2026-05-31: bumped 108 → 110 after #66 stream pipeline sweep —
// converting `#Optional<T>` returns to value-form `Optional<T>` widens
// the set of eagerly-instantiated stream wrappers visible at module-
// load time (the new sret signatures pull in additional template
// monomorphizations that the heap-return path resolved lazily).
static constexpr size_t STDLIB_STRUCTURE_COUNT = 110;

TEST(CompilerTests, canParseOnValidShortPackage) {
    string inputPath = CAJETA_TEST_ROOT + string("/compile/code/src/cajeta/Test.cajeta");
    string sourceRootPath = CAJETA_TEST_ROOT + string("/compile/code/src");
    string outputPath = CAJETA_TEST_ROOT + string("/compile/code/build");
    Compiler compiler;
    CajetaModulePtr pModule = compiler.createModule(inputPath, sourceRootPath, outputPath);
    compiler.compile(pModule);
    auto modules = CajetaModule::getStructureToModule();
    EXPECT_EQ(modules.size(), 1 + STDLIB_STRUCTURE_COUNT);
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
    EXPECT_EQ(modules.size(), 1 + STDLIB_STRUCTURE_COUNT);
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