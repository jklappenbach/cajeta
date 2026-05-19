// String Phase 2b-β.2: view-mode constructor —
// `String(int8[] bytes, int32 byteLength)` constructs a view-mode
// String that borrows the caller's bytes without claiming ownership.
//
// This is the precursor to literal codegen flip (Phase 2b-β.3): once
// literal strings can be materialized as view-mode String instances
// pointing at static byte storage, every existing string-literal site
// can shift from i8* (primitive alias) to String class instance. The
// view-mode ctor exposed here is the user-callable surface; the
// compiler-side literal codegen will eventually synthesize equivalent
// initialization without going through this ctor.
//
// Mode = 1 = view ⇒ drop chain does NOT reclaim the bytes (caller
// owns them). For literal-backed strings the "caller" is the
// process's static-data segment; for user-constructed views the
// caller is whatever Cajeta-allocated buffer is on the heap.

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

// View-mode ctor stores the caller's byteLength argument into the
// header's byteLength field — proves the int32 parameter reaches the
// field assignment.
TEST(StringViewCtorTests, viewCtorPopulatesByteLength) {
    auto src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int8[] b = new int8[5];\n"
        "        String s = heap String(b, 5);\n"
        "        return s.byteLength;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
}

// View-mode ctor sets the mode discriminator to 1 (view). This is the
// load-bearing semantic difference vs. the no-arg ctor (which sets
// mode = 0 owned): pin it explicitly so refactors can't silently
// flip the default.
TEST(StringViewCtorTests, viewCtorSetsModeToView) {
    auto src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int8[] b = new int8[3];\n"
        "        String s = heap String(b, 3);\n"
        "        return s.mode;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// View-mode ctor leaves cachedCpLength = -1 (not yet computed), so the
// first count() call walks the bytes correctly. Pins both ctor
// initialization and end-to-end view-ctor + count() integration.
TEST(StringViewCtorTests, viewCtorPlusCountEndToEnd) {
    auto src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        // 'a' + 'é' + '😀' = 1 + 2 + 4 = 7 bytes, 3 codepoints.
        "        int8[] b = new int8[7];\n"
        "        b[0] = (int8) 0x61;\n"  // 'a'
        "        b[1] = (int8) 0xC3;\n"  // 'é' leader
        "        b[2] = (int8) 0xA9;\n"  // 'é' continuation
        "        b[3] = (int8) 0xF0;\n"  // '😀' leader
        "        b[4] = (int8) 0x9F;\n"
        "        b[5] = (int8) 0x98;\n"
        "        b[6] = (int8) 0x80;\n"
        "        String s = heap String(b, 7);\n"
        "        return s.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI64(src), 3);
}
