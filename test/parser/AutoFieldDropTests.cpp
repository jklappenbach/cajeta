//
// MemoryModel.md § Known gaps — automatic field drops.
//
// When a heap-allocated class instance drops, its synthesized drop
// wrapper should automatically release every owned heap field
// (class-typed refs and arrays), in reverse declaration order,
// without requiring the user to write an explicit `~ClassName()`
// destructor that calls each field's drop. Stack allocations
// already had this behavior (verified in UnifiedClassSyntaxTests's
// `stackClassWithOwnedHeapField`); the heap path was the gap.
//
// Observability: heap drops route through __cajeta_drop_pop_run
// which bumps the dropCount atomic. The auto-drop here calls the
// field class's drop wrapper DIRECTLY (no chain entry, no
// dropCount increment for that call). To observe a field auto-drop
// firing, the field's class needs a user destructor whose body
// allocates a tracked resource — that resource's drop entry
// bumps dropCount when it fires.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// Heap-allocated parent owns one heap-allocated class-ref field.
// When parent drops, the field's destructor should fire even
// without a user-defined parent destructor.
//
// Without auto field drop:
//   - Holder entry fires → count += 1 (pop_run pre-increment)
//   - Holder wrapper: no user drop, skips class-ref fields, frees body
//   - Tracer leaks; ~Tracer() never runs; no array drop; count = 1
//
// With auto field drop (target):
//   - Holder entry fires → count += 1
//   - Holder wrapper: walks fields, calls Tracer's drop directly
//   - Tracer wrapper: runs ~Tracer() → allocates int32[1]
//   - int32[1]'s drop entry fires at ~Tracer scope exit → count += 1
//   - Total: 2
TEST(AutoFieldDropTests, heapClassAutoDropsOwnedClassRefField) {
    auto src =
        "package test;\n"
        "public class Tracer {\n"
        "    public ~Tracer() {\n"
        "        int32[] junk = new int32[1];\n"
        "    }\n"
        "}\n"
        "public class Holder {\n"
        "    public Tracer t;\n"
        "    public Holder() { this.t = heap Tracer(); }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cajeta.dropCountReset();\n"
        "        Holder h = heap Holder();\n"
        "        return (int32) 0;\n"
        "    }\n"
        "    public static int64 read() {\n"
        "        return Cajeta.dropCount();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    jit->lookup<int32_t (*)()>("run")();
    EXPECT_EQ(jit->lookup<int64_t (*)()>("read")(), 2);
}

// Two owned class-ref fields. Each Tracer destructor adds 1 to the
// count via its array allocation. Holder's entry pre-increments
// once. Total: 1 + 2 = 3.
TEST(AutoFieldDropTests, heapClassAutoDropsMultipleClassRefFields) {
    auto src =
        "package test;\n"
        "public class Tracer {\n"
        "    public ~Tracer() {\n"
        "        int32[] junk = new int32[1];\n"
        "    }\n"
        "}\n"
        "public class Holder {\n"
        "    public Tracer a;\n"
        "    public Tracer b;\n"
        "    public Holder() {\n"
        "        this.a = heap Tracer();\n"
        "        this.b = heap Tracer();\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cajeta.dropCountReset();\n"
        "        Holder h = heap Holder();\n"
        "        return 0;\n"
        "    }\n"
        "    public static int64 read() {\n"
        "        return Cajeta.dropCount();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    jit->lookup<int32_t (*)()>("run")();
    EXPECT_EQ(jit->lookup<int64_t (*)()>("read")(), 3);
}

