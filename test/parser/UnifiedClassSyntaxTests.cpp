//
// Phase 1a — heap/stack expression prefixes for class allocation.
//
// First sub-phase of the unified-class rollout (UnifiedClasses.md):
//   - `heap MyClass(args)` parses and behaves as today's `new MyClass(args)`
//     (malloc + ctor call, returns a heap reference).
//   - `stack MyClass { ... }` parses and behaves as today's bare
//     aggregate-init `MyClass { ... }` (stack-allocated body, per-field
//     stores). v1's struct-only stack-aggregate behavior is now reachable
//     via the explicit keyword.
//   - `heap MyClass { ... }` and `stack MyClass(args)` parse but currently
//     reject at codegen with a "Phase 2" message — the new codegen paths
//     (heap aggregate-init; stack alloca + ctor call) land alongside the
//     CajetaStruct collapse.
//
// `new MyClass(args)` continues to work unchanged during the deprecation
// cycle; bare `MyClass(args)` rejection is deferred until the deprecation
// alias for `new` retires.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// ---------------------------------------------------------------------
// `heap MyClass(args)` — synonym for today's `new MyClass(args)`.
// ---------------------------------------------------------------------

TEST(UnifiedClassSyntaxTests, heapConstructorCallAllocates) {
    auto src =
        "package test;\n"
        "public class Box {\n"
        "    int32 width;\n"
        "    int32 height;\n"
        "    public Box(int32 w, int32 h) {\n"
        "        this.width = w;\n"
        "        this.height = h;\n"
        "    }\n"
        "    public int32 area() { return this.width * this.height; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Box b = heap Box(4, 5);\n"
        "        return b.area();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 20);
}

TEST(UnifiedClassSyntaxTests, heapAndNewProduceEquivalentBehavior) {
    // `heap` is a synonym for `new` in Phase 1a — same allocation, same
    // ctor invocation. Both pass through NewExpression at codegen.
    auto src =
        "package test;\n"
        "public class Counter {\n"
        "    int32 n;\n"
        "    public Counter() { this.n = 0; }\n"
        "    public void increment() { this.n = this.n + 1; }\n"
        "    public int32 value() { return this.n; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Counter a = new  Counter();\n"
        "        Counter b = heap Counter();\n"
        "        a.increment(); a.increment(); a.increment();\n"
        "        b.increment(); b.increment();\n"
        "        return a.value() + b.value();\n"  // 3 + 2 = 5
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
}

// ---------------------------------------------------------------------
// `stack MyClass { ... }` — synonym for today's bare aggregate-init.
// ---------------------------------------------------------------------

TEST(UnifiedClassSyntaxTests, stackAggregateInitDeclares) {
    // Aggregate-init via the explicit `stack` keyword. Behaves identically
    // to the bare form (which v1 supports for structs).
    auto src =
        "package test;\n"
        "public struct Point {\n"
        "    int32 x;\n"
        "    int32 y;\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Point p = stack Point { x: 3, y: 4 };\n"
        "        return p.x * p.x + p.y * p.y;\n"  // 9 + 16 = 25
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 25);
}

TEST(UnifiedClassSyntaxTests, stackAggregateAndBareAggregateProduceSameValue) {
    // `stack MyClass { ... }` is the explicit form of `MyClass { ... }`.
    // Both are stack-allocated; both initialize fields the same way.
    auto src =
        "package test;\n"
        "public struct Pair {\n"
        "    int32 first;\n"
        "    int32 second;\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Pair a =        Pair { first: 10, second: 20 };\n"  // bare form
        "        Pair b = stack  Pair { first: 100, second: 200 };\n" // explicit form
        "        return (a.first + a.second) + (b.first + b.second);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 330);
}

// ---------------------------------------------------------------------
// Phase 1a deferred forms — parse, reject at codegen with a clear message.
// ---------------------------------------------------------------------

// ---------------------------------------------------------------------
// Phase 2a — `heap MyClass { f: v }` now works: malloc + per-field stores.
// ---------------------------------------------------------------------

TEST(UnifiedClassSyntaxTests, heapAggregateInitAllocatesStruct) {
    // `heap Foo { ... }` on a struct type allocates the body on the heap
    // (via malloc + memset) and stores each labeled binding into the
    // matching field. The resulting reference can be passed around like
    // any heap-allocated value; lifetime is owner-managed.
    auto src =
        "package test;\n"
        "public struct Point {\n"
        "    int32 x;\n"
        "    int32 y;\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Point p = heap Point { x: 3, y: 4 };\n"
        "        return p.x * p.x + p.y * p.y;\n"  // 25
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 25);
}

