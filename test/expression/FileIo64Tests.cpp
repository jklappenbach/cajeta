// FileIo64Tests — cajeta-llama plan Unit 4 (4.1.1–4.1.5, 4.2.5): 64-bit file
// I/O and memory mapping (spec §3.1, §3.2, §3.7, §3.8).
//
// The unit closes the 2^31 truncations on the file path (a bit-31 read length
// goes negative through the int32 casts and returns 0 — indistinguishable from
// EOF), the AllocaInst-only array-argument lowering that makes
// `File.writeAllBytes(path, h.data, n)` write from the slot address instead of
// the array (defect writeallbytes-field-arg), and adds the owning `MappedFile`
// type (decision 13.4) that Unit 5's safetensors loading is built on.
//
// The ≥2 GiB and huge-mapping tests allocate/map multi-GiB transiently; the
// files are sparse so the disk cost is metadata only.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __linux__
#include <fstream>
#endif

// MinGW's CRT has no pwrite(2), and its off_t is 32 bits — the exact width
// these tests exist to exercise. The Windows equivalents (_lseeki64,
// _chsize_s) are 64-bit clean; FSCTL_SET_SPARSE is what keeps the multi-GiB
// fixtures metadata-only on NTFS, the way ftruncate already does on POSIX.
#ifdef _WIN32
#include <io.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#endif

#ifndef O_BINARY
#define O_BINARY 0
#endif

using cajeta_test::CajetaJit;

namespace {

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
    path += "/cajeta_file_io64_";
    path += std::to_string((long long) ::getpid());
    path += "_";
    path += name;
    return path;
}

std::string readRaw(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY | O_BINARY);
    if (fd < 0) return std::string();
    std::string out;
    char buf[4096];
    for (;;) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        out.append(buf, (size_t) n);
    }
    ::close(fd);
    return out;
}

// Mark the file sparse before it is extended. A no-op on POSIX, where
// ftruncate past EOF already leaves a hole; on NTFS the flag is what turns
// the 2 GiB / 8 GiB fixtures from a reserve-and-zero-fill into metadata.
void markSparse(int fd) {
#ifdef _WIN32
    HANDLE h = (HANDLE) ::_get_osfhandle(fd);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD returned = 0;
        ::DeviceIoControl(h, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0,
                          &returned, nullptr);
    }
#else
    (void) fd;
#endif
}

// Set the file length. ::ftruncate takes off_t, which is 32 bits on MinGW —
// a 2^31+16 length would wrap negative there, silently defeating the very
// bit-31 case under test.
int truncateTo(int fd, int64_t size) {
#ifdef _WIN32
    return ::_chsize_s(fd, size) == 0 ? 0 : -1;
#else
    return ::ftruncate(fd, (off_t) size);
#endif
}

// Positional write. MinGW has no pwrite(2); these fixtures are
// single-threaded and own the descriptor, so seek-then-write is exact.
int64_t writeAt(int fd, const void* buf, size_t len, int64_t offset) {
#ifdef _WIN32
    if (::_lseeki64(fd, offset, SEEK_SET) < 0) return -1;
    return (int64_t) ::_write(fd, buf, (unsigned int) len);
#else
    return (int64_t) ::pwrite(fd, buf, len, (off_t) offset);
#endif
}

// Create a sparse file of `size` bytes whose last `markLen` bytes are `mark`.
// Sparse: only the tail extent occupies disk.
void makeSparseWithTailMark(const std::string& path, int64_t size,
                            const char* mark, size_t markLen) {
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_BINARY, 0644);
    ASSERT_GE(fd, 0) << "open(" << path << ")";
    markSparse(fd);
    ASSERT_EQ(truncateTo(fd, size), 0);
    ASSERT_EQ(writeAt(fd, mark, markLen, size - (int64_t) markLen),
              (int64_t) markLen);
    ::close(fd);
}

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

#ifdef __linux__
long rssKb() {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            return std::atol(line.c_str() + 6);
        }
    }
    return -1;
}

// Number of lines in /proc/self/maps — a mapping-count proxy for the
// unmap-on-drop test.
long mapCount() {
    std::ifstream f("/proc/self/maps");
    long n = 0;
    std::string line;
    while (std::getline(f, line)) n++;
    return n;
}
#endif

