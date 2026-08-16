// String Phase 2b-β.1: first real method on cajeta.lang.String —
// `count()` returning the number of Unicode characters (codepoints).
//
// Distinct from byteLength (the raw UTF-8 byte count, exposed as a
// public field on the String header). For ASCII the two coincide; for
// multibyte UTF-8 they diverge (e.g. 'é' = 1 codepoint, 2 bytes;
// '😀' = 1 codepoint, 4 bytes).
//
// First call walks the bytes and caches in `cachedCpLength`; subsequent
// calls are O(1). Tests pin both the walk and the cache.
//
// Convention: `count()` is the going-forward element-count API across
// String and Collections (replaces the prior `size()` convention as of
// 2026-05-18). The legacy `size()` intrinsic on primitive-alias String
// receivers still routes through `__cajeta_str_len` — that's the
// MethodCallExpression intrinsic path which now ONLY fires for the
// primitive alias (CajetaClass-typed Strings dispatch to user methods).

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
} // namespace

// Default-constructed String is the empty Inline form;
// count() returns 0 and the cache is populated.

// Three ASCII bytes — one codepoint per byte, count = 3.

// Two-byte UTF-8 sequence 'é' = 0xC3 0xA9 (U+00E9). One codepoint,
// two bytes. Pins that count() skips continuation bytes.

// Four-byte UTF-8 sequence for '😀' = U+1F600 = 0xF0 0x9F 0x98 0x80.
// One codepoint, four bytes.

// Mixed ASCII + 2-byte + 4-byte: 'a' + 'é' + '😀' = 3 codepoints,
// 1 + 2 + 4 = 7 bytes.

// Cache: when cachedCpLength is pre-populated (not -1), count() returns
// the cached value without walking. Set bytes to 3-ASCII but cache to
// 999 — the cached value wins.
TEST(StringClassMethodTests, countReturnsCachedWhenPopulated) {
    auto src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        int8[] b = heap int8[3];\n"
        "        b[0] = (int8) 'x';\n"
        "        b[1] = (int8) 'y';\n"
        "        b[2] = (int8) 'z';\n"
        "        String t = heap String(#b, 3);\n"
        "        t.cachedCpLength = 999;\n"   // bypass walk
        "        return t.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI64(src), 999);
}
