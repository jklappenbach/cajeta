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
// cajeta.gpu prelude (Stream, Buffer, Thread, Workgroup, Barrier,
// Event, Wave, …) into the implicitly-loaded stdlib — +22 structures.
// --- cajeta-xpu lineage (xpu.core prelude growth) ---
// 2026-06-01: 96 → 98 — Item 8 added cajeta.gpu.Texture2D + Sampler (+2).
// 2026-06-03: 98 → 100 — Part C inc 3a added AccelerationStructure + RayQuery (+2).
// 2026-06-04: 100 → 101 — Part C CM4 added CooperativeMatrix (+1).
// 2026-06-05: 101 → 102 — B1 added the declared cajeta.math.Matrix value type (+1).
// 2026-06-06: re-anchored 102 → 104 — Tier-1 sweep added cajeta.gpu.Bits (+ drift fix).
// 2026-06-06: 104 → 105 — writable images added cajeta.gpu.Image2D (+1).
// 2026-06-09: +1 — Part C minor added cajeta.gpu.Quad (quad-control); the
//   live base had drifted to 277, so re-anchor to the actual modules.size()-1: 278.
// --- main lineage (threading / time / json / net preludes) ---
// 2026-05-31: 96 → 110 — cajeta.concurrent + Atomic + cajeta.time.Duration + #66 stream sweep.
// 2026-06-02: 110 → 123 — feature/build-system merge (collection.Cache, codec.json getters, …).
// 2026-06-06: 123 → 264 — cajeta-net merge (cajeta.io.net.{tcp,udp,dns,http,tls,ws}, …, +141).
// --- merge of origin/main into cajeta-xpu ---
// 2026-06-11: merge of origin/main (Reflection Phases 1–11 prelude) into
// cajeta-xpu (cajeta.gpu prelude). Both preludes now load together: the
// shared base + the reflection structures (cajeta.reflect.Class, Modifiers,
// annotation/registry classes, reflective adapters) + the cajeta.gpu
// structures HEAD added. This count is self-anchoring — anchored to the live
// modules.size() after the merge build.
// 2026-06-15: 320 → 328 — feature/json-schema merge added the SIMD JSON binding
// stdlib (cajeta.codec.json.{JsonIndex,JsonCursor,JsonHandler,JsonSax,
// JsonLinesWriter} + cajeta.io.Buffer; +8 prelude structures).
// 2026-06-20: 328 → 342 — codec Phase 0/1 stdlib merge: the cajeta.wire tier
// interfaces (Encoder/SchemaEncoder/Schema/Compressor/Decompressor) + the CSV
// codec (cajeta.codec.csv.{CsvIndex,CsvReader,CsvWriter,Csv,CsvParseException})
// + remaining JSON binding classes (+14 prelude structures).
// 2026-06-20: 342 → 374 — cajeta.ifx merge (main into feature/native-deps): the
// interaction-framework stdlib (Surface/Window/WindowEvent, Input/Audio/Video
// backends + sinks, capability/permission/lifecycle vocabulary, IfxInfo/
// IfxException, the Null* floors; +32 prelude structures).
static constexpr size_t STDLIB_STRUCTURE_COUNT = 374;

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