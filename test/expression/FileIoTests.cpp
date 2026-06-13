// Tests for cajeta.io.file — Phase A scope (one-shot read/write + streaming
// FileReader/FileWriter). The Path class lands in Phase B; tests here use raw
// String paths.
//
// Each test uses a unique per-test temp path under TEST_TMPDIR (falls back to
// /tmp) to avoid cross-test interference. We don't unlink between tests because
// the assertions are content-based: a stale file just makes the next test fail
// fast.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

// Windows opens text-mode by default, translating \n<->\r\n and corrupting the
// byte counts these round-trip tests assert; force binary like the cajeta
// runtime does. No-op on POSIX (open has no O_BINARY).
#ifndef O_BINARY
#define O_BINARY 0
#endif

using cajeta_test::CajetaJit;

namespace {

// Portable temp root. Prefer $TEST_TMPDIR (CI override); else the OS temp dir
// (std::filesystem honors %TEMP%/%TMP% on Windows, $TMPDIR or /tmp on POSIX —
// a hard-coded "/tmp" doesn't exist on Windows, which failed every FileIoTest
// there at open()=-1). Backslashes are normalized to '/': this path is embedded
// verbatim into cajeta SOURCE string literals (File.openRead("...")) where '\'
// would start an escape sequence, and '/' is accepted by both the OS and
// cajeta's open().
const char* tmpRoot() {
    static const std::string root = [] {
        std::string p;
        if (const char* r = std::getenv("TEST_TMPDIR"); r && *r) {
            p = r;
        } else {
            p = std::filesystem::temp_directory_path().string();
        }
        std::replace(p.begin(), p.end(), '\\', '/');
        while (p.size() > 1 && p.back() == '/') p.pop_back();
        return p;
    }();
    return root.c_str();
}

std::string uniquePath(const std::string& name) {
    std::string path = tmpRoot();
    path += "/cajeta_file_io_";
    path += std::to_string((long long) ::getpid());
    path += "_";
    path += name;
    return path;
}

void writeRaw(const std::string& path, const std::string& content) {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0644);
    ASSERT_GE(fd, 0) << "open(" << path << ") failed";
    ssize_t n = ::write(fd, content.data(), content.size());
    ASSERT_EQ((size_t) n, content.size());
    ::close(fd);
}

std::string readRaw(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY | O_BINARY);
    if (fd < 0) return std::string();
    std::string out;
    char buf[1024];
    for (;;) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        out.append(buf, (size_t) n);
    }
    ::close(fd);
    return out;
}

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.F");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

std::string makeSource(const std::string& body) {
    return "package test;\n"
           "import cajeta.io.file.File;\n"
           "public final class F {\n"
           "    public static int32 run() {\n"
           "        " + body + "\n"
           "        return 0;\n"
           "    }\n"
           "}\n";
}

} // namespace

// --- Repro probe (NO File involved) — verify the int8[].count() if-return
//     shape works in isolation. Should pass with a no-op File package.
TEST(FileIoTests, reproIntArrayCountIfReturn) {
    std::string src = "package test;\n"
                      "public final class F {\n"
                      "    public static int32 run() {\n"
                      "        int8[] bytes = heap int8[5];\n"
                      "        if (bytes.count() == 5) return 1;\n"
                      "        return 0;\n"
                      "    }\n"
                      "}\n";
    EXPECT_EQ(runI32(src), 1);
}


// --- One-shot read --------------------------------------------------------

// File.readAllBytes returns the file's contents as an int8[] sized to the
// file's byte length. The returned array is owned by the caller (the `#`
// return ownership convention).
TEST(FileIoTests, readAllBytesMatchesContent) {
    std::string path = uniquePath("readAllBytes_basic.txt");
    writeRaw(path, "hello, file");
    std::string src = makeSource(
        "int8[] bytes = File.readAllBytes(\"" + path + "\");\n"
        "if (bytes.count() == 11) return 1;\n"
        "return 0;");
    EXPECT_EQ(runI32(src), 1);
}

