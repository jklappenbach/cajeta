// LinkedList with a CLASS-typed element — popHead/popTail round trips.
//
// linkedlist-class-pop-spec.md: `LinkedList<String>` (any class-typed T) was
// reported to SIGSEGV on pop, while `LinkedList<int32>` popped fine. The
// suspected shape was the remove-shaped return handing the element back while
// the popped node's drop freed it.
//
// No code in the repo instantiated a class-typed LinkedList before the report
// (grep found only a doc comment), so the path was never exercised. These pin
// it.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include <cstdint>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// The reported repro: two class-typed elements in, one popped off the tail.
// Returning its length proves the popped element is ALIVE, not freed.
TEST(LinkedListClassPopTests, popTailReturnsLiveClassElement) {
    EXPECT_EQ(runI32(
        "package test;\n"
        "import cajeta.collection.LinkedList;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        LinkedList<String> xs = heap LinkedList<String>();\n"
        "        xs.addTail(\"alpha\");\n"
        "        xs.addTail(\"beta\");\n"
        "        String last = xs.popTail();\n"
        "        return (int32) last.size();\n"
        "    }\n"
        "}\n"), 4);   // "beta"
}

TEST(LinkedListClassPopTests, popHeadReturnsLiveClassElement) {
    EXPECT_EQ(runI32(
        "package test;\n"
        "import cajeta.collection.LinkedList;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        LinkedList<String> xs = heap LinkedList<String>();\n"
        "        xs.addTail(\"alpha\");\n"
        "        xs.addTail(\"beta\");\n"
        "        String first = xs.popHead();\n"
        "        return (int32) first.size();\n"
        "    }\n"
        "}\n"), 5);   // "alpha"
}

// Pop everything, then keep using what came out — the drop-chain-clean case
// (spec 3.1). If a popped node's drop frees the element, the SECOND pop or the
// trailing reads are where it shows.
TEST(LinkedListClassPopTests, drainKeepsEveryPoppedElementAlive) {
    EXPECT_EQ(runI32(
        "package test;\n"
        "import cajeta.collection.LinkedList;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        LinkedList<String> xs = heap LinkedList<String>();\n"
        "        xs.addTail(\"alpha\");\n"
        "        xs.addTail(\"beta\");\n"
        "        xs.addTail(\"gamma\");\n"
        "        String a = xs.popHead();\n"
        "        String b = xs.popTail();\n"
        "        String c = xs.popHead();\n"
        "        return (int32) (a.size() + b.size() + c.size())\n"
        "             + (int32) xs.count();\n"
        "    }\n"
        "}\n"), 5 + 5 + 4 + 0);   // alpha + gamma + beta, list emptied
}

// The single-element list takes popTail's OTHER branch (no predecessor — the
// title lives in headNode, not in prev.next).
TEST(LinkedListClassPopTests, popTailSingleElementTakesHeadNodeBranch) {
    EXPECT_EQ(runI32(
        "package test;\n"
        "import cajeta.collection.LinkedList;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        LinkedList<String> xs = heap LinkedList<String>();\n"
        "        xs.addTail(\"solo\");\n"
        "        String only = xs.popTail();\n"
        "        return (int32) only.size() + (int32) xs.count();\n"
        "    }\n"
        "}\n"), 4);
}

// The primitive path already worked; pin it so a fix for the class path
// cannot regress it.

// BOUND — a user class surrendered with `heap` pops correctly.

// BOUND — and so does one the caller LENDS.

// PROBE — String via a NAMED LOCAL rather than a literal at the call site.
TEST(LinkedListClassPopTests, popStringViaNamedLocalSurvives) {
    EXPECT_EQ(runI32(
        "package test;\n"
        "import cajeta.collection.LinkedList;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        LinkedList<String> xs = heap LinkedList<String>();\n"
        "        String a = \"alpha\";\n"
        "        xs.addTail(#a);\n"
        "        String t = xs.popTail();\n"
        "        return (int32) t.size();\n"
        "    }\n"
        "}\n"), 5);
}

