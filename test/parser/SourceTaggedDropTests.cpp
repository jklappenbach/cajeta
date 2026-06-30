//
// Source-tagged drop-chain entries (debug mode v1).
//
// Verifies that when CompilerFlags::sourceTags is on (default under the
// debug mode profile), every drop-chain push records the alloc-site
// file + line into the extended cajeta_drop_entry_debug struct.
// Observation channel: the Cajeta-side intrinsics
//   Cajeta.dropChainHeadAllocLine()   -> int32 line, 0 if empty
//   Cajeta.dropChainHeadAllocFile()   -> const char* (returned as pointer)
// read the head entry's tags so a test can assert that the LVD's source
// line landed in the runtime struct.
//
// Spec: docs/CompilerModes.md § Source-tagged drop-chain entries.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// Heap class local: the LVD's source line should land in the chain
// entry's alloc_line. The `heap Tracer()` push happens DURING the
// LVD's generateCode, which threads `getSourceLine()` through
// emitDropEntryForFn. While `t` is in scope, the head of the chain
// IS t's entry, so reading the head's alloc_line returns the LVD's
// line number.
TEST(SourceTaggedDropTests, heapClassLocalCarriesAllocLine) {
    // The `heap Tracer()` LVD is at line 5 of the source string (1-indexed,
    // counting "package test;\n" as line 1).
    auto src =
        "package test;\n"                                              // line 1
        "public class Tracer { }\n"                                    // line 2
        "public final class D {\n"                                     // line 3
        "    public static int32 run() {\n"                            // line 4
        "        Tracer t = heap Tracer();\n"                          // line 5
        "        return Cajeta.dropChainHeadAllocLine();\n"            // line 6
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
}

// Heap array local — different drop_fn (__cajeta_free_array), same
// source-tag path. Uses a non-primitive element type (String[]): a
// non-escaping single-dim PRIMITIVE array is frame-arena-routed
// (bump-allocated, reclaimed by scope reset, never on the drop chain — so no
// source tag, by design), while a reference-element array still takes the
// malloc + drop-chain + free_array path this test exercises.
TEST(SourceTaggedDropTests, heapArrayLocalCarriesAllocLine) {
    auto src =
        "package test;\n"                                              // 1
        "public final class D {\n"                                     // 2
        "    public static int32 run() {\n"                            // 3
        "        String[] xs = heap String[8];\n"                       // 4
        "        return Cajeta.dropChainHeadAllocLine();\n"            // 5
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 4);
}

// Multiple owners pushed in sequence — the most recent one wins as
// the head. Confirms entries chain correctly with source tags.
TEST(SourceTaggedDropTests, headIsTheMostRecentlyPushedEntry) {
    auto src =
        "package test;\n"                                              // 1
        "public class A { }\n"                                         // 2
        "public class B { }\n"                                         // 3
        "public final class D {\n"                                     // 4
        "    public static int32 run() {\n"                            // 5
        "        A a = heap A();\n"                                    // 6 — pushed first
        "        B b = heap B();\n"                                    // 7 — pushed second (head)
        "        return Cajeta.dropChainHeadAllocLine();\n"            // 8 — reads head = b
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// Empty chain — Cajeta.dropChainHeadAllocLine() must return 0 (no
// crash on a null head).
TEST(SourceTaggedDropTests, emptyChainReturnsZero) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        return Cajeta.dropChainHeadAllocLine();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}

// Source-FILE accessor returns the interned per-module source-file
// pointer. We can't easily inspect the string contents from a Cajeta
// int32-returning method without string facilities; instead, assert
// non-null. The line-accessor tests above already cover the value-
// integrity story; this one covers that the file slot is also
// populated.
TEST(SourceTaggedDropTests, fileAccessorReturnsNonNullWhenChainNonEmpty) {
    auto src =
        "package test;\n"
        "public class Tracer { }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Tracer t = heap Tracer();\n"
        "        if (Cajeta.dropChainHeadAllocFile() == null) {\n"
        "            return 0;\n"
        "        }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// SIGABRT handler shares its chain-walk helper with this test path —
// Cajeta.dumpDropChain() calls __cajeta_dump_drop_chain which is the
// same routine the SIGABRT handler invokes. We can't easily test the
// signal path itself without aborting the test process; this verifies
// the underlying dumper walks correctly and returns the expected count.
TEST(SourceTaggedDropTests, dumpDropChainReturnsEntryCount) {
    auto src =
        "package test;\n"
        "public class A { }\n"
        "public class B { }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        A a1 = heap A();\n"
        "        A a2 = heap A();\n"
        "        B b1 = heap B();\n"
        "        return Cajeta.dumpDropChain();\n"
        "    }\n"
        "}\n";
    // Three locals each pushed an entry; dumper returns >= 3. May exceed
    // when there are residual entries from earlier-allocated stdlib
    // structures still on the chain (e.g. main-method-level entries
    // wrapping run's frame). Pin >= 3 rather than exactly 3.
    EXPECT_GE(runI32(src), 3);
}