TEST(UnifiedClassSyntaxTests, heapAggregateInitOnClassDirectFieldRead) {
    // `heap` aggregate-init on a plain class returns the malloc'd
    // reference; field reads through that reference work normally. The
    // bypass in loadIfLValue keeps the catch-all from loading the full
    // class struct through the reference (which would corrupt the
    // receiving slot to only the first 8 bytes — the vtable pointer).
    auto src =
        "package test;\n"
        "public class Counter {\n"
        "    int32 n;\n"
        "    public Counter() { this.n = 0; }\n"  // ctor not called by aggregate-init
        "    public int32 value() { return this.n; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Counter c = heap Counter { n: 99 };\n"
        "        return c.n;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 99);
}

// ---------------------------------------------------------------------
// Phase 2b — stack aggregate-init lifted from struct-only to any class.
// ---------------------------------------------------------------------

TEST(UnifiedClassSyntaxTests, stackAggregateInitOnClass) {
    // `stack MyClass { ... }` on a plain class allocs the body on the
    // caller's frame, initializes the vtable slot, stores per-field
    // bindings, and returns a body pointer. Lifts the v1 struct-only
    // restriction on stack aggregate-init.
    auto src =
        "package test;\n"
        "public class Counter {\n"
        "    int32 n;\n"
        "    public Counter() { this.n = 0; }\n"
        "    public int32 value() { return this.n; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Counter c = stack Counter { n: 7 };\n"
        "        return c.value();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

TEST(UnifiedClassSyntaxTests, stackAggregateInitInitializesVtable) {
    // Confirms the stack path writes the class's vtable into slot 0.
    // Without that write, method dispatch on the stack instance would
    // segfault (loads slot 0 as the vtable pointer; null vtable + ptr
    // hash lookup → crash). The test passing proves the slot is
    // correctly initialized.
    auto src =
        "package test;\n"
        "public class Holder {\n"
        "    int32 a;\n"
        "    int32 b;\n"
        "    public Holder() { this.a = 0; this.b = 0; }\n"
        "    public int32 sum() { return this.a + this.b; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Holder h = stack Holder { a: 10, b: 20 };\n"
        "        return h.sum();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 30);
}

TEST(UnifiedClassSyntaxTests, heapAggregateInitOnClassVtableDispatches) {
    // The heap path also writes the class's vtable into slot 0. Method
    // calls on the aggregate-init'd instance dispatch normally — same
    // vtable layout as `new Counter()` / `heap Counter()`.
    auto src =
        "package test;\n"
        "public class Counter {\n"
        "    int32 n;\n"
        "    public Counter() { this.n = 0; }\n"  // ctor not called
        "    public int32 value() { return this.n; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Counter c = heap Counter { n: 42 };\n"
        "        return c.value();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// ---------------------------------------------------------------------
// Phase 2a — `stack MyClass(args)` now works: alloca + vtable + ctor.
// ---------------------------------------------------------------------

TEST(UnifiedClassSyntaxTests, stackConstructorCallAllocates) {
    // `stack MyClass(args)` allocates the class body in the caller's
    // frame, initializes the vtable slot, and dispatches to the matching
    // constructor with the supplied arguments. Lifetime is tied to the
    // enclosing scope; the borrow checker rejects escape (return / heap-
    // field-store) via the S10.3-generalized check.
    auto src =
        "package test;\n"
        "public class Counter {\n"
        "    int32 n;\n"
        "    public Counter(int32 initial) { this.n = initial; }\n"
        "    public void increment() { this.n = this.n + 1; }\n"
        "    public int32 value() { return this.n; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Counter c = stack Counter(10);\n"
        "        c.increment(); c.increment();\n"
        "        return c.value();\n"  // 12
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 12);
}

// ---------------------------------------------------------------------
// Phase 4 probe — covariant return types.
//
// Q2 in the rollout doc proposed landing covariant returns as a feature.
// Probe: cajeta's hash-based vtable hashes the canonical signature, which
// (per Method::buildCanonical) is `parent::name(paramTypes)` — return
// type is NOT part of the canonical. Override detection in
// CajetaClass::buildVirtualTable keys by canonical, so subclass overrides
// with narrower (covariant) returns should already collide with the base
// entry and replace it. Tests below verify this without needing any
// compiler change.
// ---------------------------------------------------------------------

TEST(UnifiedClassSyntaxTests, covariantReturnConcreteReceiverSeesNarrower) {
    // Base returns Base; subclass overrides with Derived return. Concrete-
    // receiver call site should see the narrower (Derived) return type so
    // the result can flow into a Derived-typed binding.
    auto src =
        "package test;\n"
        "public class Animal {\n"
        "    int32 id;\n"
        "    public Animal(int32 i) { this.id = i; }\n"
        "    public Animal copy() { return heap Animal(this.id); }\n"
        "}\n"
        "public class Dog extends Animal {\n"
        "    public Dog(int32 i) { this.id = i; }\n"
        "    public Dog copy() { return heap Dog(this.id); }\n"  // covariant: Dog narrower than Animal
        "    public int32 dogTag() { return this.id + 7; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Dog d = heap Dog(5);\n"
        "        Dog d2 = d.copy();\n"  // concrete: Dog return, Dog binding works only if covariant
        "        return d2.dogTag();\n"  // 5 + 7 = 12 — proves d2 is a Dog (has dogTag)
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 12);
}