// Empty file → zero-length int8[]. Distinct from null; the array exists.
TEST(FileIoTests, readAllBytesEmptyFile) {
    std::string path = uniquePath("readAllBytes_empty.txt");
    writeRaw(path, "");
    std::string src = makeSource(
        "int8[] bytes = File.readAllBytes(\"" + path + "\");\n"
        "if (bytes.count() == 0) return 1;\n"
        "return 0;");
    EXPECT_EQ(runI32(src), 1);
}

// --- One-shot write -------------------------------------------------------

TEST(FileIoTests, writeAllBytesRoundTripsViaReadAllBytes) {
    std::string path = uniquePath("writeAllBytes_round.txt");
    std::string src = makeSource(
        "int8[] data = { (int8) 72, (int8) 101, (int8) 108, (int8) 108, "
        "(int8) 111 };\n"  // "Hello"
        "File.writeAllBytes(\"" + path + "\", data, 5);\n"
        "int8[] back = File.readAllBytes(\"" + path + "\");\n"
        "if (back.count() == 5) return 1;\n"
        "return 0;");
    EXPECT_EQ(runI32(src), 1);
    EXPECT_EQ(readRaw(path), "Hello");
}

// --- Streaming reader: openRead → read loop → close ---------------------

TEST(FileIoTests, fileReaderReadsBytesInChunks) {
    std::string path = uniquePath("reader_chunks.txt");
    writeRaw(path, "abcdef");
    std::string src = "package test;\n"
                      "import cajeta.io.file.File;\n"
                      "import cajeta.io.file.FileReader;\n"
                      "public final class F {\n"
                      "    public static int32 run() {\n"
                      "        FileReader r = File.openRead(\"" + path + "\");\n"
                      "        int8[] buf = heap int8[16];\n"
                      "        int32 n = r.read(buf, 16);\n"
                      "        return n;\n"
                      "    }\n"
                      "}\n";
    EXPECT_EQ(runI32(src), 6);
}

TEST(FileIoTests, fileReaderPositionTracksBytesRead) {
    std::string path = uniquePath("reader_position.txt");
    writeRaw(path, "Hello, world!");
    std::string src = "package test;\n"
                      "import cajeta.io.file.File;\n"
                      "import cajeta.io.file.FileReader;\n"
                      "public final class F {\n"
                      "    public static int32 run() {\n"
                      "        FileReader r = File.openRead(\"" + path + "\");\n"
                      "        int8[] buf = heap int8[5];\n"
                      "        r.read(buf, 5);\n"
                      "        return (int32) r.position();\n"
                      "    }\n"
                      "}\n";
    EXPECT_EQ(runI32(src), 5);
}

TEST(FileIoTests, fileReaderReturnsZeroAtEof) {
    std::string path = uniquePath("reader_eof.txt");
    writeRaw(path, "hi");
    std::string src = "package test;\n"
                      "import cajeta.io.file.File;\n"
                      "import cajeta.io.file.FileReader;\n"
                      "public final class F {\n"
                      "    public static int32 run() {\n"
                      "        FileReader r = File.openRead(\"" + path + "\");\n"
                      "        int8[] buf = heap int8[8];\n"
                      "        r.read(buf, 8);\n"
                      "        int32 n2 = r.read(buf, 8);\n"
                      "        return n2;\n"
                      "    }\n"
                      "}\n";
    EXPECT_EQ(runI32(src), 0);
}

// --- Streaming writer: openWrite → write → close ------------------------

// --- Phase E: random-access File ---------------------------------------

TEST(FileIoTests, randomAccessOpenReadAndPosition) {
    std::string path = uniquePath("rax_read.txt");
    writeRaw(path, "Hello, world!");
    std::string src = "package test;\n"
                      "import cajeta.io.file.File;\n"
                      "import cajeta.io.file.OpenMode;\n"
                      "public final class F {\n"
                      "    public static int32 run() {\n"
                      "        File f = File.open(\"" + path + "\", OpenMode.READ);\n"
                      "        int8[] buf = heap int8[5];\n"
                      "        f.read(buf, 0, 5);\n"
                      "        return (int32) f.position();\n"
                      "    }\n"
                      "}\n";
    EXPECT_EQ(runI32(src), 5);
}

