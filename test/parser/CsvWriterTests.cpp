// CsvWriter (codec Phase 1.3) — RFC-4180 writer tests. Output is asserted byte-
// exact, plus a writer->CsvReader round-trip. Each test builds field values as
// int8[] literals and returns an int32 encoding of the result.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {
// int8[] literal initializer for a field value, e.g. lit("a,b"). Bracket form:
// the brace array-initializer was retired (collection-literals 5) — this
// helper generated braces programmatically, so that migration's tree-wide
// snippet rewrite could not see it.
std::string lit(const std::string& s) {
    std::string r = "[";
    for (size_t i = 0; i < s.size(); ++i) {
        if (i) r += ", ";
        r += "(int8) " + std::to_string((int)(unsigned char)s[i]);
    }
    r += "]";
    return r;
}

// Compile run() = `<body>` returning int32, with the given imports.
int32_t runCsv(const std::string& imports, const std::string& body) {
    std::string src =
        "package test;\n" + imports +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        " + body + "\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

constexpr const char* W = "import cajeta.codec.csv.CsvWriter;\n";
constexpr const char* WR =
    "import cajeta.codec.csv.CsvWriter;\n"
    "import cajeta.codec.csv.CsvReader;\n";
} // namespace

// Two plain fields + endRow → "a,b\n" (4 bytes; byte[1] == ',').
TEST(CsvWriterTests, plainRow) {
    EXPECT_EQ(runCsv(W,
        "int8[] fa = " + lit("a") + "; int8[] fb = " + lit("b") + ";"
        " CsvWriter w = heap CsvWriter(); w.writeField(fa); w.writeField(fb); w.endRow();"
        " int8[] out = w.toBytes();"
        " return ((int32) out.count()) * 1000 + (int32) out[1];"),
        4 * 1000 + ',');   // "a,b\n"  -> 4044
}

// A field containing a comma is quoted: `"a,b",c\n` (8 bytes; byte[0] == '"').
TEST(CsvWriterTests, quotesFieldWithComma) {
    EXPECT_EQ(runCsv(W,
        "int8[] f0 = " + lit("a,b") + "; int8[] f1 = " + lit("c") + ";"
        " CsvWriter w = heap CsvWriter(); w.writeField(f0); w.writeField(f1); w.endRow();"
        " int8[] out = w.toBytes();"
        " return ((int32) out.count()) * 100 + (int32) out[0];"),
        8 * 100 + '"');    // `"a,b",c\n` -> 834
}

// A field containing a quote is quoted with the inner quote doubled:
// a"b -> `"a""b"` (6 bytes; byte[2] == '"' from the escape).
TEST(CsvWriterTests, escapesInnerQuote) {
    EXPECT_EQ(runCsv(W,
        "int8[] f0 = " + lit("a\"b") + ";"
        " CsvWriter w = heap CsvWriter(); w.writeField(f0); w.endRow();"
        " int8[] out = w.toBytes();"
        " return ((int32) out.count()) * 100 + (int32) out[2];"),
        7 * 100 + '"');    // `"a""b"\n` (6 + LF = 7) -> 734
}

// Round-trip: write a row whose first field needs quoting, read it back via
// CsvReader; field(0) decodes to "a,b" (len 3), 2 fields, 1 row.
TEST(CsvWriterTests, roundTripThroughReader) {
    EXPECT_EQ(runCsv(WR,
        "int8[] f0 = " + lit("a,b") + "; int8[] f1 = " + lit("c") + ";"
        " CsvWriter w = heap CsvWriter(); w.writeField(f0); w.writeField(f1); w.endRow();"
        " int8[] csv = w.toBytes();"
        " CsvReader r = heap CsvReader(csv, (int64) csv.count());"
        " r.nextRow(); int32 fc = r.fieldCount(); int8[] g0 = r.field(0);"
        " return fc * 1000 + ((int32) g0.count()) * 100 + (int32) g0[1];"),
        2 * 1000 + 3 * 100 + ',');   // fc 2, "a,b" len 3, g0[1]==',' -> 2344
}
