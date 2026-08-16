// Codec Phase 1.2b-b2 — Tier-1 CSV typed-bind synthesizer.
//
// `Csv.parse<T[]>(bytes, length)` is a method-level template whose body is
// SYNTHESIZED per-element-T at instantiation time: the compiler walks T's
// declared fields, builds a header→column index map, and emits a two-pass
// row bind over `CsvReader` (pass 1 reads the header + counts data rows;
// pass 2 fills a freshly-allocated `T[]`). No value tree, no reflection.
//
// b2 scope: name-match bind of primitive fields
// (int32/int64/float64/boolean/String); columns matched to fields by header
// name (order-independent); unmapped header columns are simply not recorded
// (basic projection — fail-loud / @CsvRequired land in b3).
//
// See docs/specification/codec/Codecs.md § CSV and
// agents/cajeta/codecs/codecs-plan.md § 1.2b.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {
// Compile `<classDecls>` + a `run()` that has the CSV fixture bytes in `buf`
// and its length in `n`; `<body>` returns int32.
int32_t runCsv(const std::string& classDecls,
               const std::string& csv,
               const std::string& body) {
    std::string bytes;
    for (size_t i = 0; i < csv.size(); ++i) {
        if (i) bytes += ", ";
        bytes += "(int8) " + std::to_string((int)(unsigned char)csv[i]);
    }
    const std::string len = std::to_string(csv.size());
    std::string src =
        "package test;\n"
        "import cajeta.codec.csv.Csv;\n"
        + classDecls +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int8[] buf = [" + bytes + "];\n"
        "        int64 n = (int64) " + len + ";\n"
        "        " + body + "\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}
} // namespace

// One int32 column, two data rows — the smallest typed bind.
//   id\n42\n7   →  [{id:42}, {id:7}]
TEST(CsvSynthesizerTests, parseSingleInt32ColumnTwoRows) {
    EXPECT_EQ(runCsv(
        "public class Box { public int32 id; }\n",
        "id\n42\n7",
        "Box[] rows #= Csv.parse<Box[]>(buf, n);"
        " return ((int32) rows.count()) * 100 + rows[0].id + rows[1].id;"),
        2 * 100 + 42 + 7);   // 249
}

// int64 column.
TEST(CsvSynthesizerTests, parseInt64Column) {
    EXPECT_EQ(runCsv(
        "public class Box { public int64 v; }\n",
        "v\n1000000\n3",
        "Box[] rows #= Csv.parse<Box[]>(buf, n);"
        " return (int32) (rows[0].v / (int64) 1000000) + (int32) rows[1].v;"),
        1 + 3);   // 4
}

// float64 column with a fractional part.
TEST(CsvSynthesizerTests, parseFloat64Column) {
    EXPECT_EQ(runCsv(
        "public class Box { public float64 x; }\n",
        "x\n3.5\n-2.25",
        "Box[] rows #= Csv.parse<Box[]>(buf, n);"
        " return (int32) (rows[0].x * 100.0) + (int32) (rows[1].x * 100.0);"),
        350 + (-225));   // 125
}

// boolean column — "true"/"false".
TEST(CsvSynthesizerTests, parseBooleanColumn) {
    EXPECT_EQ(runCsv(
        "public class Box { public boolean b; }\n",
        "b\ntrue\nfalse",
        "Box[] rows #= Csv.parse<Box[]>(buf, n);"
        " int32 r = 0;"
        " if (rows[0].b) { r = r + 10; }"
        " if (rows[1].b) { r = r + 1; }"
        " return r;"),
        10);
}

// String column — value bytes become a heap String.
TEST(CsvSynthesizerTests, parseStringColumn) {
    EXPECT_EQ(runCsv(
        "public class Box { public cajeta.lang.String s; }\n",
        "s\nhi\nyo",
        "Box[] rows #= Csv.parse<Box[]>(buf, n);"
        " return (int32) rows[0].s.byteAt(0) * 100 + (int32) rows[1].s.byteAt(0);"),
        'h' * 100 + 'y');   // 10421
}

// Header order need not match field declaration order — columns bind by name.
//   b,a\n10,20\n30,40  →  a from col 1, b from col 0
TEST(CsvSynthesizerTests, parseColumnsBoundByName) {
    EXPECT_EQ(runCsv(
        "public class Rec { public int32 a; public int32 b; }\n",
        "b,a\n10,20\n30,40",
        "Rec[] rows #= Csv.parse<Rec[]>(buf, n);"
        " return rows[0].a * 100 + rows[0].b + rows[1].a + rows[1].b;"),
        20 * 100 + 10 + 40 + 30);   // 2080
}

// A header column with no matching field is ignored (basic projection).
//   a,x\n5,9  →  Box{a} binds a=5, ignores x
TEST(CsvSynthesizerTests, unmappedHeaderColumnIgnored) {
    EXPECT_EQ(runCsv(
        "public class Box { public int32 a; }\n",
        "a,x\n5,9",
        "Box[] rows #= Csv.parse<Box[]>(buf, n);"
        " return ((int32) rows.count()) * 100 + rows[0].a;"),
        1 * 100 + 5);   // 105
}

