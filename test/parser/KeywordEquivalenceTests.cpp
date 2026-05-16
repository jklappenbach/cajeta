//
// S1 — view / struct keyword equivalence.
//
// In Session 1 of the Structs+Views rollout (see StructsViewsStatus.md),
// both `view` and `struct` keywords parse and lower to the same
// CajetaStruct AST node. The semantic split happens in S2.
//
// These three tests pin that equivalence so subsequent sessions can
// diverge the two paths intentionally rather than by drift.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

std::string sourceWithKeyword(const std::string& keyword) {
    return "package test;\n"
           "public " + keyword + " Header {\n"
           "    int32 version;\n"
           "    int64 timestamp;\n"
           "    int32 payloadLen;\n"
           "}\n"
           "public final class S {\n"
           "    public static int32 run() {\n"
           "        int32[] bytes = new int32[4];\n"
           "        Header h = Header(bytes);\n"
           "        h.version = 7;\n"
           "        h.payloadLen = 35;\n"
           "        return h.version + h.payloadLen;\n"
           "    }\n"
           "}\n";
}

} // namespace

TEST(KeywordEquivalenceTests, viewKeywordParsesAndExecutes) {
    auto jit = CajetaJit::compile(sourceWithKeyword("view"), "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 42);
}

TEST(KeywordEquivalenceTests, structKeywordStillParsesAndExecutes) {
    // Backwards compatibility: existing struct-using code keeps working
    // until S2 routes struct to a distinct AST node.
    auto jit = CajetaJit::compile(sourceWithKeyword("struct"), "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 42);
}

TEST(KeywordEquivalenceTests, bothKeywordsProduceEquivalentBehavior) {
    // Same source written two ways; same runtime answer. In S2 the
    // expected answer for the struct path will change (struct gains
    // stack-alloca semantics) and this test will need to evolve.
    auto viewJit = CajetaJit::compile(sourceWithKeyword("view"), "test.S");
    auto structJit = CajetaJit::compile(sourceWithKeyword("struct"), "test.S");

    auto viewFn = viewJit->lookup<int32_t (*)()>("run");
    auto structFn = structJit->lookup<int32_t (*)()>("run");

    int32_t viewResult = viewFn();
    int32_t structResult = structFn();

    EXPECT_EQ(viewResult, structResult);
    EXPECT_EQ(viewResult, 42);
}
