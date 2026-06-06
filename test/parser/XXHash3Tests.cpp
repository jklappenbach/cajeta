// XXH3-64 tests. The reference implementation lives in /usr/include/xxhash.h
// (linked statically via cajeta_runtime.c). These tests pin:
//   - Streaming digest matches one-shot for the same seed + bytes
//   - Different seeds produce different digests
//   - hashSeeded is deterministic across runs (same seed + bytes → same digest)
//   - String wrapper produces the same digest as the underlying byte hash
//
// Specific magic-number test vectors would couple too tightly to the
// vendored xxhash version. We pin behavior, not specific values.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

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

// Streaming vs one-shot: writing the same bytes via update() must
// produce the same digest as the one-shot hashSeeded() over the
// concatenated buffer.
TEST(XXHash3Tests, streamingMatchesOneShot) {
    auto src =
        "package test;\n"
        "import cajeta.hash.XXHash3;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int8[] piece1 = heap int8[3];\n"
        "        piece1[0L] = (int8) 1; piece1[1L] = (int8) 2; piece1[2L] = (int8) 3;\n"
        "        int8[] piece2 = heap int8[5];\n"
        "        piece2[0L] = (int8) 4; piece2[1L] = (int8) 5;\n"
        "        piece2[2L] = (int8) 6; piece2[3L] = (int8) 7; piece2[4L] = (int8) 8;\n"
        "        int8[] full = heap int8[8];\n"
        "        full[0L] = (int8) 1; full[1L] = (int8) 2; full[2L] = (int8) 3;\n"
        "        full[3L] = (int8) 4; full[4L] = (int8) 5;\n"
        "        full[5L] = (int8) 6; full[6L] = (int8) 7; full[7L] = (int8) 8;\n"
        "        XXHash3 h = heap XXHash3(42L);\n"
        "        h.update(piece1, 3L);\n"
        "        h.update(piece2, 5L);\n"
        "        int64 streamed = h.finish();\n"
        "        int64 oneshot = XXHash3.hashSeeded(full, 8L, 42L);\n"
        "        return (streamed == oneshot) ? 1 : 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// Different seeds, same data → different digests.
TEST(XXHash3Tests, seedSensitivity) {
    auto src =
        "package test;\n"
        "import cajeta.hash.XXHash3;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int8[] data = heap int8[4];\n"
        "        data[0L] = (int8) 65; data[1L] = (int8) 66;\n"
        "        data[2L] = (int8) 67; data[3L] = (int8) 68;\n"
        "        int64 h1 = XXHash3.hashSeeded(data, 4L, 1L);\n"
        "        int64 h2 = XXHash3.hashSeeded(data, 4L, 2L);\n"
        "        return (h1 != h2) ? 1 : 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// Determinism: same seed + same bytes within one process → same
// digest twice in a row. Doesn't pin the value because the test
// can't know it without depending on the xxhash library version.
TEST(XXHash3Tests, deterministicSameSeed) {
    auto src =
        "package test;\n"
        "import cajeta.hash.XXHash3;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int8[] data = heap int8[5];\n"
        "        data[0L] = (int8) 1; data[1L] = (int8) 2;\n"
        "        data[2L] = (int8) 3; data[3L] = (int8) 4; data[4L] = (int8) 5;\n"
        "        int64 h1 = XXHash3.hashSeeded(data, 5L, 9999L);\n"
        "        int64 h2 = XXHash3.hashSeeded(data, 5L, 9999L);\n"
        "        return (h1 == h2) ? 1 : 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// String wrapper produces the same digest as the underlying byte
// hash. Confirms hashString routes the String's `.bytes` /
// `.byteLength` through XXHash3.oneshot correctly.
TEST(XXHash3Tests, hashStringMatchesByteHash) {
    auto src =
        "package test;\n"
        "import cajeta.hash.XXHash3;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int8[] bytes = heap int8[3];\n"
        "        bytes[0L] = (int8) 97; bytes[1L] = (int8) 98; bytes[2L] = (int8) 99;\n"
        "        String s = heap String(#bytes, 3);\n"
        "        int64 h1 = XXHash3.hashStringSeeded(s, 0L);\n"
        "        int64 h2 = XXHash3.hashSeeded(s.bytes, 3L, 0L);\n"
        "        return (h1 == h2) ? 1 : 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}
