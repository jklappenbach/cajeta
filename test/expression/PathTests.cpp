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
#include <cstdio>
#include <fcntl.h>
#include <string>
#include <unistd.h>

namespace {
inline void writeRaw(const std::string& path, const std::string& content) {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    if (!content.empty()) ::write(fd, content.data(), content.size());
    ::close(fd);
}
} // namespace

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

// --- Phase C: stat-touching predicates ----------------------------------

TEST(PathTests, existsTrueForRealFile) {
    // Use a stable path that exists on every POSIX system.
    EXPECT_EQ(runI32(makeSource(
        "Path p = Path.of(\"/etc/passwd\");\n"
        "return p.exists() ? 1 : 0;")), 1);
}

TEST(PathTests, existsFalseForMissingFile) {
    EXPECT_EQ(runI32(makeSource(
        "Path p = Path.of(\"/this/path/does/not/exist/__cajeta__\");\n"
        "return p.exists() ? 1 : 0;")), 0);
}

TEST(PathTests, isFileTrueForRegular) {
    EXPECT_EQ(runI32(makeSource(
        "Path p = Path.of(\"/etc/passwd\");\n"
        "return p.isFile() ? 1 : 0;")), 1);
}

TEST(PathTests, isDirTrueForDirectory) {
    EXPECT_EQ(runI32(makeSource(
        "Path p = Path.of(\"/etc\");\n"
        "return p.isDir() ? 1 : 0;")), 1);
}

TEST(PathTests, isFileFalseForDirectory) {
    EXPECT_EQ(runI32(makeSource(
        "Path p = Path.of(\"/etc\");\n"
        "return p.isFile() ? 1 : 0;")), 0);
}

TEST(PathTests, isDirFalseForRegularFile) {
    EXPECT_EQ(runI32(makeSource(
        "Path p = Path.of(\"/etc/passwd\");\n"
        "return p.isDir() ? 1 : 0;")), 0);
}

// --- canonical ---------------------------------------------------------

// --- Phase D: mkdirs / delete ------------------------------------------

TEST(PathTests, mkdirsCreatesNestedDirectory) {
    // Use /tmp + pid + test-name for isolation across parallel runs.
    std::string base = "/tmp/cajeta_path_test_" + std::to_string(::getpid()) + "_mkdirs";
    std::string nested = base + "/a/b/c";
    EXPECT_EQ(runI32(makeSource(
        "Path p = Path.of(\"" + nested + "\");\n"
        "p.mkdirs();\n"
        "return p.isDir() ? 1 : 0;")), 1);
    // Clean up via cajeta:
    runI32(makeSource(
        "Path.of(\"" + nested + "\").delete();\n"
        "Path.of(\"" + base + "/a/b\").delete();\n"
        "Path.of(\"" + base + "/a\").delete();\n"
        "Path.of(\"" + base + "\").delete();\n"
        "return 0;"));
}

TEST(PathTests, deleteRemovesEmptyFile) {
    std::string path = "/tmp/cajeta_path_test_" + std::to_string(::getpid()) + "_delete.txt";
    // Set up a target file.
    writeRaw(path, "");
    EXPECT_EQ(runI32(makeSource(
        "Path p = Path.of(\"" + path + "\");\n"
        "if (!p.exists()) return -1;\n"
        "p.delete();\n"
        "return p.exists() ? 0 : 1;")), 1);
}

TEST(PathTests, canonicalResolvesSymlinkFreePathToItself) {
    // /etc/passwd is canonical on most distros; if a vendor's image
    // symlinks /etc to something else, this test won't apply — adjust
    // the path under the inevitable Linux container variability rather
    // than rely on a host-specific guarantee.
    EXPECT_EQ(runI32(makeSource(
        "Path p = Path.of(\"/etc/passwd\");\n"
        "Path c = p.canonical();\n"
        "String s = heap String(c.bytes, (int32) c.bytes.count());\n"
        "return s.equals(\"/etc/passwd\") ? 1 : 0;")), 1);
}

