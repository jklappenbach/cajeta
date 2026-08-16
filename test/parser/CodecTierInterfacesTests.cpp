// Codec Phase 0.1 — foundation tier interfaces (cajeta.wire).
//
// Pins that the tier vocabulary every format resolves against compiles, is
// implementable, and round-trips on a trivial type:
//   - SchemaEncoder<T> + Schema (the untagged-format binder; carries a schema)
//   - Compressor / Decompressor (the byte->byte block tier, format-agnostic)
//   - Encoder<T> (shipped) still implements unchanged.
// All four are JIT-compiled from a trivial little-endian int32 codec so the
// round-trip is a real encode/decode, not just a declaration check.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cstdint>

using cajeta_test::CajetaJit;

namespace {
// A trivial value type + its little-endian int32 (de)serialization, shared by
// the encoder tests. `<<EXTRA>>`/`<<IMPORTS>>` get format-specific bits spliced in.
int32_t runCodec(const std::string& imports, const std::string& classes,
                 const std::string& body) {
    std::string src =
        "package test;\n" + imports +
        "public final class Box {\n"
        "    public int32 v;\n"
        "    public Box(int32 v) { this.v = v; }\n"
        "}\n" + classes +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        " + body + "\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}
} // namespace

// SchemaEncoder<T>: encode(value, schema) -> bytes, decode(bytes, schema) -> T.
// Round-trips a Box through a Schema-carrying codec; the schema is threaded
// through both directions (here a 1-byte marker the codec ignores).
TEST(CodecTierInterfacesTests, schemaEncoderRoundTrip) {
    const std::string imports =
        "import cajeta.wire.SchemaEncoder;\n"
        "import cajeta.wire.Schema;\n";
    const std::string classes =
        "public class BoxCodec implements SchemaEncoder<Box> {\n"
        "    public BoxCodec() { }\n"
        "    public #int8[] encode(Box value, Schema schema) {\n"
        "        int8[] out = heap int8[4];\n"
        "        int32 x = value.v;\n"
        "        out[0] = (int8) (x & 255);\n"
        "        out[1] = (int8) ((x >> 8) & 255);\n"
        "        out[2] = (int8) ((x >> 16) & 255);\n"
        "        out[3] = (int8) ((x >> 24) & 255);\n"
        "        return out;\n"
        "    }\n"
        "    public #Box decode(int8[] bytes, Schema schema) {\n"
        "        int32 b0 = ((int32) bytes[0]) & 255;\n"
        "        int32 b1 = ((int32) bytes[1]) & 255;\n"
        "        int32 b2 = ((int32) bytes[2]) & 255;\n"
        "        int32 b3 = ((int32) bytes[3]) & 255;\n"
        "        int32 v = b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);\n"
        "        return heap Box(v);\n"
        "    }\n"
        "}\n";
    const std::string body =
        "int8[] def = [(int8) 1];\n"
        "Schema s = heap Schema(#def);\n"
        "SchemaEncoder<Box> codec = heap BoxCodec();\n"
        "Box b = heap Box(12345);\n"
        "int8[] bytes #= codec.encode(b, s);\n"
        "Box back #= codec.decode(bytes, s);\n"
        "return back.v;";
    EXPECT_EQ(runCodec(imports, classes, body), 12345);
}

// Schema carries its raw definition bytes (v1 is an opaque carrier; Avro will
// enrich it). definition() hands back what the ctor took ownership of.
TEST(CodecTierInterfacesTests, schemaCarriesDefinition) {
    const std::string imports = "import cajeta.wire.Schema;\n";
    const std::string body =
        "int8[] def = [(int8) 7, (int8) 9, (int8) 11];\n"
        "Schema s = heap Schema(#def);\n"
        "int8[] d = s.definition();\n"
        "return ((int32) d.count()) * 100 + (int32) d[2];";   // len 3, d[2]==11
    EXPECT_EQ(runCodec(imports, "", body), 3 * 100 + 11);
}

// Compressor + Decompressor: byte->byte block tier. Identity codec round-trip:
// decompress(compress(src), n) == src. expandedLen is int64 (a byte length).
TEST(CodecTierInterfacesTests, compressorDecompressorRoundTrip) {
    const std::string imports =
        "import cajeta.wire.Compressor;\n"
        "import cajeta.wire.Decompressor;\n";
    const std::string classes =
        "public class IdCompressor implements Compressor {\n"
        "    public IdCompressor() { }\n"
        "    public #int8[] compress(int8[] src) {\n"
        "        int64 n = (int64) src.count();\n"
        "        int8[] out = heap int8[n];\n"
        "        int64 i = 0;\n"
        "        while (i < n) { out[i] = src[i]; i = i + 1; }\n"
        "        return out;\n"
        "    }\n"
        "}\n"
        "public class IdDecompressor implements Decompressor {\n"
        "    public IdDecompressor() { }\n"
        "    public #int8[] decompress(int8[] src, int64 expandedLen) {\n"
        "        int8[] out = heap int8[expandedLen];\n"
        "        int64 i = 0;\n"
        "        while (i < expandedLen) { out[i] = src[i]; i = i + 1; }\n"
        "        return out;\n"
        "    }\n"
        "}\n";
    const std::string body =
        "int8[] raw = [(int8) 4, (int8) 8, (int8) 15, (int8) 16, (int8) 23];\n"
        "Compressor c = heap IdCompressor();\n"
        "Decompressor d = heap IdDecompressor();\n"
        "int8[] packed #= c.compress(raw);\n"
        "int8[] back #= d.decompress(packed, (int64) 5);\n"
        "return ((int32) back.count()) * 100 + (int32) back[3];";  // len 5, back[3]==16
    EXPECT_EQ(runCodec(imports, classes, body), 5 * 100 + 16);
}

// Shipped Encoder<T> still implements + round-trips unchanged (0.1 must not
// disturb it — same little-endian int32 codec, no Schema).
TEST(CodecTierInterfacesTests, shippedEncoderUnchanged) {
    const std::string imports = "import cajeta.wire.Encoder;\n";
    const std::string classes =
        "public class BoxEncoder implements Encoder<Box> {\n"
        "    public BoxEncoder() { }\n"
        "    public #int8[] encode(Box value) {\n"
        "        int8[] out = heap int8[4];\n"
        "        int32 x = value.v;\n"
        "        out[0] = (int8) (x & 255);\n"
        "        out[1] = (int8) ((x >> 8) & 255);\n"
        "        out[2] = (int8) ((x >> 16) & 255);\n"
        "        out[3] = (int8) ((x >> 24) & 255);\n"
        "        return out;\n"
        "    }\n"
        "    public #Box decode(int8[] bytes) {\n"
        "        int32 b0 = ((int32) bytes[0]) & 255;\n"
        "        int32 b1 = ((int32) bytes[1]) & 255;\n"
        "        int32 b2 = ((int32) bytes[2]) & 255;\n"
        "        int32 b3 = ((int32) bytes[3]) & 255;\n"
        "        int32 v = b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);\n"
        "        return heap Box(v);\n"
        "    }\n"
        "}\n";
    const std::string body =
        "Encoder<Box> codec = heap BoxEncoder();\n"
        "Box b = heap Box(305419896);\n"   // 0x12345678
        "int8[] bytes #= codec.encode(b);\n"
        "Box back #= codec.decode(bytes);\n"
        "return back.v - 305419896;";       // 0 iff round-trip exact
    EXPECT_EQ(runCodec(imports, classes, body), 0);
}
