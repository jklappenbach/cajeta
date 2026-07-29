//
// Probe for the `#T[]` (owned array return) compiler gap that blocks b4/DNS:
// a multi-parameter static method returning a #-marked array tripped
// CAJETA_ERROR_BORROW_RETURN_MULTI_PARAM (returnsOwnership not set + return type
// resolving to the `pointer` fallback). Single-param #T[] (Router.splitPath)
// works; scalar #T multi-param (Uri.resolve) works. This pins the array case.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>

using cajeta_test::CajetaJit;

TEST(OwnedArrayReturnProbe, multiParamOwnedPrimitiveArrayReturn) {
    auto src =
        "package test;\n"
        "public final class T {\n"
        "    public static #int32[] make(int32 a, int32 b) {\n"
        "        int32[] r = heap int32[a];\n"
        "        r[0] = b;\n"
        "        return #r;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int32[] x = T.make(3, 7);\n"
        "        return x[0];\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.T");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 7);
}

// CLASS-element owned array — matches Dns.resolve's `#SocketAddress[]` shape.
TEST(OwnedArrayReturnProbe, multiParamOwnedClassArrayReturn) {
    auto src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "public final class T {\n"
        "    public static #String[] make(String a, String b) {\n"
        "        String[] r = heap String[2];\n"
        "        r[0] = a;\n"
        "        r[1] = b;\n"
        "        return #r;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        String[] x = T.make(\"hi\", \"yo\");\n"
        "        return x[0].byteLength();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.T");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 2);
}

// The EXACT Dns.resolve shape: multi-param static returning #SocketAddress[].
TEST(OwnedArrayReturnProbe, multiParamOwnedSocketAddressArrayReturn) {
    auto src =
        "package test;\n"
        "import cajeta.io.net.SocketAddress;\n"
        "public final class T {\n"
        "    public static #SocketAddress[] make(String host, int32 port) {\n"
        "        SocketAddress[] r = heap SocketAddress[1];\n"
        "        return #r;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        SocketAddress[] x = T.make(\"h\", 80);\n"
        "        return x.count();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.T");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 1);
}