// BOUND — add + read-without-pop is fine for String. Isolates POP as the fault.

// BOUND — baseline: a literal outside any container.

// BOUND — the decisive one: a user class that OWNS AN ARRAY, so a spurious
// drop is observable exactly the way String's is. It passes, under a poisoned
// allocator too — so the fault is not "T's drop frees something", it is String.

// PROBE — a String built at RUNTIME rather than a literal. Every earlier
// String probe used a literal (directly or via a local holding one), so the
// literal's own storage/title status was never excluded.
TEST(LinkedListClassPopTests, popRuntimeBuiltStringSurvives) {
    EXPECT_EQ(runI32(
        "package test;\n"
        "import cajeta.collection.LinkedList;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        LinkedList<String> xs = heap LinkedList<String>();\n"
        "        String a = \"alp\" + \"habet\";\n"
        "        xs.addTail(#a);\n"
        "        String t = xs.popTail();\n"
        "        return (int32) t.size();\n"
        "    }\n"
        "}\n"), 8);
}

// PROBE — String in a DIFFERENT container, to separate "String" from
// "LinkedList's pop".

// PROBE — a user class with String's EXACT field shape (int32, int32, int8[],
// int32). If this fails, the fault tracks the layout/ABI; if it passes, String
// is special-cased somewhere.

// PROBE — the same node/extract shape as LinkedList.pop, written from scratch
// outside the stdlib. Isolates "String field + fused claim" from anything
// LinkedList-specific.
static const char* MINI = R"SRC(
package test;
public final class MiniNode<T> {
    public T value;
    public MiniNode(T v) { this.value #= v; }
}
public final class MiniBox<T> {
    MiniNode<T> node;
    public MiniBox() { this.node = null; }
    public void put(T v) { this.node #= heap MiniNode<T>(#v); }
    public T take() {
        MiniNode<T> n #= this.node;
        this.node = null;
        T t #= n.value;
        return #t;
    }
}
)SRC";

TEST(LinkedListClassPopTests, miniBoxStringExtractionSurvives) {
    EXPECT_EQ(runI32(std::string(MINI) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        MiniBox<String> b = heap MiniBox<String>();\n"
        "        b.put(\"alpha\");\n"
        "        String t = b.take();\n"
        "        return (int32) t.size();\n"
        "    }\n"
        "}\n"), 5);
}


// The plain-read probe (plainReadExtractsStringSafely) is retired: its
// hypothesis — that a PLAIN read of a String field resolves a copy, so an
// extraction never needs the claim — was rejected by mode-carrying-claim §4
// ("why a plain read is not a substitute": the title must leave the slot
// WITH the value, which is what the claim does). It only ever passed because
// String fields then had no ownership bit, so the node's teardown leaked the
// wrapper and the returned borrow stayed readable by accident; with String
// field bits landed (39283f70/c322461a) the shape is an ordinary lifetime
// error. The correct extraction spelling is pinned just below and by
// miniBoxStringExtractionSurvives above.

// PROBE — Julian's spelling: `#=` store with a PLAIN rhs (single sharp), not
// the stdlib's `#= n.value` double-sharp.
static const char* MINI_SINGLE = R"SRC(
package test;
public final class SNode<T> {
    public T value;
    public SNode(T v) { this.value #= v; }
}
public final class SBox<T> {
    SNode<T> node;
    public SBox() { this.node = null; }
    public void put(T v) { this.node #= heap SNode<T>(#v); }
    public T take() {
        SNode<T> n #= this.node;
        this.node = null;
        T t #= n.value;         // single sharp — the store carries the transfer
        return #t;
    }
}
)SRC";

TEST(LinkedListClassPopTests, singleSharpStoreFromStringFieldWorks) {
    EXPECT_EQ(runI32(std::string(MINI_SINGLE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        SBox<String> b = heap SBox<String>();\n"
        "        b.put(\"alpha\");\n"
        "        String t = b.take();\n"
        "        return (int32) t.size();\n"
        "    }\n"
        "}\n"), 5);
}

