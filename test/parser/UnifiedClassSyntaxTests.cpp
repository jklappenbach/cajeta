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

TEST(UnifiedClassSyntaxTests, heapAggregateInitRejectedAsPhase2) {
    // `heap MyClass { ... }` needs new codegen (heap-alloc + per-field
    // stores). Defers to Phase 2 alongside the CajetaStruct collapse.
    auto src =
        "package test;\n"
        "public struct Point {\n"
        "    int32 x;\n"
        "    int32 y;\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Point p = heap Point { x: 1, y: 2 };\n"
        "        return p.x;\n"
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.S");
        FAIL() << "expected CAJETA_ERROR_HEAP_AGGREGATE_INIT_UNIMPLEMENTED";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_HEAP_AGGREGATE_INIT_UNIMPLEMENTED");
    }
}

TEST(UnifiedClassSyntaxTests, stackConstructorCallRejectedAsPhase2) {
    // `stack MyClass(args)` needs new codegen (alloca + ctor invocation).
    // Defers to Phase 2 alongside the CajetaStruct collapse.
    auto src =
        "package test;\n"
        "public class Counter {\n"
        "    int32 n;\n"
        "    public Counter() { this.n = 0; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Counter c = stack Counter();\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.S");
        FAIL() << "expected CAJETA_ERROR_STACK_CTOR_UNIMPLEMENTED";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_STACK_CTOR_UNIMPLEMENTED");
    }
}
