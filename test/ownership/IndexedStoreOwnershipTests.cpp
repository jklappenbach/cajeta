//
// ownership-title-classifier 8.2.3 — the indexed store is the sink model
// (decided 2026-09-08 with the developer: `m[k] = v` must not force the
// transfer). `HashMap.operator[]=` takes PLAIN formals and forwards the
// caller's word into `put`, so the CALLER chooses:
//
//   m[k] = v     lends the key and the value — the caller keeps both titles
//   m[k] = #v    hands the value's title on (the key still lends)
//   m[k] #= v    mode-carrying: a title if `v` owns, a borrow if it does not
//   m[k] = heap  a fresh value is the slot's
//
// Each verdict program returns 0 on pass; `Cajeta.liveCount()` is balanced
// over 64 calls when every value is dropped exactly once.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runVerdict(const std::string& src) {
    try {
        auto jit = CajetaJit::compile(src.c_str(), "test.A");
        if (!jit) return -1;
        auto fn = jit->lookup<int32_t (*)()>("run");
        if (!fn) return -2;
        return fn();
    } catch (cajeta::Exception& e) {
        ADD_FAILURE() << "unexpected rejection: " << e.getErrorId() << ": " << e.getMessage();
        return -3;
    }
}

const char* PRE =
    "package test;\n"
    "import cajeta.lang.Cajeta;\n"
    "import cajeta.lang.String;\n"
    "import cajeta.collection.HashMap;\n"
    "public final class A {\n"
    "    static class Cell {\n"
    "        public int32 n;\n"
    "        public Cell(int32 n) { this.n = n; }\n"
    "    }\n"
    "    static #Cell mk(int32 n) { return heap Cell(n); }\n"
    "    static Cell viaPlain(int32 n) { return A.mk(n); }\n";

std::string loop(const std::string& members, const std::string& probeBody,
                 const std::string& want, int failBase) {
    return std::string(PRE) + members
        + "    static int32 probe(int32 i) {\n" + probeBody + "    }\n"
        + "    public static int32 run() {\n"
        + "        int64 l0 = Cajeta.liveCount();\n"
        + "        int32 i = 1;\n"
        + "        while (i < 65) {\n"
        + "            if (A.probe(i) != " + want + ") { return " + std::to_string(failBase) + "; }\n"
        + "            i = i + 1;\n"
        + "        }\n"
        + "        if (Cajeta.liveCount() != l0) { return " + std::to_string(failBase + 1) + "; }\n"
        + "        return 0;\n"
        + "    }\n"
        + "}\n";
}

} // namespace

TEST(IndexedStoreOwnershipTests, plainIndexedStoreLendsTheValue) {
    std::string src = loop("",
        "        Cell v = heap Cell(i);\n"                // the local owns
        "        HashMap<int32, Cell> m = heap HashMap<int32, Cell>(4);\n"
        "        m[7] = v;\n"                             // a lend: the map records a borrow
        "        int32 got = m[7].n;\n"
        "        v.n = v.n + 1;\n"                        // still ours to use and to free
        "        return got + v.n - i;\n", "i + 1", 10);
    EXPECT_EQ(runVerdict(src), 0) << "10 = wrong value; 11 = the map freed the lent cell (or it leaked)";
}

TEST(IndexedStoreOwnershipTests, sharpValueIndexedStoreTransfers) {
    std::string src = loop("",
        "        Cell v = heap Cell(i);\n"
        "        HashMap<int32, Cell> m = heap HashMap<int32, Cell>(4);\n"
        "        m[7] = #v;\n"                            // the map owns it now
        "        return m[7].n;\n", "i", 20);
    EXPECT_EQ(runVerdict(src), 0) << "20 = wrong value; 21 = the cell leaked or was freed twice";
}

TEST(IndexedStoreOwnershipTests, modeCarryingIndexedStoreRecordsWhatTheValueHolds) {
    std::string src = loop("",
        "        Cell own = heap Cell(i);\n"
        "        Cell keep = heap Cell(i + 100);\n"
        "        Cell lent = keep;\n"                     // a borrow of keep's cell
        "        HashMap<int32, Cell> m = heap HashMap<int32, Cell>(4);\n"
        "        m[1] #= own;\n"                          // own's title moves to the slot
        "        m[2] #= lent;\n"                         // a borrow is recorded as one
        "        return m[1].n + m[2].n - 100;\n", "i + i", 30);
    EXPECT_EQ(runVerdict(src), 0) << "30 = wrong value; 31 = a cell leaked or keep's cell was freed twice";
}

TEST(IndexedStoreOwnershipTests, freshValueIndexedStoreIsTheSlots) {
    std::string src = loop("",
        "        HashMap<int32, Cell> m = heap HashMap<int32, Cell>(4);\n"
        "        m[3] = heap Cell(i);\n"                  // fresh: the slot owns it
        "        return m[3].n;\n", "i", 40);
    EXPECT_EQ(runVerdict(src), 0) << "40 = wrong value; 41 = the fresh cell leaked";
}

TEST(IndexedStoreOwnershipTests, ownedCallResultSharpIndexedStoreIsTheSlots) {
    // A plain `=` here is OWNED_RESULT_NEEDS_TRANSFER (§4.6), as for any plain
    // receipt of a `#R` result; `#=` records the title the call handed out.
    std::string src = loop("",
        "        HashMap<int32, Cell> m = heap HashMap<int32, Cell>(4);\n"
        "        m[4] #= A.mk(i + 1);\n"
        "        return m[4].n - 1;\n", "i", 45);
    EXPECT_EQ(runVerdict(src), 0) << "45 = wrong value; 46 = the call's cell leaked";
}

TEST(IndexedStoreOwnershipTests, borrowedStringKeyIndexedStoreLendsTheKey) {
    // The tour's HashMapDemo shape: keys are borrows of a literal array's
    // elements and the map counts them — nothing to give, nothing forced.
    std::string src = loop("",
        "        String[] words = [\"the\", \"fox\", \"the\"];\n"
        "        HashMap<String, int32> freq = heap HashMap<String, int32>(4);\n"
        "        int32 k = 0;\n"
        "        while (k < 3) {\n"
        "            String w = words[k];\n"
        "            if (freq.containsKey(w)) { freq[w] = freq[w] + 1; } else { freq[w] = 1; }\n"
        "            k = k + 1;\n"
        "        }\n"
        "        return freq[\"the\"] * 10 + freq[\"fox\"];\n", "21", 50);
    EXPECT_EQ(runVerdict(src), 0) << "50 = wrong counts; 51 = a key wrapper leaked or was freed";
}
