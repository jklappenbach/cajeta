// Tests for cajeta.collection.graph.Digraph — the interned-CSR directed
// graph core. The class is stdlib (runtime/src/cajeta/collection/graph/
// Digraph.cajeta), so it's loaded by the time these JIT.
//
// Shape mirrors HashMapTests: each test compiles a snippet instantiating
// Digraph<String> (String satisfies the N contract: content hash() +
// equals()) and returns a value the assertion pins.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include <cstdint>

using cajeta_test::CajetaJit;

TEST(DigraphTests, constructOnly) {
    auto src =
        "package test;\n"
        "import cajeta.collection.graph.Digraph;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Digraph<String> g = heap Digraph<String>();\n"
        "        return g.nodeCount();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 0);
}

// intern() is add-or-get: an equal payload returns the existing id.
TEST(DigraphTests, internDedupes) {
    auto src =
        "package test;\n"
        "import cajeta.collection.graph.Digraph;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Digraph<String> g = heap Digraph<String>();\n"
        "        int32 a = g.intern(\"alpha\");\n"
        "        int32 b = g.intern(\"beta\");\n"
        "        int32 a2 = g.intern(\"alpha\");\n"
        "        if (a2 != a) { return 100; }\n"
        "        if (b == a) { return 101; }\n"
        "        return g.nodeCount();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 2);
}

// a -> b -> c, d -> b: degrees and CSR adjacency, forward and reverse.
TEST(DigraphTests, degreesAndAdjacency) {
    auto src =
        "package test;\n"
        "import cajeta.collection.graph.Digraph;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Digraph<String> g = heap Digraph<String>();\n"
        "        int32 a = g.intern(\"a\");\n"
        "        int32 b = g.intern(\"b\");\n"
        "        int32 c = g.intern(\"c\");\n"
        "        int32 d = g.intern(\"d\");\n"
        "        g.addEdge(a, b);\n"
        "        g.addEdge(b, c);\n"
        "        g.addEdge(d, b);\n"
        "        if (g.outDegree(a) != 1) { return 1; }\n"
        "        if (g.inDegree(b) != 2) { return 2; }\n"
        "        if (g.outAt(b, 0) != c) { return 3; }\n"
        "        if (g.outAt(b, 5) != -1) { return 4; }\n"
        "        if (g.inDegree(a) != 0) { return 5; }\n"
        "        return 42;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 42);
}

// Multi-root reachability is the core primitive (coco's dead-code analysis
// seeds it with entry + every executed method).
TEST(DigraphTests, multiRootReachability) {
    auto src =
        "package test;\n"
        "import cajeta.collection.graph.Digraph;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Digraph<String> g = heap Digraph<String>();\n"
        "        int32 a = g.intern(\"a\");\n"
        "        int32 b = g.intern(\"b\");\n"
        "        int32 c = g.intern(\"c\");\n"
        "        int32 d = g.intern(\"d\");\n"
        "        int32 e = g.intern(\"e\");\n"
        "        g.addEdge(a, b);\n"
        "        g.addEdge(b, c);\n"
        "        g.addEdge(d, e);\n"
        "        int32[] roots #= heap int32[2];\n"
        "        roots[0] = a;\n"
        "        roots[1] = d;\n"
        "        int8[] seen #= g.reachableFrom(roots);\n"
        "        int32 reach = 0;\n"
        "        int32 i = 0;\n"
        "        while (i < 5) {\n"
        "            if (seen[i] == 1) { reach = reach + 1; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return reach;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 5);
}

// Diamond a->{b,c}->d: BFS visits each node once; DFS preorder follows the
// first-successor chain to the bottom before backtracking.
TEST(DigraphTests, bfsAndDfsOrders) {
    auto src =
        "package test;\n"
        "import cajeta.collection.graph.Digraph;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Digraph<String> g = heap Digraph<String>();\n"
        "        int32 a = g.intern(\"a\");\n"
        "        int32 b = g.intern(\"b\");\n"
        "        int32 c = g.intern(\"c\");\n"
        "        int32 d = g.intern(\"d\");\n"
        "        g.addEdge(a, b);\n"
        "        g.addEdge(a, c);\n"
        "        g.addEdge(b, d);\n"
        "        g.addEdge(c, d);\n"
        "        int32[] bfs #= g.bfsOrder(a);\n"
        "        if ((int32) bfs.count() != 4) { return 1; }\n"
        "        if (bfs[0] != a) { return 2; }\n"
        "        if (bfs[3] != d) { return 3; }\n"
        "        int32[] dfs #= g.dfsOrder(a);\n"
        "        if ((int32) dfs.count() != 4) { return 4; }\n"
        "        if (dfs[0] != a) { return 5; }\n"
        "        if (dfs[1] != b) { return 6; }\n"
        "        if (dfs[2] != d) { return 7; }\n"
        "        if (dfs[3] != c) { return 8; }\n"
        "        return 42;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 42);
}

