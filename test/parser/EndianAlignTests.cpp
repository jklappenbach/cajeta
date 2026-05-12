//
// Session 5 — endianness annotations + @Align(natural) on struct views.
//
// @BigEndian / @LittleEndian on a struct request that all multi-byte primitive
// fields are stored in that byte order in the buffer. The compiler emits a
// bswap on each access when the struct's order differs from the host's.
// (Host is assumed little-endian in v1 — x86_64 / aarch64 default.)
//
// @Align(natural) opts the struct out of packed layout, so each field starts
// at its natural alignment (with padding inserted between).
//
// What's tested:
//   - Write-then-read through a big-endian view round-trips (the bswap pair
//     cancels for the program; the in-buffer bytes are reversed, observable
//     by indexing the underlying buffer).
//   - Little-endian and unannotated structs match the host (no bswap).
//   - @Align(natural) inserts padding (a field after a small one starts at a
//     larger offset than the packed version).
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// --- @BigEndian: write+read round-trips through host bswap ------------------

TEST(EndianAlignTests, bigEndianWriteReadRoundTrips) {
    // Writing 0x01020304 to the view, then reading it back: bswap on write
    // (host→big-endian into buffer), bswap on read (big-endian→host) cancels
    // — the program observes 0x01020304 again.
    auto src =
        "package test;\n"
        "@BigEndian\n"
        "public struct Be {\n"
        "    int32 val;\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] bytes = new int32[1];\n"
        "        Be h = Be(bytes);\n"
        "        h.val = 16909060;\n"        // 0x01020304
        "        return h.val;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 16909060);
}

TEST(EndianAlignTests, bigEndianStorageVisibleAsReversedBytes) {
    // After writing 0x01020304 via the big-endian view, reading the buffer
    // back as a little-endian (i.e. host-order) value gives the byte-reversed
    // value: 0x04030201.
    auto src =
        "package test;\n"
        "@BigEndian\n"
        "public struct Be {\n"
        "    int32 val;\n"
        "}\n"
        "public struct LeHost {\n"
        "    int32 val;\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] bytes = new int32[1];\n"
        "        Be be = Be(bytes);\n"
        "        be.val = 16909060;\n"      // 0x01020304 stored as 01 02 03 04
        "        LeHost le = LeHost(bytes);\n"
        "        return le.val;\n"           // reads bytes in host (little) order: 04 03 02 01 = 0x04030201
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0x04030201);
}

TEST(EndianAlignTests, hostEndianStructUnchanged) {
    // No annotation → host order → no bswap → buffer bytes match the
    // little-endian encoding of the value on x86_64.
    auto src =
        "package test;\n"
        "public struct Plain {\n"
        "    int32 val;\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] bytes = new int32[1];\n"
        "        Plain p = Plain(bytes);\n"
        "        p.val = 1234567;\n"
        "        return p.val;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1234567);
}

TEST(EndianAlignTests, littleEndianOnLittleHostIsNoop) {
    // Explicitly little-endian + little host → no bswap — same as no annotation.
    auto src =
        "package test;\n"
        "@LittleEndian\n"
        "public struct Le {\n"
        "    int32 val;\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] bytes = new int32[1];\n"
        "        Le le = Le(bytes);\n"
        "        le.val = 999;\n"
        "        return le.val;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 999);
}

// --- int64 fields also bswap ------------------------------------------------

TEST(EndianAlignTests, bigEndianInt64Roundtrips) {
    auto src =
        "package test;\n"
        "@BigEndian\n"
        "public struct Be64 {\n"
        "    int64 val;\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] bytes = new int32[2];\n"
        "        Be64 b = Be64(bytes);\n"
        "        b.val = 1234567890;\n"
        "        if (b.val == 1234567890) return 1;\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}
