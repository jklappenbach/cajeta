// String buffer constructor — `String(#int8[] bytes, int32 byteLength)`
// takes FULL ownership of the caller's byte buffer (the `#` formal
// transfers; the caller's drop entry deactivates). Under the 6.2.2
// tagged core: <= 12 B copies Inline and the buffer is freed
// immediately; longer payloads adopt the buffer as this String's OWNED
// root (drop frees it). This is the builder path every stdlib
// `return heap String(#out, n)` routes through.
//
// History: this ctor originally set the old mode field to 1 (view)
// while still taking `#` — the signature transferred ownership the body
// then declined, so every transferred buffer leaked (slices plan 9.1
// fixed it to owned). Literal-backed strings never route here.

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

// Buffer ctor carries the caller's byteLength into the core's lenTag —
// proves the int32 parameter reaches the tag.

// A <= 12 B adoption normalizes to the Inline form (no root, offset 0)
// and a > 12 B adoption keeps the buffer as the OWNED root — pin both so
// a refactor can't silently flip the ctor back to the leaky borrow
// behavior slices 9.1 removed.
TEST(StringViewCtorTests, bufferCtorNormalizesForms) {
    auto src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int8[] b = heap int8[3];\n"
        "        String s = heap String(#b, 3);\n"
        "        int32 r = 0;\n"
        "        if (s.root() == null) { r = r + 1; }\n"
        "        int8[] c = heap int8[20];\n"
        "        String t = heap String(#c, 20);\n"
        "        if (t.root() != null) { r = r + 10; }\n"
        "        if (t.byteOffset() == (int64) 0) { r = r + 100; }\n"
        "        return r;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 111);
}

// Buffer ctor leaves cachedCpLength = -1 (not yet computed), so the
// first count() call walks the bytes correctly. Pins both ctor
// initialization and end-to-end ctor + count() integration.