// Two-level chain: Inner owns a Tracer, Outer owns an Inner. When
// Outer drops, the recursion goes Outer → Inner → Tracer.
//   Outer entry: +1
//   Outer wrapper auto-drops Inner.t (which is the Inner field)
//     Inner's wrapper auto-drops Tracer
//       ~Tracer's array → +1
// Total: 2
TEST(AutoFieldDropTests, heapClassAutoDropsNestedClassRefs) {
    auto src =
        "package test;\n"
        "public class Tracer {\n"
        "    public ~Tracer() {\n"
        "        int32[] junk = new int32[1];\n"
        "    }\n"
        "}\n"
        "public class Inner {\n"
        "    public Tracer t;\n"
        "    public Inner() { this.t = heap Tracer(); }\n"
        "}\n"
        "public class Outer {\n"
        "    public Inner inner;\n"
        "    public Outer() { this.inner = heap Inner(); }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cajeta.dropCountReset();\n"
        "        Outer o = heap Outer();\n"
        "        return 0;\n"
        "    }\n"
        "    public static int64 read() {\n"
        "        return Cajeta.dropCount();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    jit->lookup<int32_t (*)()>("run")();
    EXPECT_EQ(jit->lookup<int64_t (*)()>("read")(), 2);
}

// Null-safe: when a class-ref field is left null (constructor doesn't
// assign), the auto-drop must NOT crash on the null pointer. Pre-fix
// the user would crash if their destructor dereferenced an
// unassigned field; auto-drop with a null guard handles it cleanly.
TEST(AutoFieldDropTests, heapClassAutoDropTolerateNullField) {
    auto src =
        "package test;\n"
        "public class Tracer {\n"
        "    public ~Tracer() {\n"
        "        int32[] junk = new int32[1];\n"
        "    }\n"
        "}\n"
        "public class Holder {\n"
        "    public Tracer t;\n"  // never assigned — stays zero/null
        "    public Holder() { }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cajeta.dropCountReset();\n"
        "        Holder h = heap Holder();\n"
        "        return 0;\n"
        "    }\n"
        "    public static int64 read() {\n"
        "        return Cajeta.dropCount();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    jit->lookup<int32_t (*)()>("run")();
    // Holder entry: +1. Tracer's auto-drop short-circuits on null,
    // no ~Tracer body, no array allocation. Total: 1.
    EXPECT_EQ(jit->lookup<int64_t (*)()>("read")(), 1);
}

// Array field — smoke test. Verify the program runs without crash
// when a class owns an array field that gets auto-freed at parent
// drop. We can't observe array free via dropCount (the free is a
// direct call, not a chain entry), but a use-after-free /
// double-free would crash glibc's heap checker.
TEST(AutoFieldDropTests, heapClassAutoDropsArrayField) {
    auto src =
        "package test;\n"
        "public class Holder {\n"
        "    public int32[] arr;\n"
        "    public Holder() { this.arr = new int32[3]; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Holder h = heap Holder();\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}

// Class-ref field auto-drop must dispatch through the vtable too —
// declared field type Animal, dynamic instance type Dog. ~Dog()
// should fire, not ~Animal(). Combines Gap 1 (virtual drop) with
// auto field drop.
TEST(AutoFieldDropTests, classRefFieldAutoDropDispatchesVirtually) {
    auto src =
        "package test;\n"
        "public class Animal {\n"
        "    public int32 sound() { return 1; }\n"
        "}\n"
        "public class Dog extends Animal {\n"
        "    public ~Dog() {\n"
        "        int32[] junk = new int32[1];\n"
        "    }\n"
        "}\n"
        "public class Owner {\n"
        "    public Animal a;\n"   // declared base type
        "    public Owner() { this.a = heap Dog(); }\n"  // dynamic = Dog
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cajeta.dropCountReset();\n"
        "        Owner o = heap Owner();\n"
        "        return 0;\n"
        "    }\n"
        "    public static int64 read() {\n"
        "        return Cajeta.dropCount();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    jit->lookup<int32_t (*)()>("run")();
    // Owner entry: +1. ~Dog() fires via virtual dispatch + auto-drop,
    // its array drop: +1. Total: 2. If ~Animal() ran instead (static
    // dispatch from the declared field type), no array → total = 1.
    EXPECT_EQ(jit->lookup<int64_t (*)()>("read")(), 2);
}
