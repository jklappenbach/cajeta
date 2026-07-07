// String buffer constructor — `String(#int8[] bytes, int32 byteLength)`
// takes ownership of the caller's byte buffer (the `#` formal transfers;
// the caller's drop entry deactivates) and sets mode = 0 (owned): this
// String's drop frees the bytes. This is the builder path every stdlib
// `return heap String(#out, n)` routes through.
//
// History: this ctor originally set mode = 1 (view) while still taking
// `#` — the signature transferred ownership the body then declined, so
// every transferred buffer leaked (slices plan 9.1 fixed it to owned;
// see the ctor's doc comment in String.cajeta). Literal-backed strings
// never route here — literal codegen materializes its own mode-1
// instances pointing at static storage.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cstdint>

using cajeta_test::CajetaJit;

namespace {
int64_t runI64(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int64_t (*)()>("run");
    return fn();
}
int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}
} // namespace

// Buffer ctor stores the caller's byteLength argument into the
// header's byteLength field — proves the int32 parameter reaches the
// field assignment.
TEST(StringViewCtorTests, bufferCtorPopulatesByteLength) {
    auto src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int8[] b = heap int8[5];\n"
        "        String s = heap String(#b, 5);\n"
        "        return s.byteLength;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
}

// Buffer ctor sets the mode discriminator to 0 (owned) — the `#` formal
// transferred the bytes in, so this String's drop must free them. Pin it
// explicitly so a refactor can't silently flip the ctor back to the
// leaky mode-1 behavior slices 9.1 removed.
TEST(StringViewCtorTests, bufferCtorSetsModeOwned) {
    auto src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int8[] b = heap int8[3];\n"
        "        String s = heap String(#b, 3);\n"
        "        return s.mode;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}

// Buffer ctor leaves cachedCpLength = -1 (not yet computed), so the
// first count() call walks the bytes correctly. Pins both ctor
// initialization and end-to-end ctor + count() integration.
TEST(StringViewCtorTests, bufferCtorPlusCountEndToEnd) {
    auto src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        // 'a' + 'é' + '😀' = 1 + 2 + 4 = 7 bytes, 3 codepoints.
        "        int8[] b = heap int8[7];\n"
        "        b[0] = (int8) 0x61;\n"  // 'a'
        "        b[1] = (int8) 0xC3;\n"  // 'é' leader
        "        b[2] = (int8) 0xA9;\n"  // 'é' continuation
        "        b[3] = (int8) 0xF0;\n"  // '😀' leader
        "        b[4] = (int8) 0x9F;\n"
        "        b[5] = (int8) 0x98;\n"
        "        b[6] = (int8) 0x80;\n"
        "        String s = heap String(#b, 7);\n"
        "        return s.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI64(src), 3);
}
