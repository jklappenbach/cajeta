//
// Phase 1a — heap/stack expression prefixes for class allocation.
//
// First sub-phase of the unified-class rollout (docs/specification/lang/UnifiedClasses.md):
//   - `heap MyClass(args)` parses and behaves as today's `heap MyClass(args)`
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
// `heap MyClass(args)` continues to work unchanged during the deprecation
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
// `heap MyClass(args)` — synonym for today's `heap MyClass(args)`.
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

// ---------------------------------------------------------------------
// `stack MyClass { ... }` — synonym for today's bare aggregate-init.
// ---------------------------------------------------------------------

TEST(UnifiedClassSyntaxTests, stackAggregateInitDeclares) {
    // Aggregate-init via the explicit `stack` keyword.
    auto src =
        "package test;\n"
        "public class Point {\n"
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
        "public class Pair {\n"
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

TEST(UnifiedClassSyntaxTests, heapAggregateInitAllocates) {
    // `heap Foo { ... }` allocates the body on the heap (via malloc +
    // memset) and stores each labeled binding into the matching field.
    // The resulting reference can be passed around like any heap-allocated
    // value; lifetime is owner-managed.
    auto src =
        "package test;\n"
        "public class Point {\n"
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
// Phase 7.3 — owned class-ref fields in stack-allocated classes drop
// at scope exit (without freeing the stack body).
// ---------------------------------------------------------------------

TEST(UnifiedClassSyntaxTests, stackClassWithOwnedHeapField) {
    // Stack-allocated class with an owned heap-class-ref field.
    // Holder's stack-drop walks the field at scope exit, calls Hello's
    // heap-drop on the heap pointer, freeing it. The stack-drop entry
    // itself pops once (the count). Hello's drop is called directly
    // inside the body — not through the drop chain — so its free
    // doesn't bump the counter.
    //
    // Pre-P7.3+ this crashed with `free(): invalid pointer` because
    // BinaryOpExpression's `=` LHS load coerced the heap Hello return
    // into a load of the 8-byte Hello struct (a vtable pointer), and
    // h ended up holding the vtable address — which Hello's drop then
    // freed. Fix: loadIfLValue bypasses NewExpression results.
    auto src =
        "package test;\n"
        "public class Hello {\n"
        "    public Hello() { }\n"
        "}\n"
        "public class Holder {\n"
        "    Hello h;\n"
        "    public Holder() { this.h = heap Hello(); }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 inner() {\n"
        "        Holder owner = stack Holder();\n"
        "        return 0;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Cajeta.dropCountReset();\n"
        "        inner();\n"
        "        return (int32) Cajeta.dropCount();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

TEST(UnifiedClassSyntaxTests, stackClassNoOwnedFieldsScopeExits) {
    // Probe: stack-allocated class with only primitive fields. The
    // stack-drop fires (1 pop) but has nothing to walk.
    auto src =
        "package test;\n"
        "public class Counter {\n"
        "    int32 n;\n"
        "    public Counter(int32 v) { this.n = v; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 inner() {\n"
        "        Counter c = stack Counter(7);\n"
        "        return c.n;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Cajeta.dropCountReset();\n"
        "        inner();\n"
        "        return (int32) Cajeta.dropCount();\n"
        "    }\n"
        "}\n";
    // Stack-drop entry registered for `c`; fires at inner's scope exit.
    // Body isn't freed. Drop count = 1 (the stack-drop entry pop).
    EXPECT_EQ(runI32(src), 1);
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


// ---------------------------------------------------------------------
// Phase 2a — `stack MyClass(args)` now works: alloca + vtable + ctor.
// ---------------------------------------------------------------------


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





// ---------------------------------------------------------------------
// Phase 3b — if/else merging for definite-assignment.
//
// Variable is DA after if/else iff DA in BOTH branches. Missing else
// means the un-taken path doesn't assign, so post-if NYA = pre-if NYA.
// ---------------------------------------------------------------------

TEST(UnifiedClassSyntaxTests, definitelyAssignedIfElseBothBranchesAssign) {
    // Both branches assign x; post-if x is DA.
    auto src =
        "package test;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32 x;\n"
        "        boolean cond = true;\n"
        "        if (cond) { x = 10; } else { x = 20; }\n"
        "        return x;\n"  // OK
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 10);
}

TEST(UnifiedClassSyntaxTests, definitelyAssignedIfWithoutElseLeavesNya) {
    // Only the then-branch assigns; post-if x is still NYA (the not-taken
    // path didn't assign, so we can't guarantee assignment).
    auto src =
        "package test;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32 x;\n"
        "        boolean cond = true;\n"
        "        if (cond) { x = 10; }\n"
        "        return x;\n"  // ← rejected — missing else
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.S");
        FAIL() << "expected CAJETA_ERROR_VARIABLE_NOT_ASSIGNED";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_VARIABLE_NOT_ASSIGNED");
    }
}

TEST(UnifiedClassSyntaxTests, definitelyAssignedIfElseElseOnlyAssignsLeavesNya) {
    // Only the else-branch assigns; post-if x is NYA (then path skips).
    auto src =
        "package test;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32 x;\n"
        "        boolean cond = true;\n"
        "        if (cond) { } else { x = 20; }\n"
        "        return x;\n"  // ← rejected
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.S");
        FAIL() << "expected CAJETA_ERROR_VARIABLE_NOT_ASSIGNED";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_VARIABLE_NOT_ASSIGNED");
    }
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
        "        int32[] buf = heap int32[4];\n"
        "        Header h = Header(buf);\n"  // ← legacy view-construction
        "        h.version = 42;\n"
        "        return h.version;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}