// Kahn topo covers an acyclic graph fully; closing a cycle shrinks it,
// which is exactly what isCyclic() reads.
TEST(DigraphTests, topoAndCycleDetection) {
    auto src =
        "package test;\n"
        "import cajeta.collection.graph.Digraph;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Digraph<String> g = heap Digraph<String>();\n"
        "        int32 a = g.intern(\"a\");\n"
        "        int32 b = g.intern(\"b\");\n"
        "        int32 c = g.intern(\"c\");\n"
        "        g.addEdge(a, b);\n"
        "        g.addEdge(b, c);\n"
        "        int32[] topo #= g.topoOrder();\n"
        "        if ((int32) topo.count() != 3) { return 1; }\n"
        "        if (topo[0] != a) { return 2; }\n"
        "        if (g.isCyclic()) { return 3; }\n"
        "        g.addEdge(c, a);\n"
        "        if (g.isCyclic() == false) { return 4; }\n"
        "        return 42;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 42);
}

// Edges referencing unknown ids are dropped, not trapped — graphs built
// from external data (parsed call graphs) meet dangling ids routinely.
TEST(DigraphTests, danglingEdgesIgnored) {
    auto src =
        "package test;\n"
        "import cajeta.collection.graph.Digraph;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Digraph<String> g = heap Digraph<String>();\n"
        "        int32 a = g.intern(\"a\");\n"
        "        g.addEdge(a, 99);\n"
        "        g.addEdge(-1, a);\n"
        "        if (g.edgeCount() != 0) { return 1; }\n"
        "        if (g.outDegree(a) != 0) { return 2; }\n"
        "        return 42;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 42);
}

// Kosaraju SCC: cycle {a,b,c}, tail d->a, isolate e. Three components;
// labels ascend in condensation topo order, so the source component (d)
// labels before the cycle it feeds.
TEST(DigraphTests, sccCycleTailIsolate) {
    auto src =
        "package test;\n"
        "import cajeta.collection.graph.Digraph;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Digraph<String> g = heap Digraph<String>();\n"
        "        int32 a = g.intern(\"a\");\n"
        "        int32 b = g.intern(\"b\");\n"
        "        int32 c = g.intern(\"c\");\n"
        "        int32 d = g.intern(\"d\");\n"
        "        int32 e = g.intern(\"e\");\n"
        "        g.addEdge(a, b);\n"
        "        g.addEdge(b, c);\n"
        "        g.addEdge(c, a);\n"
        "        g.addEdge(d, a);\n"
        "        if (g.sccCount() != 3) { return 1; }\n"
        "        int32[] l #= g.sccLabels();\n"
        "        if (l[a] != l[b]) { return 2; }\n"
        "        if (l[b] != l[c]) { return 3; }\n"
        "        if (l[d] == l[a]) { return 4; }\n"
        "        if (l[e] == l[a]) { return 5; }\n"
        "        if (l[e] == l[d]) { return 6; }\n"
        "        if (l[d] > l[a]) { return 7; }\n"      // d feeds the cycle: source labels first
        "        return 42;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 42);
}

// Two 2-cycles bridged b->c: cross-component edges go smaller->larger label.
TEST(DigraphTests, sccBridgedCyclesOrdering) {
    auto src =
        "package test;\n"
        "import cajeta.collection.graph.Digraph;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Digraph<String> g = heap Digraph<String>();\n"
        "        int32 a = g.intern(\"a\");\n"
        "        int32 b = g.intern(\"b\");\n"
        "        int32 c = g.intern(\"c\");\n"
        "        int32 d = g.intern(\"d\");\n"
        "        g.addEdge(a, b);\n"
        "        g.addEdge(b, a);\n"
        "        g.addEdge(c, d);\n"
        "        g.addEdge(d, c);\n"
        "        g.addEdge(b, c);\n"
        "        if (g.sccCount() != 2) { return 1; }\n"
        "        int32[] l #= g.sccLabels();\n"
        "        if (l[a] != l[b]) { return 2; }\n"
        "        if (l[c] != l[d]) { return 3; }\n"
        "        if (l[a] >= l[c]) { return 4; }\n"
        "        return 42;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 42);
}

// Acyclic chain: every node is its own singleton component, labels in
// topo order along the chain.
TEST(DigraphTests, sccAcyclicSingletons) {
    auto src =
        "package test;\n"
        "import cajeta.collection.graph.Digraph;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Digraph<String> g = heap Digraph<String>();\n"
        "        int32 a = g.intern(\"a\");\n"
        "        int32 b = g.intern(\"b\");\n"
        "        int32 c = g.intern(\"c\");\n"
        "        g.addEdge(a, b);\n"
        "        g.addEdge(b, c);\n"
        "        if (g.sccCount() != 3) { return 1; }\n"
        "        int32[] l #= g.sccLabels();\n"
        "        if (l[a] >= l[b]) { return 2; }\n"
        "        if (l[b] >= l[c]) { return 3; }\n"
        "        return 42;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 42);
}
