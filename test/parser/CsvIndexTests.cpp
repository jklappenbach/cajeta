// CsvIndex (codec Phase 1.1) — stage-1 structural index tests. Each fixture is
// fed as an int8[] and CsvIndex.build() runs over it; run() encodes the result
// as cnt*10000 + idx[0]*100 + idx[1] so a single int32 pins the count and the
// first two recorded positions. Structural delimiters are unquoted ',' / '\n';
// delimiters inside quoted fields (incl. across "" escapes) are excluded.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {
// Compile a run() that builds the index over `csv` and returns the encoding.
int32_t runIndex(const std::string& csv) {
    std::string bytes;
    for (size_t i = 0; i < csv.size(); ++i) {
        if (i) bytes += ", ";
        bytes += "(int8) " + std::to_string((int)(unsigned char)csv[i]);
    }
    const std::string n = std::to_string(csv.size());
    std::string src =
        "package test;\n"
        "import cajeta.codec.csv.CsvIndex;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int8[] buf = {" + bytes + "};\n"
        "        int32[] idx = heap int32[" + n + "];\n"
        "        int32 cnt = CsvIndex.build(buf, (int64) " + n + ", idx);\n"
        "        int32 i0 = 0; if (cnt > 0) { i0 = idx[0]; }\n"
        "        int32 i1 = 0; if (cnt > 1) { i1 = idx[1]; }\n"
        "        return cnt * 10000 + i0 * 100 + i1;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

constexpr int32_t enc(int cnt, int i0, int i1) { return cnt * 10000 + i0 * 100 + i1; }
} // namespace

// `a,b,c` — two unquoted commas at 1 and 3 (scalar tail path).
TEST(CsvIndexTests, simpleRow) {
    EXPECT_EQ(runIndex("a,b,c"), enc(2, 1, 3));
}

// `a,b\nc,d` — comma 1, newline 3, comma 5; cnt 3.
TEST(CsvIndexTests, twoRows) {
    EXPECT_EQ(runIndex("a,b\nc,d"), enc(3, 1, 3));
}

// `"a,b",c` — the comma at 2 is inside quotes (not structural); only the comma
// at 5 (after the closing quote) is recorded.
TEST(CsvIndexTests, quotedCommaIgnored) {
    EXPECT_EQ(runIndex("\"a,b\",c"), enc(1, 5, 0));
}

// `"a""b",c` — doubled-quote escape inside the field; only the comma at 6 is
// structural.
TEST(CsvIndexTests, doubledQuoteEscape) {
    EXPECT_EQ(runIndex("\"a\"\"b\",c"), enc(1, 6, 0));
}

// 16-byte block exercises the SIMD path: `"a,b",cdefghijkl`. Quoted comma at 2
// masked; structural comma at 5.
TEST(CsvIndexTests, simdBlockQuotedComma) {
    EXPECT_EQ(runIndex("\"a,b\",cdefghijkl"), enc(1, 5, 0));
}

// SIMD block (16 'a's, no delimiter) + scalar tail: comma at 16.
TEST(CsvIndexTests, simdBlockThenTailComma) {
    EXPECT_EQ(runIndex("aaaaaaaaaaaaaaaa,b"), enc(1, 16, 0));
}

// Quoted field opens in the SIMD block and closes in the tail: the comma at 14
// (inside quotes, in-block) is masked and `inq` carries true across the block
// boundary; the comma at 17 (after the close at 16) is structural.
// bytes: 0-11 'a', 12 '"', 13 'a', 14 ',', 15 'b', 16 '"', 17 ',', 18 'c'
TEST(CsvIndexTests, quoteCarriesAcrossBlockBoundary) {
    EXPECT_EQ(runIndex("aaaaaaaaaaaa\"a,b\",c"), enc(1, 17, 0));
}
