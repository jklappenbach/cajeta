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



