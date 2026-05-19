//
// Gap 5 (MemoryModel.md § Known gaps): String stdlib helpers leak.
//
// `__cajeta_str_concat`, `__cajeta_str_substring`, and
// `__cajeta_str_toUpperCase` all malloc fresh memory and return it
// without registering a drop entry, so any String produced by these
// helpers leaks at scope exit.
//
// Root cause: the type system collapses owned and borrowed String
// into one type, so the drop registration site can't tell whether
// the local owns the heap memory (concat result) or just aliases
// shared storage (string literal). Without that distinction, the
// drop chain can't safely free String locals (would double-free
// every literal).
//
// Fix outline:
//   1. Add an OwnedString flag to the String type instance.
//   2. Stdlib helpers (`concat`, `substring`, `toUpperCase`, etc.)
//      return OwnedString. Literal loads and field reads return
//      regular String.
//   3. LocalVariableDeclaration registers a drop entry only when
//      the initializer's resolved type is OwnedString.
//
// Tests below are  until that landing — the leak counter
// has to be wired before EXPECT_EQ has anything to compare. Drop
// the prefix once the OwnedString variant lands.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int64_t observeDropCount(const std::string& body) {
    std::string src =
        std::string("package test;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Cajeta.dropCountReset();\n"
        "        ") + body + "\n"
        "        return 0;\n"
        "    }\n"
        "    public static int64 read() {\n"
        "        return Cajeta.dropCount();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.S");
    jit->lookup<int32_t (*)()>("run")();
    return jit->lookup<int64_t (*)()>("read")();
}

} // namespace

// Binary `+` on String operands is in a broken state post Phase 2b-β:
// string literals now materialize as class String instances, so the
// receivers BinaryOpExpression sees are class instance pointers, but
// the underlying `__cajeta_str_concat` runtime symbol still expects
// `char*`. Flipping concat to produce a class String (and reading
// receivers via the class's bytes field) is a follow-up — Phase
// 2b-γ. In the meantime, `cajeta.lang.String` follows the universal
// never-drop rule (cajeta-docs/stdlib/lang/String.md § Memory model);
// concat results would be no-op-dropped even after the path-flip.
// Test disabled until the concat surface is reworked in terms of
// class methods OR retired entirely.
TEST(OwnedStringDropTests, DISABLED_concatResultDropsAtScopeExit) {
    EXPECT_EQ(observeDropCount(
        "String result = \"hello\" + \" world\";"
    ), 1);
}

// substring is a routed method intrinsic that mallocs a fresh
// buffer (see __cajeta_str_substring in cajeta_runtime.c).
TEST(OwnedStringDropTests, substringResultDropsAtScopeExit) {
    EXPECT_EQ(observeDropCount(
        "String result = \"hello world\".substring(0, 5);"
    ), 1);
}

// toUpperCase mallocs a fresh buffer.
TEST(OwnedStringDropTests, toUpperCaseResultDropsAtScopeExit) {
    EXPECT_EQ(observeDropCount(
        "String result = \"hello\".toUpperCase();"
    ), 1);
}

// String literal alias is NOT owned — borrowing a literal must not
// register a drop, else we'd attempt to free .rodata at scope exit.
TEST(OwnedStringDropTests, literalAliasDoesNotDrop) {
    EXPECT_EQ(observeDropCount(
        "String alias = \"hello\";"
    ), 0);
}