const char* PRE =
    "package test;\n"
    "import cajeta.io.file.File;\n"
    "import cajeta.io.file.OpenMode;\n";

} // namespace

// 4.1.1 — a read of ≥2 GiB in one call transfers the full length (spec 3.2).
// The length 2^31+16 has bit 31 set: through today's int32 cast
// (MethodCallExpression.cpp `file.len32`) it goes negative and the native
// helper returns 0 — indistinguishable from EOF. The file is sparse (holes
// read as zeros); only the 8-byte tail marker is real, and finding it past
// the 2 GiB line proves the full length transferred.
//
// Transient cost: one 2 GiB host buffer, zero-filled then overwritten.
//
// RED until 4.2.1.
TEST(FileIo64Tests, readHonorsLengthPastBit31) {
    const std::string path = uniquePath("big_read.bin");
    const int64_t size = (int64_t) 2147483648LL + 16;   // 2^31 + 16
    static const char MARK[8] = {90, 91, 92, 93, 94, 95, 96, 97};
    makeSparseWithTailMark(path, size, MARK, sizeof(MARK));

    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        // 2^31 + 16, built by arithmetic so no literal-width questions arise.
        "        int64 want = 1024;\n"
        "        want = want * 1024 * 1024 * 2 + 16;\n"
        "        File f = File.open(\"" + path + "\", OpenMode.READ);\n"
        "        if (f.size() != want) { return -1; }\n"
        "        int8[] buf = heap int8[want];\n"
        "        int64 got = f.read(buf, 0, want);\n"
        "        f.close();\n"
        "        if (got == 0) { return -2; }\n"       // today's failure: bit-31 -> \"EOF\"
        "        if (got != want) { return -3; }\n"
        "        if (buf[want - 8] != 90) { return -4; }\n"
        "        if (buf[want - 1] != 97) { return -5; }\n"
        "        if (buf[12345] != 0) { return -6; }\n"  // hole reads as zeros
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
    ::unlink(path.c_str());
}

// 4.1.2 — File.writeAllBytes writes correct bytes when its data argument is a
// struct FIELD (spec 3.7, defect writeallbytes-field-arg): `loadArrayDataPtr`
// unwraps only AllocaInst today (MethodCallExpression.cpp:3400-3408), so a
// field argument — a DotExpression GEP — passes the slot address and the file
// receives adjacent struct memory instead of the array. The String-argument
// path was already fixed the same way at :386 (loadIfLValue); this pins the
// array-argument analog.
//
// RED until 4.2.2.
TEST(FileIo64Tests, writeAllBytesFromStructField) {
    const std::string path = uniquePath("field_arg.bin");
    std::string src = std::string(PRE) +
        "public final class H {\n"
        "    public int8[] data;\n"
        "    public H() {\n"
        "        this.data = heap int8[3];\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        H h = heap H();\n"
        "        h.data[0] = 65;\n"
        "        h.data[1] = 66;\n"
        "        h.data[2] = 67;\n"
        "        File.writeAllBytes(\"" + path + "\", h.data, 3);\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    ASSERT_EQ(runI32(src), 1);
    EXPECT_EQ(readRaw(path), "ABC");
    ::unlink(path.c_str());
}

// 4.1.3 — a runtime-constructed path (concatenation, substring) opens
// correctly, i.e. is NUL-terminated at the native seam (spec 3.8, defect
// runtime-path-nul-termination). Both construction forms are exercised: a
// `+`-concatenated path, and a mode-2 windowed substring — the form with no
// NUL at its window end, which `__cajeta_string_cstr` must materialize.
TEST(FileIo64Tests, runtimeConstructedPathOpens) {
    const std::string base = uniquePath("rt");
    const std::string cat = base + "_concat.bin";
    const std::string sub = base + "_sub.bin";
    // substring test: the cajeta source builds "<sub>###" then takes the
    // prefix window of byteLength(sub).
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int8[] d = heap int8[3];\n"
        "        d[0] = 88; d[1] = 89; d[2] = 90;\n"           // "XYZ"
        // concatenation-built path
        "        String p1 = \"" + base + "\" + \"_concat.bin\";\n"
        "        File.writeAllBytes(p1, d, 3);\n"
        "        int8[] r1 = File.readAllBytes(p1);\n"
        "        if (r1.count() != 3) { return -1; }\n"
        "        if (r1[0] != 88) { return -2; }\n"
        // substring-built path (windowed view; no NUL at the window end)
        "        String longer = \"" + sub + "###\";\n"
        "        String p2 = longer.substring(0, " + std::to_string(sub.size()) + ");\n"
        "        File.writeAllBytes(p2, d, 3);\n"
        "        int8[] r2 = File.readAllBytes(p2);\n"
        "        if (r2.count() != 3) { return -3; }\n"
        "        if (r2[2] != 90) { return -4; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
    EXPECT_EQ(readRaw(cat), "XYZ");
    EXPECT_EQ(readRaw(sub), "XYZ");
    ::unlink(cat.c_str());
    ::unlink(sub.c_str());
}

