// CsvReader (codec Phase 1.2a) — streaming row/field reader tests. Each test
// supplies a body operating on a constructed `CsvReader r` over a CSV fixture
// and returning an int32 encoding of the result. Covers row/field slicing,
// quoted-field value extraction ("" collapse), CRLF \r trimming, and the
// no-phantom-trailing-row rule.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {
// Compile run() = `<body>` with `buf` (the fixture bytes) and a CsvReader `r`
// already in scope; body returns int32.
int32_t runReader(const std::string& csv, const std::string& body) {
    std::string bytes;
    for (size_t i = 0; i < csv.size(); ++i) {
        if (i) bytes += ", ";
        bytes += "(int8) " + std::to_string((int)(unsigned char)csv[i]);
    }
    const std::string n = std::to_string(csv.size());
    std::string src =
        "package test;\n"
        "import cajeta.codec.csv.CsvReader;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int8[] buf = {" + bytes + "};\n"
        "        CsvReader r = heap CsvReader(buf, (int64) " + n + ");\n"
        "        " + body + "\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}
} // namespace

// `a,b,c` — one row, three fields; field(1) == "b".
TEST(CsvReaderTests, simpleRowFields) {
    EXPECT_EQ(runReader("a,b,c",
        "r.nextRow(); int32 fc = r.fieldCount(); int8[] f1 = r.field(1);"
        " return fc * 100 + (int32) f1[0];"),
        3 * 100 + 'b');   // 398
}

// `a,b\nc,d,e` — two rows, 2 + 3 fields.
TEST(CsvReaderTests, twoRowsFieldCounts) {
    EXPECT_EQ(runReader("a,b\nc,d,e",
        "int32 rows = 0; int32 tf = 0;"
        " while (r.nextRow()) { rows = rows + 1; tf = tf + r.fieldCount(); }"
        " return rows * 100 + tf;"),
        2 * 100 + 5);     // 205
}

// `"a,b",c` — quoted field keeps its embedded comma; field(0) == "a,b" (len 3).
TEST(CsvReaderTests, quotedCommaPreserved) {
    EXPECT_EQ(runReader("\"a,b\",c",
        "r.nextRow(); int32 fc = r.fieldCount(); int8[] f0 = r.field(0);"
        " return fc * 1000 + ((int32) f0.count()) * 100 + (int32) f0[1];"),
        2 * 1000 + 3 * 100 + ',');   // 2344
}

// `"a""b",c` — doubled quote collapses to one; field(0) == a"b (len 3).
TEST(CsvReaderTests, doubledQuoteCollapsed) {
    EXPECT_EQ(runReader("\"a\"\"b\",c",
        "r.nextRow(); int32 fc = r.fieldCount(); int8[] f0 = r.field(0);"
        " return fc * 1000 + ((int32) f0.count()) * 100 + (int32) f0[1];"),
        2 * 1000 + 3 * 100 + '"');   // 2334
}

// `a,b\r\nc,d` — CRLF terminator; field(1) of row 1 == "b" (len 1, \r trimmed).
TEST(CsvReaderTests, crlfTrimmed) {
    EXPECT_EQ(runReader("a,b\r\nc,d",
        "r.nextRow(); int32 fc = r.fieldCount(); int8[] f1 = r.field(1);"
        " return fc * 100 + (int32) f1.count();"),
        2 * 100 + 1);     // 201  (202 would mean the \r leaked into the field)
}

// `a,b\n` — trailing newline yields exactly one row, not a phantom empty row.
TEST(CsvReaderTests, trailingNewlineNoPhantomRow) {
    EXPECT_EQ(runReader("a,b\n",
        "int32 rows = 0; while (r.nextRow()) { rows = rows + 1; } return rows;"),
        1);
}

// `abc` — no delimiters at all: one row, one field "abc" (len 3).
TEST(CsvReaderTests, singleFieldNoDelimiter) {
    EXPECT_EQ(runReader("abc",
        "r.nextRow(); int8[] f0 = r.field(0);"
        " return ((int32) r.fieldCount()) * 100 + (int32) f0.count();"),
        1 * 100 + 3);     // 103
}
