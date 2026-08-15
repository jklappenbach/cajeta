// Functional tests for cajeta.collection.ltm.BPlusTree — the disk-backed,
// larger-than-memory ordered map. Each test compiles a small cajeta program
// (including a concrete Encoder<int32>) and runs it through the JIT, doing
// real file I/O against a unique temp path.
//
// The pool capacity is deliberately tiny relative to the number of pages, so
// eviction (and thus serialize / deserialize round-trips) actually happens
// while the tree is in use — this is what exercises "larger than memory".

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>

using cajeta_test::CajetaJit;

namespace {

// Portable temp root. Prefer $TEST_TMPDIR (CI override); else the OS temp dir.
// A hard-coded "/tmp" doesn't exist on Windows, which crashed the file-backed
// B+tree tests there. Backslashes normalized to '/' since this path is embedded
// verbatim into cajeta source string literals. See FileIoTests.cpp tmpRoot.
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
    path += "/cajeta_ltm_bplus_";
    path += std::to_string((long long) ::getpid());
    path += "_";
    path += name;
    return path;
}

// A concrete little-endian Encoder<int32>, defined alongside the driver class
// so the JIT can instantiate LtmBPlusTree<int32, int32> end to end.
const char* kI32Enc =
    "public final class I32Enc implements Encoder<int32> {\n"
    "    public I32Enc() { }\n"
    "    public #int8[] encode(int32 value) {\n"
    "        int8[] b = heap int8[4];\n"
    "        b[0] = (int8) (value & 0xFF);\n"
    "        b[1] = (int8) ((value >> 8) & 0xFF);\n"
    "        b[2] = (int8) ((value >> 16) & 0xFF);\n"
    "        b[3] = (int8) ((value >> 24) & 0xFF);\n"
    "        return #b;\n"
    "    }\n"
    "    public #int32 decode(int8[] bytes) {\n"
    "        int32 b0 = ((int32) bytes[0]) & 0xFF;\n"
    "        int32 b1 = ((int32) bytes[1]) & 0xFF;\n"
    "        int32 b2 = ((int32) bytes[2]) & 0xFF;\n"
    "        int32 b3 = ((int32) bytes[3]) & 0xFF;\n"
    "        int32 r = b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);\n"
    "        return #r;\n"
    "    }\n"
    "}\n";

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// Build a tree with many entries through a 6-frame pool (order 4 -> dozens of
// pages), forcing eviction on the write path, then read back through the same
// (still cold-evicting) pool. Proves the tree is correct while only a fraction
// of its pages are ever resident.
TEST(LtmBPlusTreeTests, insertAndLookupWithEviction) {
    std::string path = uniquePath("evict.idx");
    ::unlink(path.c_str());
    std::string src =
        "package test;\n"
        "import cajeta.collection.ltm.LtmBPlusTree;\n"
        "import cajeta.wire.Encoder;\n"
        + std::string(kI32Enc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Encoder<int32> ke = heap I32Enc();\n"
        "        Encoder<int32> ve = heap I32Enc();\n"
        "        LtmBPlusTree<int32, int32> t ="
        " heap LtmBPlusTree<int32, int32>(\"" + path + "\", ke, ve, 4, 6);\n"
        "        int32 i = 0;\n"
        "        while (i < 100) {\n"
        "            t.put(i, i * 2);\n"
        "            i = i + 1;\n"
        "        }\n"
        "        int32 r = t.get(50) + t.min() + (int32) t.count();\n"  // 100 + 0 + 100
        "        t.close();\n"
        "        return r;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 200);
}

// A concrete BufferEncoder<int32> whose bytes match I32Enc exactly — the
// allocation-free companion (cajeta-llama plan 10.2.4).
const char* kI32FastEnc =
    "public final class I32Fast implements BufferEncoder<int32> {\n"
    "    public I32Fast() { }\n"
    "    public int32 encodeInto(int32 value, int8[] dst, int32 off) {\n"
    "        dst[off] = (int8) (value & 0xFF);\n"
    "        dst[off + 1] = (int8) ((value >> 8) & 0xFF);\n"
    "        dst[off + 2] = (int8) ((value >> 16) & 0xFF);\n"
    "        dst[off + 3] = (int8) ((value >> 24) & 0xFF);\n"
    "        return 4;\n"
    "    }\n"
    "}\n";

// Build + flush + close, then reopen from a fresh pager with a cold cache and
// verify every key survived the disk round-trip. The reopen pool (8 frames) is
// far smaller than the page count, so the verification scan pages in/out from
// disk throughout.
TEST(LtmBPlusTreeTests, persistsAcrossReopen) {
    std::string path = uniquePath("reopen.idx");
    ::unlink(path.c_str());
    std::string src =
        "package test;\n"
        "import cajeta.collection.ltm.LtmBPlusTree;\n"
        "import cajeta.wire.Encoder;\n"
        + std::string(kI32Enc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Encoder<int32> ke = heap I32Enc();\n"
        "        Encoder<int32> ve = heap I32Enc();\n"
        "        LtmBPlusTree<int32, int32> t ="
        " heap LtmBPlusTree<int32, int32>(\"" + path + "\", ke, ve, 4, 8);\n"
        "        int32 i = 0;\n"
        "        while (i < 200) {\n"
        "            t.put(i, i * 3);\n"
        "            i = i + 1;\n"
        "        }\n"
        "        t.flush();\n"
        "        t.close();\n"
        "        Encoder<int32> ke2 = heap I32Enc();\n"
        "        Encoder<int32> ve2 = heap I32Enc();\n"
        "        LtmBPlusTree<int32, int32> t2 ="
        " heap LtmBPlusTree<int32, int32>(\"" + path + "\", ke2, ve2, 4, 8);\n"
        "        int32 ok = 0;\n"
        "        int32 j = 0;\n"
        "        while (j < 200) {\n"
        "            if (t2.get(j) == j * 3) { ok = ok + 1; }\n"
        "            j = j + 1;\n"
        "        }\n"
        "        t2.close();\n"
        "        return ok;\n"  // expect all 200 keys recovered from disk
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 200);
}

// ── cajeta-llama Unit 10: delete, ordered scan, alloc-free Encoder ─────────

// 10.1.1: remove makes a key absent to get/containsKey and count drops;
// a re-put of a removed key revives it.
TEST(LtmBPlusTreeTests, removeMakesKeysAbsentAndReputRevives) {
    std::string path = uniquePath("remove.idx");
    ::unlink(path.c_str());
    std::string src =
        "package test;\n"
        "import cajeta.collection.ltm.LtmBPlusTree;\n"
        "import cajeta.wire.Encoder;\n"
        + std::string(kI32Enc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Encoder<int32> ke = heap I32Enc();\n"
        "        Encoder<int32> ve = heap I32Enc();\n"
        "        LtmBPlusTree<int32, int32> t ="
        " heap LtmBPlusTree<int32, int32>(\"" + path + "\", ke, ve, 4, 6);\n"
        "        int32 i = 0;\n"
        "        while (i < 100) { t.put(i, i * 2); i = i + 1; }\n"
        "        i = 0;\n"
        "        while (i < 100) {\n"
        "            if (i % 2 == 0) {\n"
        "                if (t.remove(i) == false) { return 1; }\n"
        "            }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (t.remove(4)) { return 2; }\n"
        "        if ((int32) t.count() != 50) { return 3; }\n"
        "        if (t.containsKey(6)) { return 4; }\n"
        "        if (t.get(6) != 0) { return 5; }\n"
        "        if (t.containsKey(7) == false) { return 6; }\n"
        "        if (t.get(7) != 14) { return 7; }\n"
        "        if (t.min() != 1) { return 8; }\n"
        "        t.put(4, 999);\n"
        "        if (t.get(4) != 999) { return 9; }\n"
        "        if ((int32) t.count() != 51) { return 10; }\n"
        "        t.close();\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}

// 10.1.3: removal is durable — reopen from a cold pager still reports the
// key absent and the count right.
TEST(LtmBPlusTreeTests, removalIsDurableAcrossReopen) {
    std::string path = uniquePath("rmdur.idx");
    ::unlink(path.c_str());
    std::string src =
        "package test;\n"
        "import cajeta.collection.ltm.LtmBPlusTree;\n"
        "import cajeta.wire.Encoder;\n"
        + std::string(kI32Enc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Encoder<int32> ke = heap I32Enc();\n"
        "        Encoder<int32> ve = heap I32Enc();\n"
        "        LtmBPlusTree<int32, int32> t ="
        " heap LtmBPlusTree<int32, int32>(\"" + path + "\", ke, ve, 4, 8);\n"
        "        int32 i = 0;\n"
        "        while (i < 60) { t.put(i, i + 100); i = i + 1; }\n"
        "        i = 0;\n"
        "        while (i < 60) {\n"
        "            if (i % 3 == 0) { t.remove(i); }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        t.close();\n"
        "        Encoder<int32> ke2 = heap I32Enc();\n"
        "        Encoder<int32> ve2 = heap I32Enc();\n"
        "        LtmBPlusTree<int32, int32> t2 ="
        " heap LtmBPlusTree<int32, int32>(\"" + path + "\", ke2, ve2, 4, 8);\n"
        "        if ((int32) t2.count() != 40) { return 1; }\n"
        "        int32 j = 0;\n"
        "        while (j < 60) {\n"
        "            if (j % 3 == 0) {\n"
        "                if (t2.containsKey(j)) { return 2; }\n"
        "            } else {\n"
        "                if (t2.get(j) != j + 100) { return 3; }\n"
        "            }\n"
        "            j = j + 1;\n"
        "        }\n"
        "        t2.close();\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}

// 10.1.2: compaction reclaims tombstone space — the file shrinks, and
// repeated insert/remove/compact cycles do not grow it without bound.
TEST(LtmBPlusTreeTests, compactionReclaimsSpace) {
    std::string path = uniquePath("compact.idx");
    ::unlink(path.c_str());
    std::string src =
        "package test;\n"
        "import cajeta.collection.ltm.LtmBPlusTree;\n"
        "import cajeta.io.file.File;\n"
        "import cajeta.io.file.OpenMode;\n"
        "import cajeta.wire.Encoder;\n"
        + std::string(kI32Enc) +
        "public final class D {\n"
        "    static int64 sizeOf(String p) {\n"
        "        File f = File.open(p, OpenMode.READ);\n"
        "        int64 s = f.size();\n"
        "        f.close();\n"
        "        return s;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Encoder<int32> ke = heap I32Enc();\n"
        "        Encoder<int32> ve = heap I32Enc();\n"
        "        LtmBPlusTree<int32, int32> t ="
        " heap LtmBPlusTree<int32, int32>(\"" + path + "\", ke, ve, 4, 8);\n"
        "        int32 i = 0;\n"
        "        while (i < 400) { t.put(i, i); i = i + 1; }\n"
        "        i = 0;\n"
        "        while (i < 400) {\n"
        "            if (i % 4 != 0) { t.remove(i); }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        t.flush();\n"
        "        int64 before = D.sizeOf(\"" + path + "\");\n"
        "        t.compact();\n"
        "        int64 after = D.sizeOf(\"" + path + "\");\n"
        "        if ((after < before) == false) { return 1; }\n"
        "        if ((int32) t.count() != 100) { return 2; }\n"
        "        i = 0;\n"
        "        while (i < 400) {\n"
        "            if (i % 4 == 0) {\n"
        "                if (t.get(i) != i) { return 3; }\n"
        "            } else {\n"
        "                if (t.containsKey(i)) { return 4; }\n"
        "            }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        int64 bound = after * 3;\n"
        "        int32 c = 0;\n"
        "        while (c < 4) {\n"
        "            i = 400;\n"
        "            while (i < 600) { t.put(i, i); i = i + 1; }\n"
        "            i = 400;\n"
        "            while (i < 600) { t.remove(i); i = i + 1; }\n"
        "            t.compact();\n"
        "            c = c + 1;\n"
        "        }\n"
        "        int64 finalSize = D.sizeOf(\"" + path + "\");\n"
        "        if ((finalSize <= bound) == false) { return 5; }\n"
        "        t.close();\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}

// 10.1.4 + 10.1.5: ordered scan walks ascending over exactly the live
// keys; scanFrom returns entries at or after the key.
TEST(LtmBPlusTreeTests, orderedScanAndScanFrom) {
    std::string path = uniquePath("scan.idx");
    ::unlink(path.c_str());
    std::string src =
        "package test;\n"
        "import cajeta.collection.ltm.LtmBPlusTree;\n"
        "import cajeta.collection.ltm.LtmCursor;\n"
        "import cajeta.wire.Encoder;\n"
        + std::string(kI32Enc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Encoder<int32> ke = heap I32Enc();\n"
        "        Encoder<int32> ve = heap I32Enc();\n"
        "        LtmBPlusTree<int32, int32> t ="
        " heap LtmBPlusTree<int32, int32>(\"" + path + "\", ke, ve, 4, 6);\n"
        "        int32 i = 0;\n"
        "        while (i < 200) {\n"
        "            int32 k = (i * 7) % 200;\n"
        "            t.put(k, k * 10);\n"
        "            i = i + 1;\n"
        "        }\n"
        "        i = 0;\n"
        "        while (i < 200) {\n"
        "            if (i % 3 == 0) { t.remove(i); }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        LtmCursor<int32, int32> c = t.scan();\n"
        "        int32 prev = -1;\n"
        "        int32 seen = 0;\n"
        "        while (c.valid()) {\n"
        "            int32 k = c.key();\n"
        "            if ((prev < k) == false) { return 1; }\n"
        "            if (k % 3 == 0) { return 2; }\n"
        "            if (c.value() != k * 10) { return 3; }\n"
        "            prev = k;\n"
        "            seen = seen + 1;\n"
        "            c.next();\n"
        "        }\n"
        "        if (seen != 133) { return 4; }\n"
        "        LtmCursor<int32, int32> c2 = t.scanFrom(37);\n"
        "        if (c2.valid() == false) { return 5; }\n"
        "        if (c2.key() != 37) { return 6; }\n"
        "        int32 n2 = 0;\n"
        "        while (c2.valid()) {\n"
        "            if (c2.key() < 37) { return 7; }\n"
        "            n2 = n2 + 1;\n"
        "            c2.next();\n"
        "        }\n"
        "        if (n2 != 109) { return 8; }\n"
        "        LtmCursor<int32, int32> c3 = t.scanFrom(39);\n"
        "        if (c3.key() != 40) { return 9; }\n"
        "        t.close();\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}

// 10.1.6 + 10.3.2: a scan (and the whole insert/remove/lookup surface)
// over a tree an order of magnitude larger than the resident pool — the
// pager pages in and out mid-walk and nothing is lost.
TEST(LtmBPlusTreeTests, scanPagesThroughTinyPoolLargerThanMemory) {
    std::string path = uniquePath("bigscan.idx");
    ::unlink(path.c_str());
    std::string src =
        "package test;\n"
        "import cajeta.collection.ltm.LtmBPlusTree;\n"
        "import cajeta.collection.ltm.LtmCursor;\n"
        "import cajeta.wire.Encoder;\n"
        + std::string(kI32Enc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Encoder<int32> ke = heap I32Enc();\n"
        "        Encoder<int32> ve = heap I32Enc();\n"
        "        LtmBPlusTree<int32, int32> t ="
        " heap LtmBPlusTree<int32, int32>(\"" + path + "\", ke, ve, 4, 12);\n"
        "        int32 i = 0;\n"
        "        while (i < 2000) {\n"
        "            int32 k = (i * 13) % 2000;\n"
        "            t.put(k, k + 7);\n"
        "            i = i + 1;\n"
        "        }\n"
        "        i = 0;\n"
        "        while (i < 2000) {\n"
        "            if (i % 5 == 0) { t.remove(i); }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if ((int32) t.count() != 1600) { return 1; }\n"
        "        LtmCursor<int32, int32> c = t.scan();\n"
        "        int32 prev = -1;\n"
        "        int32 seen = 0;\n"
        "        int64 sum = 0;\n"
        "        while (c.valid()) {\n"
        "            int32 k = c.key();\n"
        "            if ((prev < k) == false) { return 2; }\n"
        "            if (c.value() != k + 7) { return 3; }\n"
        "            prev = k;\n"
        "            seen = seen + 1;\n"
        "            sum = sum + (int64) k;\n"
        "            c.next();\n"
        "        }\n"
        "        if (seen != 1600) { return 4; }\n"
        "        if (sum != 1600000) { return 5; }\n"
        "        t.close();\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}

// 10.1.7 + 10.1.8: the BufferEncoder path allocates NOTHING per call
// (asserted by count), and a tree built with buffer encoders round-trips
// and stays readable by the allocating form.
TEST(LtmBPlusTreeTests, bufferEncoderIsAllocationFree) {
    std::string path = uniquePath("fastenc.idx");
    ::unlink(path.c_str());
    std::string src =
        "package test;\n"
        "import cajeta.collection.ltm.LtmBPlusTree;\n"
        "import cajeta.lang.Cajeta;\n"
        "import cajeta.wire.BufferEncoder;\n"
        "import cajeta.wire.Encoder;\n"
        + std::string(kI32Enc)
        + std::string(kI32FastEnc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        BufferEncoder<int32> fast = heap I32Fast();\n"
        "        int8[] buf = heap int8[4096];\n"
        "        fast.encodeInto(42, buf, 0);\n"
        "        int64 b0 = Cajeta.allocatedBytes();\n"
        "        int32 i = 0;\n"
        "        int32 off = 0;\n"
        "        while (i < 1000) {\n"
        "            off = (i % 512) * 4;\n"
        "            int32 n = fast.encodeInto(i, buf, off);\n"
        "            if (n != 4) { return 1; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (Cajeta.allocatedBytes() != b0) { return 2; }\n"
        "        Encoder<int32> ke = heap I32Enc();\n"
        "        Encoder<int32> ve = heap I32Enc();\n"
        "        BufferEncoder<int32> kf = heap I32Fast();\n"
        "        BufferEncoder<int32> vf = heap I32Fast();\n"
        "        LtmBPlusTree<int32, int32> t ="
        " heap LtmBPlusTree<int32, int32>(\"" + path + "\", ke, ve, kf, vf, 4, 6);\n"
        "        i = 0;\n"
        "        while (i < 150) { t.put(i, i * 5); i = i + 1; }\n"
        "        t.flush();\n"
        "        i = 0;\n"
        "        while (i < 150) {\n"
        "            if (t.get(i) != i * 5) { return 3; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        t.close();\n"
        "        Encoder<int32> ke2 = heap I32Enc();\n"
        "        Encoder<int32> ve2 = heap I32Enc();\n"
        "        LtmBPlusTree<int32, int32> t2 ="
        " heap LtmBPlusTree<int32, int32>(\"" + path + "\", ke2, ve2, 4, 6);\n"
        "        i = 0;\n"
        "        while (i < 150) {\n"
        "            if (t2.get(i) != i * 5) { return 4; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        t2.close();\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}

// 10.1.9: `order` is genuinely a parameter — the same data round-trips at
// a small and a large fanout, and a reopened file keeps its stored order.
TEST(LtmBPlusTreeTests, orderIsGenuinelyParametric) {
    std::string pathA = uniquePath("order4.idx");
    std::string pathB = uniquePath("order64.idx");
    ::unlink(pathA.c_str());
    ::unlink(pathB.c_str());
    std::string src =
        "package test;\n"
        "import cajeta.collection.ltm.LtmBPlusTree;\n"
        "import cajeta.wire.Encoder;\n"
        + std::string(kI32Enc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Encoder<int32> ka = heap I32Enc();\n"
        "        Encoder<int32> va = heap I32Enc();\n"
        "        LtmBPlusTree<int32, int32> a ="
        " heap LtmBPlusTree<int32, int32>(\"" + pathA + "\", ka, va, 4, 8);\n"
        "        Encoder<int32> kb = heap I32Enc();\n"
        "        Encoder<int32> vb = heap I32Enc();\n"
        "        LtmBPlusTree<int32, int32> b ="
        " heap LtmBPlusTree<int32, int32>(\"" + pathB + "\", kb, vb, 64, 8);\n"
        "        int32 i = 0;\n"
        "        while (i < 300) {\n"
        "            int32 k = (i * 11) % 300;\n"
        "            a.put(k, k * 2);\n"
        "            b.put(k, k * 2);\n"
        "            i = i + 1;\n"
        "        }\n"
        "        i = 0;\n"
        "        while (i < 300) {\n"
        "            if (a.get(i) != b.get(i)) { return 1; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        a.close();\n"
        "        b.close();\n"
        "        Encoder<int32> kc = heap I32Enc();\n"
        "        Encoder<int32> vc = heap I32Enc();\n"
        "        LtmBPlusTree<int32, int32> a2 ="
        " heap LtmBPlusTree<int32, int32>(\"" + pathA + "\", kc, vc, 128, 8);\n"
        "        a2.put(1000, 2000);\n"
        "        if (a2.get(1000) != 2000) { return 2; }\n"
        "        i = 0;\n"
        "        while (i < 300) {\n"
        "            if (a2.get(i) != i * 2) { return 3; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        a2.close();\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}
