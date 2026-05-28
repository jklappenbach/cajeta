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
#include <filesystem>
#include <string>
#include <unistd.h>

namespace {
inline void writeRaw(const std::string& path, const std::string& content) {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    if (!content.empty()) ::write(fd, content.data(), content.size());
    ::close(fd);
}

// A unique temp path in forward-slash form. Forward slashes are safe inside
// cajeta string literals (backslashes would be escape sequences) and are
// accepted by stat/open on both POSIX and Windows. Earlier stat-touching
// tests hardcoded POSIX paths like "/etc/passwd" / "/etc" / "/tmp", which
// don't exist on Windows; deriving a real path from temp_directory_path()
// keeps them portable and actually exercises the platform's stat.
inline std::string tmpFwd(const std::string& name) {
    static int ctr = 0;
    auto p = std::filesystem::temp_directory_path() /
             ("cajeta_path_" + std::to_string((long long) ::getpid()) + "_" +
              std::to_string(ctr++) + "_" + name);
    return p.generic_string();
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
    std::string path = tmpFwd("exists.txt");
    writeRaw(path, "x");
    EXPECT_EQ(runI32(makeSource(
        "Path p = Path.of(\"" + path + "\");\n"
        "return p.exists() ? 1 : 0;")), 1);
    std::filesystem::remove(path);
}

TEST(PathTests, existsFalseForMissingFile) {
    EXPECT_EQ(runI32(makeSource(
        "Path p = Path.of(\"/this/path/does/not/exist/__cajeta__\");\n"
        "return p.exists() ? 1 : 0;")), 0);
}

TEST(PathTests, isFileTrueForRegular) {
    std::string path = tmpFwd("isfile.txt");
    writeRaw(path, "x");
    EXPECT_EQ(runI32(makeSource(
        "Path p = Path.of(\"" + path + "\");\n"
        "return p.isFile() ? 1 : 0;")), 1);
    std::filesystem::remove(path);
}

TEST(PathTests, isDirTrueForDirectory) {
    std::string dir = tmpFwd("isdir_d");
    std::filesystem::create_directories(dir);
    EXPECT_EQ(runI32(makeSource(
        "Path p = Path.of(\"" + dir + "\");\n"
        "return p.isDir() ? 1 : 0;")), 1);
    std::filesystem::remove(dir);
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
    // /etc/passwd is canonical on Linux only: macOS resolves /etc -> /private/etc
    // (so canonical() differs from the input), and Windows has no such path. The
    // assertion is about realpath()-style canonicalization of an already-canonical
    // path, which is a Linux-specific guarantee here.
#ifndef __linux__
    GTEST_SKIP() << "canonical(/etc/passwd) == /etc/passwd is Linux-specific";
#else
    // /etc/passwd is canonical on most distros; if a vendor's image
    // symlinks /etc to something else, this test won't apply — adjust
    // the path under the inevitable Linux container variability rather
    // than rely on a host-specific guarantee.
    EXPECT_EQ(runI32(makeSource(
        "Path p = Path.of(\"/etc/passwd\");\n"
        "Path c = p.canonical();\n"
        "String s = heap String(c.bytes, (int32) c.bytes.count());\n"
        "return s.equals(\"/etc/passwd\") ? 1 : 0;")), 1);
#endif
}

