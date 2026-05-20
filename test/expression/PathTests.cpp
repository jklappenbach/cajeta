// Tests for cajeta.io.file.Path — Phase B scope.
//
// Pure-path manipulation only here (no syscalls). Stat-touching tests
// (Path.exists, isFile, isDir, isSymlink, info) land with Phase C
// alongside the FileInfo class.
//
// Path equality / String returns use the class String's `equals`
// method so byte comparison is straightforward — return 1 on match,
// 0 otherwise, and the test asserts EXPECT_EQ(runI32(src), 1).

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.P");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

std::string makeSource(const std::string& body) {
    return "package test;\n"
           "import cajeta.lang.String;\n"
           "import cajeta.io.file.Path;\n"
           "public final class P {\n"
           "    public static int32 run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

} // namespace

// --- Construction + absolute/relative ----------------------------------

TEST(PathTests, ofThenIsAbsoluteTrueForLeadingSlash) {
    EXPECT_EQ(runI32(makeSource(
        "Path p = Path.of(\"/etc/passwd\");\n"
        "return p.isAbsolute() ? 1 : 0;")), 1);
}

TEST(PathTests, ofThenIsAbsoluteFalseForRelative) {
    EXPECT_EQ(runI32(makeSource(
        "Path p = Path.of(\"foo/bar\");\n"
        "return p.isAbsolute() ? 1 : 0;")), 0);
}

TEST(PathTests, ofThenIsRelativeTrueForBareName) {
    EXPECT_EQ(runI32(makeSource(
        "Path p = Path.of(\"data.json\");\n"
        "return p.isRelative() ? 1 : 0;")), 1);
}

TEST(PathTests, ofEmptyStringIsRelative) {
    EXPECT_EQ(runI32(makeSource(
        "Path p = Path.of(\"\");\n"
        "return p.isRelative() ? 1 : 0;")), 1);
}

// --- name / stem / extension -------------------------------------------

TEST(PathTests, nameOfSimplePath) {
    EXPECT_EQ(runI32(makeSource(
        "Path p = Path.of(\"/foo/bar/baz.txt\");\n"
        "return p.name().equals(\"baz.txt\") ? 1 : 0;")), 1);
}

TEST(PathTests, nameOfRootPath) {
    // "/foo" → name is "foo".
    EXPECT_EQ(runI32(makeSource(
        "Path p = Path.of(\"/foo\");\n"
        "return p.name().equals(\"foo\") ? 1 : 0;")), 1);
}

TEST(PathTests, nameOfBareFile) {
    EXPECT_EQ(runI32(makeSource(
        "Path p = Path.of(\"file.cajeta\");\n"
        "return p.name().equals(\"file.cajeta\") ? 1 : 0;")), 1);
}

TEST(PathTests, stemStripsTrailingExtension) {
    EXPECT_EQ(runI32(makeSource(
        "Path p = Path.of(\"/x/y/data.json\");\n"
        "return p.stem().equals(\"data\") ? 1 : 0;")), 1);
}

TEST(PathTests, stemNoExtensionReturnsWholeName) {
    EXPECT_EQ(runI32(makeSource(
        "Path p = Path.of(\"/etc/passwd\");\n"
        "return p.stem().equals(\"passwd\") ? 1 : 0;")), 1);
}

TEST(PathTests, extensionLastDotSuffix) {
    EXPECT_EQ(runI32(makeSource(
        "Path p = Path.of(\"archive.tar.gz\");\n"
        "return p.extension().equals(\"gz\") ? 1 : 0;")), 1);
}

TEST(PathTests, extensionNoneIsEmpty) {
    EXPECT_EQ(runI32(makeSource(
        "Path p = Path.of(\"README\");\n"
        "return p.extension().isEmpty() ? 1 : 0;")), 1);
}

// --- parent ------------------------------------------------------------

TEST(PathTests, parentStripsLastSegment) {
    EXPECT_EQ(runI32(makeSource(
        "Path p = Path.of(\"/foo/bar/baz.txt\");\n"
        "Path par = p.parent();\n"
        "// Re-format par's bytes through String to compare.\n"
        "String s = heap String(par.bytes, (int32) par.bytes.count());\n"
        "return s.equals(\"/foo/bar\") ? 1 : 0;")), 1);
}

TEST(PathTests, parentOfRootIsRoot) {
    EXPECT_EQ(runI32(makeSource(
        "Path p = Path.of(\"/\");\n"
        "Path par = p.parent();\n"
        "String s = heap String(par.bytes, (int32) par.bytes.count());\n"
        "return s.equals(\"/\") ? 1 : 0;")), 1);
}

// --- operator/ (join) --------------------------------------------------

// Test using resolve() (named alias). Operator/ via the BinaryOp
// dispatch is exercised separately below — same body, parser path
// differs.
TEST(PathTests, resolveAppendsSegmentWithSeparator) {
    EXPECT_EQ(runI32(makeSource(
        "Path p = Path.of(\"/etc\");\n"
        "Path q = p.resolve(\"passwd\");\n"
        "String s = heap String(q.bytes, (int32) q.bytes.count());\n"
        "return s.equals(\"/etc/passwd\") ? 1 : 0;")), 1);
}

TEST(PathTests, resolveOntoRelativePath) {
    EXPECT_EQ(runI32(makeSource(
        "Path p = Path.of(\"src\");\n"
        "Path q = p.resolve(\"main.cajeta\");\n"
        "String s = heap String(q.bytes, (int32) q.bytes.count());\n"
        "return s.equals(\"src/main.cajeta\") ? 1 : 0;")), 1);
}