// ---- b3: projection of T fields + @CsvRequired fail-loud ----

// A T field with no matching header column and no @CsvRequired stays at its
// default (projection of fields) — no throw.
//   a\n5  →  Box{a,b}: a=5, b defaults to 0
TEST(CsvSynthesizerTests, unmappedFieldDefaultsWhenNotRequired) {
    EXPECT_EQ(runCsv(
        "public class Box { public int32 a; public int32 b; }\n",
        "a\n5",
        "Box[] rows #= Csv.parse<Box[]>(buf, n);"
        " return rows[0].a * 100 + rows[0].b;"),
        5 * 100 + 0);   // 500
}

// A @CsvRequired field whose column IS present binds normally — no throw.
TEST(CsvSynthesizerTests, requiredColumnPresentBinds) {
    EXPECT_EQ(runCsv(
        "public class Box { @CsvRequired public int32 id; }\n",
        "id\n42",
        "Box[] rows #= Csv.parse<Box[]>(buf, n);"
        " return rows[0].id;"),
        42);
}

// A @CsvRequired field absent from the header fails loud — CsvParseException.
//   header has `x`, not the required `id`
TEST(CsvSynthesizerTests, requiredColumnMissingThrows) {
    EXPECT_EQ(runCsv(
        "public class Box { @CsvRequired public int32 id; public int32 x; }\n",
        "x\n9",
        "try {"
        "   Box[] rows #= Csv.parse<Box[]>(buf, n);"
        "   return 0;"
        " } catch (cajeta.codec.csv.CsvParseException e) {"
        "   return 1;"
        " }"),
        1);
}

// Two required fields, one present, one missing — still throws.
TEST(CsvSynthesizerTests, requiredOnePresentOneMissingThrows) {
    EXPECT_EQ(runCsv(
        "public class Pair { @CsvRequired public int32 left;"
        " @CsvRequired public int32 right; }\n",
        "left\n1",
        "try {"
        "   Pair[] rows #= Csv.parse<Pair[]>(buf, n);"
        "   return 0;"
        " } catch (cajeta.codec.csv.CsvParseException e) {"
        "   return 1;"
        " }"),
        1);
}

// ---- b4: annotation surface (@CsvColumn / @CsvNamingStrategy / @CsvIgnore / @CsvAlias) ----

// @CsvColumn("user_id") renames the header key the field binds to.
TEST(CsvSynthesizerTests, csvColumnRename) {
    EXPECT_EQ(runCsv(
        "public class Box { @CsvColumn(\"user_id\") public int32 id; }\n",
        "user_id\n42",
        "Box[] rows #= Csv.parse<Box[]>(buf, n);"
        " return rows[0].id;"),
        42);
}

// Class-level @CsvNamingStrategy("SNAKE_CASE") matches camelCase fields to
// snake_case header keys.
TEST(CsvSynthesizerTests, csvNamingStrategySnakeCase) {
    EXPECT_EQ(runCsv(
        "@CsvNamingStrategy(\"SNAKE_CASE\")\n"
        "public class User { public int32 firstName; }\n",
        "first_name\n7",
        "User[] rows #= Csv.parse<User[]>(buf, n);"
        " return rows[0].firstName;"),
        7);
}

// KEBAB_CASE uses '-' separators.
TEST(CsvSynthesizerTests, csvNamingStrategyKebabCase) {
    EXPECT_EQ(runCsv(
        "@CsvNamingStrategy(\"KEBAB_CASE\")\n"
        "public class User { public int32 firstName; }\n",
        "first-name\n8",
        "User[] rows #= Csv.parse<User[]>(buf, n);"
        " return rows[0].firstName;"),
        8);
}

// Field-level @CsvColumn wins over the class naming strategy — the snake-case
// key "first_name" no longer binds; only the explicit column does.
TEST(CsvSynthesizerTests, csvColumnOverridesStrategy) {
    EXPECT_EQ(runCsv(
        "@CsvNamingStrategy(\"SNAKE_CASE\")\n"
        "public class User { @CsvColumn(\"EXPLICIT\") public int32 firstName; }\n",
        "EXPLICIT\n9",
        "User[] rows #= Csv.parse<User[]>(buf, n);"
        " return rows[0].firstName;"),
        9);
}

// @CsvIgnore drops a field from the bind entirely — a matching header column
// does not populate it (stays at default).
TEST(CsvSynthesizerTests, csvIgnoreField) {
    EXPECT_EQ(runCsv(
        "public class Box { public int32 a; @CsvIgnore public int32 secret; }\n",
        "a,secret\n5,99",
        "Box[] rows #= Csv.parse<Box[]>(buf, n);"
        " return rows[0].a * 100 + rows[0].secret;"),
        5 * 100 + 0);   // 500 — secret ignored
}

// @CsvAlias adds alternate header keys accepted on read.
TEST(CsvSynthesizerTests, csvAliasReadsAlternateKey) {
    EXPECT_EQ(runCsv(
        "public class Box { @CsvAlias({\"uid\", \"user-id\"}) public int32 id; }\n",
        "uid\n42",
        "Box[] rows #= Csv.parse<Box[]>(buf, n);"
        " return rows[0].id;"),
        42);
}