TEST(FileIoTests, randomAccessSeekChangesPosition) {
    std::string path = uniquePath("rax_seek.txt");
    writeRaw(path, "Hello, world!");
    std::string src = "package test;\n"
                      "import cajeta.io.file.File;\n"
                      "import cajeta.io.file.OpenMode;\n"
                      "public final class F {\n"
                      "    public static int32 run() {\n"
                      "        File f = File.open(\"" + path + "\", OpenMode.READ);\n"
                      "        f.seek(7);\n"
                      "        return (int32) f.position();\n"
                      "    }\n"
                      "}\n";
    EXPECT_EQ(runI32(src), 7);
}

TEST(FileIoTests, randomAccessSizeReturnsFileLength) {
    std::string path = uniquePath("rax_size.txt");
    writeRaw(path, "abcdefgh");  // 8 bytes
    std::string src = "package test;\n"
                      "import cajeta.io.file.File;\n"
                      "import cajeta.io.file.OpenMode;\n"
                      "public final class F {\n"
                      "    public static int32 run() {\n"
                      "        File f = File.open(\"" + path + "\", OpenMode.READ);\n"
                      "        return (int32) f.size();\n"
                      "    }\n"
                      "}\n";
    EXPECT_EQ(runI32(src), 8);
}

TEST(FileIoTests, randomAccessSeekFromEndPositionsAtEof) {
    std::string path = uniquePath("rax_seekend.txt");
    writeRaw(path, "abcdefgh");
    std::string src = "package test;\n"
                      "import cajeta.io.file.File;\n"
                      "import cajeta.io.file.OpenMode;\n"
                      "public final class F {\n"
                      "    public static int32 run() {\n"
                      "        File f = File.open(\"" + path + "\", OpenMode.READ);\n"
                      "        f.seekFromEnd(0);\n"
                      "        return (int32) f.position();\n"
                      "    }\n"
                      "}\n";
    EXPECT_EQ(runI32(src), 8);
}

TEST(FileIoTests, randomAccessWriteAtCurrentPosition) {
    std::string path = uniquePath("rax_write.txt");
    // Pre-populate with placeholder bytes that the write will overwrite.
    writeRaw(path, "xxxxx");
    std::string src = "package test;\n"
                      "import cajeta.io.file.File;\n"
                      "import cajeta.io.file.OpenMode;\n"
                      "public final class F {\n"
                      "    public static int32 run() {\n"
                      "        File f = File.open(\"" + path + "\", OpenMode.READ_WRITE);\n"
                      "        int8[] data = { (int8) 72, (int8) 105, (int8) 33 };\n"  // "Hi!"
                      "        f.write(data, 0, 3);\n"
                      "        f.close();\n"
                      "        return (int32) Path.of(\"" + path + "\").exists();\n"
                      "    }\n"
                      "}\n";
    // Just verify the write happens. Detailed byte verification via readRaw:
    runI32("package test;\n"
           "import cajeta.io.file.File;\n"
           "import cajeta.io.file.OpenMode;\n"
           "public final class F {\n"
           "    public static int32 run() {\n"
           "        File f = File.open(\"" + path + "\", OpenMode.READ_WRITE);\n"
           "        int8[] data = { (int8) 72, (int8) 105, (int8) 33 };\n"
           "        f.write(data, 0, 3);\n"
           "        f.close();\n"
           "        return 0;\n"
           "    }\n"
           "}\n");
    std::string content = readRaw(path);
    EXPECT_EQ(content.substr(0, 3), "Hi!");
}

// --- String integration on FileReader / FileWriter ---------------------