// 4.1.4 — a mapped file reads correctly at arbitrary offsets, and the mapping
// is released on drop (spec 3.1, decision 13.4: an owning MappedFile with a
// KernelBuffer-style RAII drop). Release is observed via /proc/self/maps: the
// probe maps and drops the file 200 times in a scoped helper; if unmap never
// runs, ~200 mappings accumulate.
//
// RED until 4.2.4: cajeta.io.file.MappedFile does not exist.
TEST(FileIo64Tests, mappedFileReadsAtOffsetsAndUnmapsOnDrop) {
    const std::string path = uniquePath("mapped.bin");
    // 1 MiB patterned file: byte[i] = (i*31+7) & 0x7F.
    {
        std::string content(1 << 20, '\0');
        for (size_t i = 0; i < content.size(); i++) {
            content[i] = (char) ((i * 31 + 7) & 0x7F);
        }
        int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0644);
        ASSERT_GE(fd, 0);
        ASSERT_EQ(::write(fd, content.data(), content.size()),
                  (ssize_t) content.size());
        ::close(fd);
    }
    auto expectAt = [](int64_t i) { return (int) ((i * 31 + 7) & 0x7F); };

    std::string src =
        "package test;\n"
        "import cajeta.io.file.MappedFile;\n"
        "public final class D {\n"
        "    static int32 probeOnce() {\n"
        "        MappedFile m = heap MappedFile(\"" + path + "\");\n"
        "        if (m.size() != 1048576) { return -11; }\n"
        "        if (m.get(0) != " + std::to_string(expectAt(0)) + ") { return -12; }\n"
        "        if (m.get(12345) != " + std::to_string(expectAt(12345)) + ") { return -13; }\n"
        "        if (m.get(1048575) != " + std::to_string(expectAt(1048575)) + ") { return -14; }\n"
        // bulk read through the mapping
        "        int8[] buf = heap int8[64];\n"
        "        int64 got = m.read(524288, buf, 0, 64);\n"
        "        if (got != 64) { return -15; }\n"
        "        if (buf[0] != " + std::to_string(expectAt(524288)) + ") { return -16; }\n"
        "        if (buf[63] != " + std::to_string(expectAt(524288 + 63)) + ") { return -17; }\n"
        "        return 1;\n"
        "    }\n"                        // m drops here — the mapping must unmap
        "    public static int32 run() {\n"
        "        return probeOnce();\n"
        "    }\n"
        "    public static int32 churn() {\n"
        "        int32 i = 0;\n"
        "        while (i < 200) {\n"
        "            int32 r = probeOnce();\n"
        "            if (r != 1) { return r; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto run = jit->lookup<int32_t (*)()>("run");
    auto churn = jit->lookup<int32_t (*)()>("churn");
    ASSERT_EQ(run(), 1);
#ifdef __linux__
    long before = mapCount();
    ASSERT_EQ(churn(), 1);
    long after = mapCount();
    // 200 leaked mappings would add ~200 lines; allow generous unrelated churn.
    EXPECT_LT(after - before, 50)
        << "MappedFile drop is not releasing its mapping";
#else
    ASSERT_EQ(churn(), 1);
#endif
    ::unlink(path.c_str());
}