// ---------------------------------------------------------------------
// Phase 3a — definite-assignment analysis (sequential case).
//
// `MyClass x;` declares the variable but does not assign; the scope's
// not-yet-assigned set tracks it. Reading before an assignment is a
// compile error (CAJETA_ERROR_VARIABLE_NOT_ASSIGNED). Assigning the
// variable removes the NYA mark; subsequent reads succeed.
//
// Sequential case only — control-flow merging (if/else, loops, switch,
// try/catch) lands in P3b.
// ---------------------------------------------------------------------

TEST(UnifiedClassSyntaxTests, definitelyAssignedRejectsReadBeforeInit) {
    auto src =
        "package test;\n"
        "public class Counter {\n"
        "    int32 n;\n"
        "    public Counter() { this.n = 0; }\n"
        "    public int32 value() { return this.n; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Counter c;\n"
        "        return c.value();\n"  // ← read before assignment
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.S");
        FAIL() << "expected CAJETA_ERROR_VARIABLE_NOT_ASSIGNED";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_VARIABLE_NOT_ASSIGNED");
    }
}

TEST(UnifiedClassSyntaxTests, definitelyAssignedAllowsReadAfterAssignment) {
    // Simpler shape: primitive assignment to bypass the heap-class
    // drop-chain interaction. Tests just the NYA → DA transition for
    // a bare int32 local.
    auto src =
        "package test;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32 x;\n"
        "        x = 42;\n"
        "        return x;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

TEST(UnifiedClassSyntaxTests, definitelyAssignedInitializerCountsAsAssignment) {
    // A local declared WITH an initializer is DA from birth; no NYA mark.
    auto src =
        "package test;\n"
        "public class Counter {\n"
        "    int32 n;\n"
        "    public Counter(int32 v) { this.n = v; }\n"
        "    public int32 value() { return this.n; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Counter c = heap Counter(7);\n"
        "        return c.value();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

TEST(UnifiedClassSyntaxTests, definitelyAssignedTracksMultipleLocalsIndependently) {
    // Two unrelated locals declared without initializers; reading either
    // before assignment is rejected independently. One gets assigned;
    // the other doesn't and is read — that's the error.
    auto src =
        "package test;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32 a;\n"
        "        int32 b;\n"
        "        a = 10;\n"
        "        return b;\n"  // ← b never assigned
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.S");
        FAIL() << "expected CAJETA_ERROR_VARIABLE_NOT_ASSIGNED";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_VARIABLE_NOT_ASSIGNED");
    }
}

TEST(UnifiedClassSyntaxTests, definitelyAssignedAcceptsMultipleLocalsAllInitialized) {
    auto src =
        "package test;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32 a;\n"
        "        int32 b;\n"
        "        a = 10;\n"
        "        b = 32;\n"
        "        return a + b;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

TEST(UnifiedClassSyntaxTests, covariantReturnBaseReceiverSeesWider) {
    // Same override shape; receiver typed as the base. The call site
    // sees the base's return type (Animal); the runtime still dispatches
    // to the override (Dog's body returns a Dog, but the caller binding
    // sees it as Animal). Confirms the override is wired through the
    // base-typed reference too.
    auto src =
        "package test;\n"
        "public class Animal {\n"
        "    int32 id;\n"
        "    public Animal(int32 i) { this.id = i; }\n"
        "    public Animal copy() { return heap Animal(this.id); }\n"
        "    public int32 tag() { return this.id; }\n"
        "}\n"
        "public class Dog extends Animal {\n"
        "    public Dog(int32 i) { this.id = i; }\n"
        "    public Dog copy() { return heap Dog(this.id); }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Animal a = heap Dog(42);\n"
        "        Animal b = a.copy();\n"  // copy() goes to Dog::copy via vtable
        "        return b.tag();\n"  // 42 — dispatches through Animal's vtable slot
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

TEST(UnifiedClassSyntaxTests, stackAndHeapInstancesShareConcreteDispatch) {
    // Concrete-receiver dispatch through the vtable lands on the same
    // method whether the instance is stack- or heap-allocated. The vtable
    // slot at offset 0 is initialized identically by both allocation
    // paths.
    //
    // Receiver typed as the concrete class (not the base). Polymorphic
    // dispatch through a base-typed reference is a separate concern (the
    // hash-based vtable lookup uses the receiver's declared method
    // canonical, which today resolves by declared class name — Phase 2c
    // work will reconcile override-by-name with vtable-by-hash).
    auto src =
        "package test;\n"
        "public class Counter {\n"
        "    int32 n;\n"
        "    public Counter(int32 initial) { this.n = initial; }\n"
        "    public int32 value() { return this.n; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Counter onStack = stack Counter(5);\n"
        "        Counter onHeap  = heap  Counter(7);\n"
        "        return onStack.value() + onHeap.value();\n"  // 12
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 12);
}

// ---------------------------------------------------------------------
// Phase 1b — bare `MyClass(args)` rejected; use heap/stack/new prefix.
// ---------------------------------------------------------------------

TEST(UnifiedClassSyntaxTests, bareClassConstructionRejected) {
    // `MyClass(args)` without an allocation prefix used to parse as a
    // method call and crash downstream. Now: clean rejection with a
    // message pointing at the heap/stack/new alternatives.
    auto src =
        "package test;\n"
        "public class Counter {\n"
        "    int32 n;\n"
        "    public Counter() { this.n = 0; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Counter c = Counter();\n"  // ← bare construction
        "        return 0;\n"
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.S");
        FAIL() << "expected CAJETA_ERROR_BARE_CLASS_CONSTRUCTION";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_BARE_CLASS_CONSTRUCTION");
    }
}

TEST(UnifiedClassSyntaxTests, bareStructConstructionRejected) {
    // Same rejection applies to struct types — v1 had `Foo(args)` on a
    // struct segfault (S6.1); the rejection turns that into a clean
    // diagnostic. Aggregate-init via `Foo { ... }` continues to work.
    auto src =
        "package test;\n"
        "public struct Point {\n"
        "    int32 x;\n"
        "    int32 y;\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Point p = Point(3, 4);\n"  // ← bare construction on struct
        "        return p.x;\n"
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.S");
        FAIL() << "expected CAJETA_ERROR_BARE_CLASS_CONSTRUCTION";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_BARE_CLASS_CONSTRUCTION");
    }
}

TEST(UnifiedClassSyntaxTests, viewConstructionStillWorks) {
    // Views keep their legacy `MyView(bytes)` construction form. The
    // bare-class-rejection above must not catch this path. Backing array
    // is `int32[]` matching the pattern used elsewhere in view tests.
    auto src =
        "package test;\n"
        "@HostEndian\n"
        "public view Header {\n"
        "    int32 magic;\n"
        "    int32 version;\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] buf = new int32[4];\n"
        "        Header h = Header(buf);\n"  // ← legacy view-construction
        "        h.version = 42;\n"
        "        return h.version;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

TEST(UnifiedClassSyntaxTests, newKeywordStillWorks) {
    // `new` continues to work during the deprecation cycle. No warning
    // emitted yet — warnings land when the alias retires.
    auto src =
        "package test;\n"
        "public class Holder {\n"
        "    int32 value;\n"
        "    public Holder(int32 v) { this.value = v; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Holder h = new Holder(99);\n"
        "        return h.value;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 99);
}
