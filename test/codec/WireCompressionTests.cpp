// The cajeta.wire compression interfaces: a codec implements the block pair,
// the levelled and capped forms, and a stream, and each dispatches through
// its interface type.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cstdint>

using cajeta_test::CajetaJit;

namespace {
int32_t runI32(const std::string& body) {
    std::string src =
        "package test;\n"
        "import cajeta.wire.Compressor;\n"
        "import cajeta.wire.Decompressor;\n"
        "import cajeta.wire.CompressStream;\n"
        "import cajeta.wire.DecompressionLimitException;\n"
        "final class CopyStream implements CompressStream {\n"
        "    int8[] held;\n"
        "    int64 n;\n"
        "    boolean done;\n"
        "    CopyStream() { this.held = heap int8[256]; this.n = 0; this.done = false; }\n"
        "    public void write(int8[] src, int64 len) {\n"
        "        int64 i = 0;\n"
        "        while (i < len) { this.held[this.n + i] = src[i]; i = i + 1; }\n"
        "        this.n = this.n + len;\n"
        "    }\n"
        "    public #int8[] flush() {\n"
        "        int8[] out = heap int8[this.n];\n"
        "        int64 i = 0;\n"
        "        while (i < this.n) { out[i] = this.held[i]; i = i + 1; }\n"
        "        this.n = 0;\n"
        "        return #out;\n"
        "    }\n"
        "    public #int8[] finish() {\n"
        "        this.done = true;\n"
        "        return this.flush();\n"
        "    }\n"
        "}\n"
        "public final class Copy implements Compressor, Decompressor {\n"
        "    public Copy() { }\n"
        "    static #int8[] take(int8[] src, int64 len) {\n"
        "        int8[] out = heap int8[len];\n"
        "        int64 i = 0;\n"
        "        while (i < len) { out[i] = src[i]; i = i + 1; }\n"
        "        return #out;\n"
        "    }\n"
        "    public #int8[] compress(int8[] src) { return Copy.take(src, src.count()); }\n"
        "    public #int8[] compress(int8[] src, int64 len, int32 level) { return Copy.take(src, len); }\n"
        "    public #CompressStream stream(int32 level) { return heap CopyStream(); }\n"
        "    public #int8[] decompress(int8[] src, int64 expandedLen) { return Copy.take(src, expandedLen); }\n"
        "    public #int8[] decompress(int8[] src, int64 len, int64 maxOut) {\n"
        "        if (len > maxOut) {\n"
        "            throw heap DecompressionLimitException(\"over the cap\", maxOut);\n"
        "        }\n"
        "        return Copy.take(src, len);\n"
        "    }\n"
        "}\n"
        "public final class W {\n"
        "    static #int8[] bytes(int32 n) {\n"
        "        int8[] b = heap int8[n];\n"
        "        int32 i = 0;\n"
        "        while (i < n) { b[i] = (int8) (i + 1); i = i + 1; }\n"
        "        return #b;\n"
        "    }\n"
        "    public static int32 run() {\n" + body + "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.W");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}
}

TEST(WireCompressionTests, blockPairRoundTripsThroughInterfaces) {
    EXPECT_EQ(runI32(
        "        Compressor c = heap Copy();\n"
        "        Decompressor d = heap Copy();\n"
        "        int8[] plain #= W.bytes(10);\n"
        "        int8[] block #= c.compress(plain);\n"
        "        int8[] back #= d.decompress(block, (int64) 10);\n"
        "        return (int32) back.count() * 100 + (int32) back[9];\n"), 1010);
}

TEST(WireCompressionTests, levelledCompressTakesAPrefix) {
    EXPECT_EQ(runI32(
        "        Compressor c = heap Copy();\n"
        "        int8[] plain #= W.bytes(10);\n"
        "        int8[] block #= c.compress(plain, (int64) 4, 9);\n"
        "        return (int32) block.count() * 10 + (int32) block[3];\n"), 44);
}

TEST(WireCompressionTests, streamFlushesAndFinishes) {
    EXPECT_EQ(runI32(
        "        Compressor c = heap Copy();\n"
        "        CompressStream s #= c.stream(1);\n"
        "        int8[] a #= W.bytes(3);\n"
        "        s.write(a, (int64) 3);\n"
        "        s.write(a, (int64) 2);\n"
        "        int8[] first #= s.flush();\n"
        "        s.write(a, (int64) 1);\n"
        "        int8[] tail #= s.finish();\n"
        "        return (int32) first.count() * 100 + (int32) first[4] * 10 + (int32) tail.count();\n"), 521);
}

TEST(WireCompressionTests, cappedDecompressAdmitsUnderTheCap) {
    EXPECT_EQ(runI32(
        "        Decompressor d = heap Copy();\n"
        "        int8[] block #= W.bytes(8);\n"
        "        int8[] out #= d.decompress(block, (int64) 8, (int64) 8);\n"
        "        return (int32) out.count();\n"), 8);
}

TEST(WireCompressionTests, cappedDecompressRaisesTheLimitException) {
    EXPECT_EQ(runI32(
        "        Decompressor d = heap Copy();\n"
        "        int8[] block #= W.bytes(8);\n"
        "        try {\n"
        "            int8[] out #= d.decompress(block, (int64) 8, (int64) 5);\n"
        "            return -1;\n"
        "        } catch (DecompressionLimitException e) {\n"
        "            return (int32) e.limit;\n"
        "        }\n"), 5);
}
