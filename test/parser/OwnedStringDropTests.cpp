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

// A non-escaping concat result is bump-allocated from the frame arena
// (frame-arena-plan U2): the escape pre-pass proves `result` never leaves the
// scope, so the concat wrapper/bytes come from the arena and are reclaimed by
// the scope-exit reset — NO individual drop entry is registered, so the count
// stays 0. (This is the arena path, not the retired never-drop rule; an
// *escaping* owned concat would instead register a drop, like substring below.)
TEST(OwnedStringDropTests, concatResultDoesNotDrop_neverDropRule) {
    EXPECT_EQ(observeDropCount(
        "String result = \"hello\" + \" world\";"
    ), 0);
}

// substring / toUpperCase return a freshly-allocated owned (mode 0) class
// String. Since the mode-aware String drop landed (the never-drop rule was
// retired — see String.md § Memory model), an owned-String local registers a
// drop entry that fires at scope exit and frees its bytes, so the count is
// nonzero. (Contrast concatResultDoesNotDrop_neverDropRule above, which stays 0
// only because the non-escaping concat result is bump-allocated from the frame
// arena and reclaimed by the scope reset instead of an individual drop.)


// String literal alias is NOT owned — borrowing a literal must not
// register a drop, else we'd attempt to free .rodata at scope exit.