// 4.1.5 — mapping a file larger than RAM does not fault the whole file in:
// resident size stays bounded (spec 3.1's reason to exist — checkpoints
// larger than available RAM). An 8 GiB sparse mapping is touched at three
// spots; RSS must not grow by anything like the file size (a MAP_POPULATE- or
// read-all-fallback implementation fails this by gigabytes).
//
// RED until 4.2.4.
TEST(FileIo64Tests, hugeMappingStaysBounded) {
#ifndef __linux__
    GTEST_SKIP() << "RSS probe reads /proc/self/status (Linux only)";
#else
    const std::string path = uniquePath("huge_map.bin");
    const int64_t size = (int64_t) 8 * 1024 * 1024 * 1024;   // 8 GiB, sparse
    static const char MARK[8] = {90, 91, 92, 93, 94, 95, 96, 97};
    makeSparseWithTailMark(path, size, MARK, sizeof(MARK));

    std::string src =
        "package test;\n"
        "import cajeta.io.file.MappedFile;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64 want = 8;\n"
        "        want = want * 1024 * 1024 * 1024;\n"         // 8 GiB
        "        MappedFile m = heap MappedFile(\"" + path + "\");\n"
        "        if (m.size() != want) { return -1; }\n"
        "        if (m.get(0) != 0) { return -2; }\n"          // hole
        "        if (m.get(want / 2) != 0) { return -3; }\n"   // hole
        "        if (m.get(want - 8) != 90) { return -4; }\n"  // tail marker
        "        if (m.get(want - 1) != 97) { return -5; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    long before = rssKb();
    ASSERT_GT(before, 0);
    ASSERT_EQ(fn(), 1);
    long after = rssKb();
    EXPECT_LT(after - before, 512 * 1024)
        << "mapping an 8 GiB file made gigabytes resident";
    ::unlink(path.c_str());
#endif
}

// MappedFile.copyTo — the bulk seam feeding cajeta-llama 5.2.2: one memcpy
// from the mapping into TYPED storage resolved by address
// (Storage.hostAddress(), the Arrow address tier), replacing element-at-a-
// time decode loops. Four known f32 little-endian patterns are written to a
// file, mapped, copied into a Storage<float32>'s buffer in one call, and
// read back as floats — bit-exact.
TEST(FileIo64Tests, mappedFileCopyToTypedStorage) {
    const std::string path = uniquePath("copy_to.bin");
    {
        const float vals[4] = {1.5f, -2.25f, 3.0f, 0.0625f};
        int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0644);
        ASSERT_GE(fd, 0);
        ASSERT_EQ(::write(fd, vals, sizeof(vals)), (ssize_t) sizeof(vals));
        ::close(fd);
    }
    std::string src =
        "package test;\n"
        "import cajeta.io.file.MappedFile;\n"
        "import cajeta.math.Storage;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        MappedFile m = heap MappedFile(\"" + path + "\");\n"
        "        if (m.size() != 16) { return -1; }\n"
        "        Storage<float32> s = heap Storage<float32>(4);\n"
        "        int64 got = m.copyTo(0, s.hostAddress(), 16);\n"
        "        if (got != 16) { return -2; }\n"
        "        if (s.get(0) != 1.5f) { return -3; }\n"
        "        if (s.get(1) != -2.25f) { return -4; }\n"
        "        if (s.get(2) != 3.0f) { return -5; }\n"
        "        if (s.get(3) != 0.0625f) { return -6; }\n"
        // out-of-range source window and dead destination are rejected
        "        if (m.copyTo(8, s.hostAddress(), 16) != -1) { return -7; }\n"
        "        if (m.copyTo(0, 0, 4) != -1) { return -8; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
    ::unlink(path.c_str());
}

// 4.2.5 pin — File.write returns the count actually written, per its
// documented contract (File.cajeta: "Returns the count actually written").
// Today the native helper returns 0 on success (cajeta_rt_lang.c) while the
// wrapper advances `pos` by the requested length — so the documented count and
// the returned value disagree.
//
// RED until 4.2.5.
TEST(FileIo64Tests, writeReturnsCountWritten) {
    const std::string path = uniquePath("write_count.bin");
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        File f = File.open(\"" + path + "\", OpenMode.WRITE);\n"
        "        int8[] d = heap int8[10];\n"
        "        int32 i = 0;\n"
        "        while (i < 10) {\n"
        "            d[i] = (int8) (65 + i);\n"
        "            i = i + 1;\n"
        "        }\n"
        "        int64 n = f.write(d, 0, 10);\n"
        "        f.close();\n"
        "        if (n != 10) { return -1; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
    EXPECT_EQ(readRaw(path), "ABCDEFGHIJ");
    ::unlink(path.c_str());
}