TEST(FileIoTests, fileReaderReadStringRoundTrip) {
    std::string path = uniquePath("reader_str.txt");
    writeRaw(path, "Hello, world!");
    std::string src = "package test;\n"
                      "import cajeta.io.file.File;\n"
                      "import cajeta.io.file.FileReader;\n"
                      "public final class F {\n"
                      "    public static int32 run() {\n"
                      "        FileReader r = File.openRead(\"" + path + "\");\n"
                      "        String s = r.readString(32);\n"
                      "        return s.equals(\"Hello, world!\") ? 1 : 0;\n"
                      "    }\n"
                      "}\n";
    EXPECT_EQ(runI32(src), 1);
}

TEST(FileIoTests, fileReaderReadStringSizeMatchesFile) {
    std::string path = uniquePath("reader_str_size.txt");
    writeRaw(path, "abcdef");
    std::string src = "package test;\n"
                      "import cajeta.io.file.File;\n"
                      "import cajeta.io.file.FileReader;\n"
                      "public final class F {\n"
                      "    public static int32 run() {\n"
                      "        FileReader r = File.openRead(\"" + path + "\");\n"
                      "        String s = r.readString(64);\n"
                      "        return (int32) s.count();\n"
                      "    }\n"
                      "}\n";
    EXPECT_EQ(runI32(src), 6);
}

TEST(FileIoTests, fileWriterWriteStringRoundTrip) {
    std::string path = uniquePath("writer_str.txt");
    std::string src = "package test;\n"
                      "import cajeta.io.file.File;\n"
                      "import cajeta.io.file.FileWriter;\n"
                      "import cajeta.io.file.OpenMode;\n"
                      "public final class F {\n"
                      "    public static int32 run() {\n"
                      "        FileWriter w = File.openWrite(\"" + path + "\", OpenMode.WRITE);\n"
                      "        String greeting = \"Hello from Cajeta!\";\n"
                      "        w.writeString(greeting);\n"
                      "        w.close();\n"
                      "        return 0;\n"
                      "    }\n"
                      "}\n";
    runI32(src);
    EXPECT_EQ(readRaw(path), "Hello from Cajeta!");
}

TEST(FileIoTests, fileWriterWriteStringThenReadStringRoundTrip) {
    std::string path = uniquePath("rw_str_roundtrip.txt");
    std::string src = "package test;\n"
                      "import cajeta.io.file.File;\n"
                      "import cajeta.io.file.FileReader;\n"
                      "import cajeta.io.file.FileWriter;\n"
                      "import cajeta.io.file.OpenMode;\n"
                      "public final class F {\n"
                      "    public static int32 run() {\n"
                      "        FileWriter w = File.openWrite(\"" + path + "\", OpenMode.WRITE);\n"
                      "        w.writeString(\"round trip\");\n"
                      "        w.close();\n"
                      "        FileReader r = File.openRead(\"" + path + "\");\n"
                      "        String back = r.readString(64);\n"
                      "        return back.equals(\"round trip\") ? 1 : 0;\n"
                      "    }\n"
                      "}\n";
    EXPECT_EQ(runI32(src), 1);
}

TEST(FileIoTests, fileWriterWritesBytesRoundTripsViaReadAllBytes) {
    std::string path = uniquePath("writer_basic.txt");
    std::string src = "package test;\n"
                      "import cajeta.io.file.File;\n"
                      "import cajeta.io.file.FileWriter;\n"
                      "import cajeta.io.file.OpenMode;\n"
                      "public final class F {\n"
                      "    public static int32 run() {\n"
                      "        FileWriter w = File.openWrite(\"" + path + "\", OpenMode.WRITE);\n"
                      "        int8[] data = { (int8) 72, (int8) 101, (int8) 108, "
                      "(int8) 108, (int8) 111 };\n"  // "Hello"
                      "        w.write(data, 5);\n"
                      "        w.close();\n"
                      "        return 0;\n"
                      "    }\n"
                      "}\n";
    EXPECT_EQ(runI32(src), 0);
    EXPECT_EQ(readRaw(path), "Hello");
}
