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
#include <vector>

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

// 10.1.2: compaction reclaims tombstone space — the file shrinks, and
// repeated insert/remove/compact cycles do not grow it without bound.

// 10.1.4 + 10.1.5: ordered scan walks ascending over exactly the live
// keys; scanFrom returns entries at or after the key.

// 10.1.6 + 10.3.2: a scan (and the whole insert/remove/lookup surface)
// over a tree an order of magnitude larger than the resident pool — the
// pager pages in and out mid-walk and nothing is lost.

// 10.1.7 + 10.1.8: the BufferEncoder path allocates NOTHING per call
// (asserted by count), and a tree built with buffer encoders round-trips
// and stays readable by the allocating form.

// 10.1.9: `order` is genuinely a parameter — the same data round-trips at
// a small and a large fanout, and a reopened file keeps its stored order.

// ── A corrupt index must FAIL, not spin ───────────────────────────────────
//
// Every root-to-leaf descent in LtmBPlusTree was written as
//
//     while (node.leaf == false) { cur = node.childIds[0]; node = fetch(cur); }
//
// with no bound and no cycle check. A zero-filled page deserializes as a
// NON-leaf whose childIds[0] is 0 — and page 0 is the page we are already on,
// so the descent revisits it forever, calling fetch() at full tilt and never
// returning. That is what a truncated or half-written index looks like, and
// cajeta-llm's suite hit it for real: BenchTest opened a BlockStore whose
// `tmp/` directory did not exist, and the run pinned a core in
// LtmBPlusTree.scan -> LtmPager.fetch until it was killed. A hang is the worst
// available failure mode: no message, no stack, no exit code.
//
// The tree cannot be deeper than the number of pages it has allocated, so that
// count is the honest bound. Exceeding it means the index is corrupt, which is
// a RecoverableException — the same call the pager already makes when every
// frame is pinned ("fail loudly — the -1 would otherwise corrupt the frame
// tables").
TEST(LtmBPlusTreeTests, scanOnAnUnopenableIndexFailsInsteadOfSpinning) {
    // The exact trigger from cajeta-llm: an index path whose DIRECTORY does
    // not exist. Nothing creates it, the open fails, and every page then reads
    // back as zeros — which deserializes as a NON-leaf node whose childIds[0]
    // is 0. Page 0 is the page already being read, so
    //
    //     while (node.leaf == false) { cur = node.childIds[0]; node = fetch(cur); }
    //
    // revisits it forever. BenchTest pinned a core there with no message, no
    // stack and no exit code until it was killed by hand.
    std::string dir = std::string(tmpRoot()) + "/cajeta-ltm-absent-"
                    + std::to_string(::getpid());
    std::filesystem::remove_all(dir);              // must NOT exist
    ASSERT_FALSE(std::filesystem::exists(dir));
    std::string path = dir + "/index.idx";

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
        "        try {\n"
        "            LtmBPlusTree<int32, int32> t ="
        " heap LtmBPlusTree<int32, int32>(\"" + path + "\", ke, ve, 4, 8);\n"
        "            LtmCursor<int32, int32> c #= t.scan();\n"
        "            return 1;\n"
        "        } catch (Exception e) {\n"
        "            return 42;\n"
        "        }\n"
        "    }\n"
        "}\n";
    // Before the descent was bounded this call never returned at all. Either
    // outcome below is acceptable as a CONTRACT -- the tree may refuse the
    // unopenable file outright, or refuse the cyclic descent -- but it must
    // return rather than spin.
    int32_t rc = runI32(src);
    EXPECT_TRUE(rc == 42 || rc == 1) << "unexpected rc " << rc;
    EXPECT_EQ(rc, 42) << "an unopenable index should fail loudly";
}
