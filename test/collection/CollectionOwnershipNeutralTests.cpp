// Every collection is ownership neutral (field-store-ownership 6.2.7): it holds a mix of lent and
// transferred elements through its own moves (grow, shift, rehash, sift, rotate, split, evict,
// remove), frees exactly the elements it owns, and never frees a lent one.

#include <gtest/gtest.h>

#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

std::string makeSource(const std::string& workBody) {
    return "package test;\n"
           "import cajeta.collection.ArrayList;\n"
           "import cajeta.collection.BPlusTree;\n"
           "import cajeta.collection.Cache;\n"
           "import cajeta.collection.HashMap;\n"
           "import cajeta.collection.HashSet;\n"
           "import cajeta.collection.Heap;\n"
           "import cajeta.collection.LinkedList;\n"
           "import cajeta.collection.RedBlackTree;\n"
           "import cajeta.collection.graph.Digraph;\n"
           "import cajeta.concurrent.Channel;\n"
           "public class Cell {\n"
           "    public int64 n;\n"
           "    public Cell(int64 v) { this.n = v; }\n"
           "    public boolean equals(Cell o) { return o != null && o.n == this.n; }\n"
           "    public int64 hash() { return this.n; }\n"
           "    public static boolean operator< (Cell x, Cell y) { return x.n < y.n; }\n"
           "    public static boolean operator> (Cell x, Cell y) { return x.n > y.n; }\n"
           "}\n"
           "public final class D {\n"
           "    public static #String key(int32 v) {\n"
           "        int8[] buf = Cajeta.allocBytes(12L);\n"
           "        int32 i = 11;\n"
           "        int32 x = v;\n"
           "        while (i >= 0) { buf[i] = (int8) (48 + (x - (x / 10) * 10)); x = x / 10; i = i - 1; }\n"
           "        return heap String(#buf, 12);\n"
           "    }\n"
           "    public static int32 scramble(int32 i, int32 n) { return (i * 37 + 11) - ((i * 37 + 11) / n) * n; }\n"
           "    public static boolean lent(int32 i) { return (i - (i / 3) * 3) == 0; }\n"
           "    public static void churn() {\n"
           "        int64 i = 0;\n"
           "        while (i < 600L) { String j #= D.key(7); Cell c = heap Cell(107800L); i = i + 1; }\n"
           "    }\n"
           "    public static #Cell[] lenders(int32 n) {\n"
           "        Cell[] o = heap Cell[n];\n"
           "        int32 i = 0;\n"
           "        while (i < n) { o[i] #= heap Cell((int64) i); i = i + 1; }\n"
           "        return #o;\n"
           "    }\n"
           "    public static int32 checkLenders(Cell[] o, int32 n) {\n"
           "        int32 i = 0;\n"
           "        while (i < n) { if (o[i].n != (int64) i) { return -900 - i; } i = i + 1; }\n"
           "        return 1;\n"
           "    }\n"
           "    public static int32 work() {\n"
           "        " + workBody + "\n"
           "    }\n"
           "    public static int32 run() {\n"
           "        int64 base = Cajeta.liveCount();\n"
           "        int32 t = D.work();\n"
           "        int64 leaked = Cajeta.liveCount() - base;\n"
           "        return (int32) (leaked * 100) + t;\n"
           "    }\n"
           "}\n";
}

