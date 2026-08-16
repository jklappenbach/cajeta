//
// Cyclic drop-function synthesis through interface vtables.
//
// THE CRASH: `patchVirtualTableDropFn` called `getOrCreateDropFunction()`
// BEFORE its cheap early-outs. That call does not merely look a function up —
// on a first call it EMITS the whole drop body, which walks the class's fields
// and recurses into their classes. When a class was still being prototyped,
// the recursion re-entered a body whose `rawLlvmType()` was not built yet and
// dereferenced it, SIGSEGVing the COMPILER (fault 0x8):
//
//   generatePrototype -> synthesizeInterfaceVTables -> getOrCreateDropFunction
//     -> emitDropBodyInline -> patchVirtualTableDropFn -> getOrCreateDropFunction
//       -> emitDropBodyInline -> ...
//
// The shape that triggers it is ordinary: a class implementing an interface
// whose method returns another interface, implemented by a type that holds a
// back-reference to the first. Plain borrow fields — no ownership markers
// involved. Found building cajeta-cloud's in-memory object store, where the
// store hands out writers that point back at it; the workaround was to
// extract the shared state into a class referencing no interface-implementing
// type.
//
// Moving the readiness checks first breaks the cycle at its source: a class
// with no vtable or no LLVM struct yet has nothing to patch, and the patch
// re-runs once it is materialized (the flag is only set on a completed patch).
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>

using cajeta_test::CajetaJit;

// The store/writer cycle: Store implements IStore (returning #IWriter), and
// Writer implements IWriter while holding a Store. Compiling this at all is
// the assertion — it used to kill the compiler.
TEST(DropSynthesisCycleTests, interfaceVtableBackReferenceCycleCompiles) {
    auto jit = CajetaJit::compile(
        "package test;\n"
        "public interface IWriter { int64 write(); }\n"
        "public interface IStore { #IWriter openWrite(); }\n"
        "public final class Store implements IStore {\n"
        "    public int64 count;\n"
        "    public Store() { this.count = 0; }\n"
        "    public #IWriter openWrite() {\n"
        "        Writer w = heap Writer(this);\n"
        "        return #w;\n"
        "    }\n"
        "}\n"
        "final class Writer implements IWriter {\n"
        "    Store store;\n"
        "    Writer(Store s) { this.store = s; return; }\n"
        "    public int64 write() {\n"
        "        this.store.count = this.store.count + 1;\n"
        "        return this.store.count;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        Store s = heap Store();\n"
        "        IWriter w #= s.openWrite();\n"
        "        int64 a = w.write();\n"
        "        int64 b = w.write();\n"
        "        return a + b + s.count;\n"      // 1 + 2 + 2
        "    }\n"
        "}\n", "test.D");
    auto fn = jit->lookup<int64_t (*)()>("run");
    EXPECT_EQ(fn(), 5);
}

// A MUTUAL cycle across two interfaces — each side hands out the other, and
// both hold back-references. Exercises the recursion from either entry point.
TEST(DropSynthesisCycleTests, mutualInterfaceCycleCompiles) {
    auto jit = CajetaJit::compile(
        "package test;\n"
        "public interface IA { int64 ping(); }\n"
        "public interface IB { int64 pong(); }\n"
        "public final class A implements IA {\n"
        "    B peer;\n"
        "    public int64 v;\n"
        "    public A() { this.v = 1; }\n"
        "    public void bind(B b) { this.peer = b; return; }\n"
        "    public int64 ping() { return this.v; }\n"
        "}\n"
        "public final class B implements IB {\n"
        "    A peer;\n"
        "    public B(A a) { this.peer = a; return; }\n"
        "    public int64 pong() { return this.peer.ping() + 10; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        A a = heap A();\n"
        "        B b = heap B(a);\n"
        "        a.bind(b);\n"
        "        IB ib = b;\n"
        "        return ib.pong();\n"
        "    }\n"
        "}\n", "test.D");
    auto fn = jit->lookup<int64_t (*)()>("run");
    EXPECT_EQ(fn(), 11);
}

// A self-referential class (the degenerate cycle) still compiles and drops.
TEST(DropSynthesisCycleTests, selfReferentialClassThroughInterfaceCompiles) {
    auto jit = CajetaJit::compile(
        "package test;\n"
        "public interface INode { int64 depth(); }\n"
        "public final class Node implements INode {\n"
        "    Node next;\n"
        "    public int64 n;\n"
        "    public Node(int64 n) { this.n = n; return; }\n"
        "    public void link(Node t) { this.next = t; return; }\n"
        "    public int64 depth() {\n"
        "        if (this.next == null) { return 1; }\n"
        "        return 1 + this.next.depth();\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        Node a = heap Node(1);\n"
        "        Node b = heap Node(2);\n"
        "        a.link(b);\n"
        "        INode i = a;\n"
        "        return i.depth();\n"
        "    }\n"
        "}\n", "test.D");
    auto fn = jit->lookup<int64_t (*)()>("run");
    EXPECT_EQ(fn(), 2);
}
