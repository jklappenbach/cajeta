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

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using cajeta_test::CajetaJit;

namespace {

const char* tmpRoot() {
    const char* r = std::getenv("TEST_TMPDIR");
    return r && *r ? r : "/tmp";
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
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    ASSERT_GE(fd, 0) << "open(" << path << ") failed";
    ssize_t n = ::write(fd, content.data(), content.size());
    ASSERT_EQ((size_t) n, content.size());
    ::close(fd);
}

std::string readRaw(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY);
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
                      "        int8[] bytes = new int8[5];\n"
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
                      "        int8[] buf = new int8[16];\n"
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
                      "        int8[] buf = new int8[5];\n"
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
                      "        int8[] buf = new int8[8];\n"
                      "        r.read(buf, 8);\n"
                      "        int32 n2 = r.read(buf, 8);\n"
                      "        return n2;\n"
                      "    }\n"
                      "}\n";
    EXPECT_EQ(runI32(src), 0);
}

// --- Streaming writer: openWrite → write → close ------------------------

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
