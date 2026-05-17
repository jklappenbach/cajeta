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

// Binary `+` lowers to __cajeta_str_concat — fresh malloc. The
// result going out of scope should fire a drop. Today: 0 drops,
// leak. After this gap fix: 1 drop.
TEST(OwnedStringDropTests, concatResultDropsAtScopeExit) {
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
