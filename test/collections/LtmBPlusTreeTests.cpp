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

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

using cajeta_test::CajetaJit;

namespace {

const char* tmpRoot() {
    const char* r = std::getenv("TEST_TMPDIR");
    return r && *r ? r : "/tmp";
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