int32_t runJit(const std::string& workBody) {
    auto jit = CajetaJit::compile(makeSource(workBody), "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

// Wraps a body that fills and exercises a collection against lenders o[0..n), then checks the
// lenders survive the collection.
std::string withLenders(int n, const std::string& body) {
    return "int32 n = " + std::to_string(n) + ";\n"
           "Cell[] o #= D.lenders(n);\n"
           "{\n" + body + "}\n"
           "D.churn();\n"
           "return D.checkLenders(o, n);";
}

}  // namespace

TEST(CollectionOwnershipNeutralTests, arrayListGrowInsertRemove) {
    EXPECT_EQ(runJit(withLenders(60,
        "ArrayList<Cell> xs = heap ArrayList<Cell>();\n"
        "int32 i = 0;\n"
        "while (i < n) {\n"
        "    if (D.lent(i)) { xs.insert(0, o[i]); } else { xs.insert(0, #heap Cell((int64) i)); }\n"
        "    i = i + 1;\n"
        "}\n"
        "Cell gone #= xs.removeAt(10);\n"
        "xs.set(3, o[0]);\n"
        "xs.set(4, #heap Cell(4000L));\n"
        "D.churn();\n"
        "if (xs.get(4).n != 4000L || xs.get(3).n != 0L) { return -1; }\n"
        "if (xs.get(0).n != (int64) (n - 1) || xs.get(xs.count() - 1).n != 0L) { return -2; }\n"
        "ArrayList<Cell> ys = heap ArrayList<Cell>();\n"
        "ys.appendAll(xs);\n"
        "D.churn();\n"
        "if (ys.get(5).n != xs.get(5).n) { return -3; }\n")), 1);
}

TEST(CollectionOwnershipNeutralTests, hashMapRehashAndRemove) {
    EXPECT_EQ(runJit(withLenders(300,
        "HashMap<String, Cell> m = heap HashMap<String, Cell>();\n"
        "int32 i = 0;\n"
        "while (i < n) {\n"
        "    if (D.lent(i)) { m.put(D.key(i), o[i]); } else { m.put(D.key(i), #heap Cell((int64) i)); }\n"
        "    i = i + 1;\n"
        "}\n"
        "i = 0;\n"
        "while (i < n) { if ((i - (i / 4) * 4) == 1) { Cell r = m.remove(D.key(i)); } i = i + 1; }\n"
        "i = 0;\n"
        "while (i < n) { if ((i - (i / 4) * 4) == 1) { m.put(D.key(i), #heap Cell((int64) i)); } i = i + 1; }\n"
        "D.churn();\n"
        "i = 0;\n"
        "while (i < n) { if (m.get(D.key(i)).n != (int64) i) { return -1 - i; } i = i + 1; }\n")), 1);
}

TEST(CollectionOwnershipNeutralTests, hashSetOfStrings) {
    EXPECT_EQ(runJit(
        "int32 n = 200;\n"
        "String[] o = heap String[n];\n"
        "int32 i = 0;\n"
        "while (i < n) { o[i] #= D.key(i); i = i + 1; }\n"
        "{\n"
        "    HashSet<String> s = heap HashSet<String>();\n"
        "    i = 0;\n"
        "    while (i < n) { if (D.lent(i)) { s.add(o[i]); } else { s.add(#D.key(i)); } i = i + 1; }\n"
        "    i = 0;\n"
        "    while (i < n) { if ((i - (i / 5) * 5) == 2) { s.remove(D.key(i)); } i = i + 1; }\n"
        "    D.churn();\n"
        "    i = 0;\n"
        "    while (i < n) {\n"
        "        boolean want = (i - (i / 5) * 5) != 2;\n"
        "        if (s.contains(D.key(i)) != want) { return -1 - i; }\n"
        "        i = i + 1;\n"
        "    }\n"
        "}\n"
        "D.churn();\n"
        "i = 0;\n"
        "while (i < n) { if (!o[i].equals(D.key(i))) { return -500 - i; } i = i + 1; }\n"
        "return 1;"), 1);
}

TEST(CollectionOwnershipNeutralTests, heapSifts) {
    EXPECT_EQ(runJit(withLenders(80,
        "Heap<Cell> h = heap Heap<Cell>();\n"
        "int32 i = 0;\n"
        "while (i < n) {\n"
        "    int32 v = D.scramble(i, n);\n"
        "    if (D.lent(v)) { h.push(o[v]); } else { h.push(#heap Cell((int64) v)); }\n"
        "    i = i + 1;\n"
        "}\n"
        "D.churn();\n"
        "i = 0;\n"
        "while (i < n / 2) { Cell c = h.pop(); if (c.n != (int64) i) { return -1 - i; } i = i + 1; }\n"
        "D.churn();\n"
        "if (h.peek().n != (int64) (n / 2)) { return -200; }\n")), 1);
}

TEST(CollectionOwnershipNeutralTests, linkedListEnds) {
    EXPECT_EQ(runJit(withLenders(40,
        "LinkedList<Cell> l = heap LinkedList<Cell>();\n"
        "int32 i = 0;\n"
        "while (i < n) {\n"
        "    if (D.lent(i)) { l.add(o[i]); } else { l.add(#heap Cell((int64) i)); }\n"
        "    i = i + 1;\n"
        "}\n"
        "Cell h0 = l.popHead();\n"
        "Cell h1 = l.popHead();\n"
        "Cell t0 = l.popTail();\n"
        "Cell t1 = l.popTail();\n"
        "l.addFirst(o[0]);\n"
        "boolean removed = l.remove(o[3]);\n"
        "D.churn();\n"
        "if (h0.n != 0L || h1.n != 1L || t0.n != (int64) (n - 1) || t1.n != (int64) (n - 2) || !removed) { return -1; }\n"
        "if (l.head().n != 0L || l.get(1L).n != 2L) { return -2; }\n")), 1);
}

TEST(CollectionOwnershipNeutralTests, redBlackTreeRotations) {
    EXPECT_EQ(runJit(withLenders(200,
        "RedBlackTree<int64, Cell> t = heap RedBlackTree<int64, Cell>();\n"
        "int32 i = 0;\n"
        "while (i < n) {\n"
        "    int32 v = D.scramble(i, n);\n"
        "    if (D.lent(v)) { t.put((int64) v, o[v]); } else { t.put((int64) v, #heap Cell((int64) v)); }\n"
        "    i = i + 1;\n"
        "}\n"
        "t.put(5L, #heap Cell(5L));\n"
        "t.put(6L, o[6]);\n"
        "D.churn();\n"
        "i = 0;\n"
        "while (i < n) { if (t.get((int64) i).n != (int64) i) { return -1 - i; } i = i + 1; }\n")), 1);
}

TEST(CollectionOwnershipNeutralTests, bPlusTreeSplits) {
    EXPECT_EQ(runJit(withLenders(400,
        "BPlusTree<int64, Cell> t = heap BPlusTree<int64, Cell>();\n"
        "int32 i = 0;\n"
        "while (i < n) {\n"
        "    int32 v = D.scramble(i, n);\n"
        "    if (D.lent(v)) { t.put((int64) v, o[v]); } else { t.put((int64) v, #heap Cell((int64) v)); }\n"
        "    i = i + 1;\n"
        "}\n"
        "D.churn();\n"
        "i = 0;\n"
        "while (i < n) { if (t.get((int64) i).n != (int64) i) { return -1 - i; } i = i + 1; }\n")), 1);
}

TEST(CollectionOwnershipNeutralTests, cacheEviction) {
    EXPECT_EQ(runJit(withLenders(100,
        "Cache<String, Cell> c = heap Cache<String, Cell>(32);\n"
        "int32 i = 0;\n"
        "while (i < n) {\n"
        "    if (D.lent(i)) { c.put(D.key(i), o[i]); } else { c.put(D.key(i), #heap Cell((int64) i)); }\n"
        "    i = i + 1;\n"
        "}\n"
        "c.remove(D.key(n - 1));\n"
        "D.churn();\n"
        "i = n - 31;\n"
        "while (i < n - 1) { if (c.get(D.key(i)).get().n != (int64) i) { return -1 - i; } i = i + 1; }\n"
        "if (c.containsKey(D.key(0))) { return -300; }\n")), 1);
}

// The ring wraps, so every slot is written again after its first item left.
TEST(CollectionOwnershipNeutralTests, channelRingWraps) {
    EXPECT_EQ(runJit(withLenders(40,
        "Channel<Cell> ch = heap Channel<Cell>(8);\n"
        "int32 i = 0;\n"
        "while (i < n) {\n"
        "    int32 k = 0;\n"
        "    while (k < 5) {\n"
        "        int32 v = i + k;\n"
        "        if (D.lent(v)) { ch.send(o[v]); } else { ch.send(#heap Cell((int64) v)); }\n"
        "        k = k + 1;\n"
        "    }\n"
        "    k = 0;\n"
        "    while (k < 5) {\n"
        "        Optional<Cell> got = ch.receive();\n"
        "        D.churn();\n"
        "        if (got.get().n != (int64) (i + k)) { return -1 - i - k; }\n"
        "        k = k + 1;\n"
        "    }\n"
        "    i = i + 5;\n"
        "}\n"
        "ch.send(#heap Cell(77L));\n"
        "ch.send(o[1]);\n")), 1);
}

// A collection takes a plain element: `intern(x)` lends and `intern(#x)` transfers.
TEST(CollectionOwnershipNeutralTests, digraphInternLendsOrTransfers) {
    EXPECT_EQ(runJit(withLenders(30,
        "Digraph<Cell> g = heap Digraph<Cell>();\n"
        "int32 i = 0;\n"
        "while (i < n) {\n"
        "    if (D.lent(i)) { g.intern(o[i]); } else { g.intern(#heap Cell((int64) i)); }\n"
        "    i = i + 1;\n"
        "}\n"
        "g.intern(#heap Cell(4L));\n"
        "g.intern(o[3]);\n"
        "D.churn();\n"
        "if (g.nodeCount() != n) { return -1; }\n"
        "i = 0;\n"
        "while (i < n) { if (g.nodeAt(i).n != (int64) i) { return -10 - i; } i = i + 1; }\n")), 1);
}
