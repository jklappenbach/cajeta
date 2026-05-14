//
// Tests for the error-model implementation (ErrorModel.md).
//
// Current scope: throws-clause parse + AST plumbing. Subsequent
// commits add the lint warning, runtime Throwable* migration, system
// default catch, and CajetaTask exception slot.
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

// Throws clause on a method parses and the body still codegens. No
// semantic enforcement yet — the list is informational. This test
// proves that an arbitrary type name in `throws T1, T2` doesn't break
// parse (the names don't need to resolve to actual types until the
// lint pass walks them).
TEST(ErrorModelTests, throwsClauseOnMethodParses) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 maybeFail() throws IOException, TimeoutException {\n"
        "        return 42;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        return maybeFail();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// Single-entry throws clause — most common case.
TEST(ErrorModelTests, throwsClauseSingleEntry) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 fetch() throws IOException {\n"
        "        return 7;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        return fetch();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// Stdlib prelude — Throwable + RecoverableException + UnrecoverableException
// load implicitly into every compilation unit. User code can reference them
// by simple name without an import, and `extends` works against them.
TEST(ErrorModelTests, stdlibThrowableInstantiable) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Throwable t = new Throwable(0);\n"
        "        return 42;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

TEST(ErrorModelTests, stdlibRecoverableExtendsThrowable) {
    auto src =
        "package test;\n"
        "public class IOException extends RecoverableException {\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        IOException e = new IOException();\n"
        "        return 7;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// Constructor throws clause — same grammar, separate parse path.
TEST(ErrorModelTests, constructorThrowsParses) {
    auto src =
        "package test;\n"
        "public class Resource {\n"
        "    public Resource() throws IOException { }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Resource r = new Resource();\n"
        "        return 11;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 11);
}
