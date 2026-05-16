//
// `view` / `struct` keyword behavior pins.
//
// S1 added both keywords with shared lowering. S2 split them:
//   - view  -> CajetaView   (generatePrototype runs the view-style codegen).
//   - struct -> CajetaStruct (generatePrototype throws
//                              CAJETA_ERROR_STRUCT_UNIMPLEMENTED until S6).
//
// These tests pin the S2 behavior:
//   1. view declarations compile and execute as before.
//   2. struct declarations parse but fail at codegen with a stub error.
//   3. Declaring + instantiating a struct local also fails (S2.4 negative).
//
// In S6, when stack-struct codegen lands, tests 2 and 3 invert their
// expectations and become positive coverage for the new construct.
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

TEST(KeywordEquivalenceTests, structKeywordParsesButRejectsAtCodegen) {
    // S2 contract: struct declarations parse cleanly but fail at codegen.
    // Inverts in S6 when stack-struct semantics land.
    EXPECT_ANY_THROW(CajetaJit::compile(sourceWithKeyword("struct"), "test.S"));
}

TEST(KeywordEquivalenceTests, structDeclaredAndUsedAsLocalIsRejected) {
    // S2.4 negative test: struct Foo { int32 x; }; Foo f; — both the
    // declaration codegen and the local instantiation hit the stub error
    // path. Today the declaration's generatePrototype is what throws;
    // when S6 introduces stack alloca for struct locals, this test pins
    // that the post-S6 error mode also rejects pre-implementation usage.
    auto src =
        "package test;\n"
        "public struct Foo {\n"
        "    int32 x;\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Foo f;\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_ANY_THROW(CajetaJit::compile(src, "test.S"));
}
