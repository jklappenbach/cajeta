// Coverage for the four wrapper-array patterns Task #46's bare-template
// ptr fallback unblocked:
//
//   1. `Optional<T>[N]` as local/param/return in a `<T>` method template.
//   2. Other wrapper arrays in template context — `ArrayList<T>[]`,
//      `Pair<K,V>[]`.
//   3. Worker → shared-slot pattern: orchestrator allocates
//      `WrapperType<T>[N]`, passes to a worker, worker writes, orchestrator
//      reads. Mirrors the findAnyParallelChain shape the fix retired.
//   4. Batched / result-array return from method-template helpers.
//
// Each test exercises a shape that crashed at JIT time before
// CajetaClass::getLlvmType's bare-template fallback landed — see Task
// #46 and OptionalArrayWorkerWriteTests for the bisection.

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

int64_t runI64(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int64_t (*)()>("run");
    return fn();
}

} // namespace

// --- Pattern 1: Optional<T>[N] return type ------------------------------

// `Optional<T>[]` as a method-template RETURN type. Mirrors the
// findAnyParallelChain shape from the inside — caller receives a
// generic-class-typed array; the array was allocated inside a
// method-template body. Crashed pre-fix because the return-type
// resolution at visitMethodDeclaration time walked through the
// bare-template Optional.
TEST(WrapperArrayInTemplateTests, optionalArrayReturnFromMethodTemplate) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static final #Optional<T>[] singleton<T>(T seed) {\n"
        "        Optional<T>[] arr = new Optional<T>[1];\n"
        "        arr[0] = heap Optional<T>(true, seed);\n"
        "        return #arr;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Optional<int32>[] r = singleton<int32>(31);\n"
        "        if (r[0].isPresent()) {\n"
        "            return r[0].get();\n"
        "        }\n"
        "        return -1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 31);
}

// --- Pattern 2: ArrayList<T>[] in template context ----------------------

// `ArrayList<T>[]` as a local AND parameter inside a method template.
// ArrayList is a templated stdlib class; the array-of-ArrayList<T> is
// the headline shape for "partitioned per-worker accumulators" in any
// future parallel collect.
TEST(WrapperArrayInTemplateTests, arrayListArrayLocalAndParam) {
    auto src =
        "package test;\n"
        "import cajeta.collection.ArrayList;\n"
        "public final class D {\n"
        "    public static final int32 fillSlot<T>(ArrayList<T>[] slots, int32 idx, T v) {\n"
        "        slots[idx] = heap ArrayList<T>();\n"
        "        slots[idx].add(v);\n"
        "        return 0;\n"
        "    }\n"
        "    public static final int32 firstOfFirst<T>(T v) {\n"
        "        ArrayList<T>[] buckets = new ArrayList<T>[2];\n"
        "        fillSlot<T>(buckets, 0, v);\n"
        "        return buckets[0].count();\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        return firstOfFirst<int32>(99);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// --- Pattern 2 (cont): Pair<K,V>[] with two type parameters --------------

// `Pair<K,V>[]` exercises the multi-type-parameter wrapper-array
// shape. The fix has to hold even when the wrapped class itself is
// a multi-arg template, not just single-T.
TEST(WrapperArrayInTemplateTests, pairArrayWithTwoTypeParams) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static final #Pair<K, V>[] makePairs<K, V>(K k, V v) {\n"
        "        Pair<K, V>[] arr = new Pair<K, V>[1];\n"
        "        arr[0] = heap Pair<K, V>(k, v);\n"
        "        return #arr;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Pair<int32, int32>[] arr = makePairs<int32, int32>(7, 35);\n"
        "        return arr[0].first() + arr[0].second();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// --- Pattern 3: orchestrator/worker shared-slot of generic class --------

// Hand-rolled orchestrator + spawn worker, with the shared slot
// typed as a generic-class array (`Pair<T, int32>[1]`). Mirrors the
// findAnyParallelChain shape the workaround retired, but with a
// different wrapper to prove the fix isn't Optional-specific.
TEST(WrapperArrayInTemplateTests, workerSharedSlotPairResult) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static async int32 producer<T>(Pair<T, int32>[] out, T v) {\n"
        "        out[0] = heap Pair<T, int32>(v, 100);\n"
        "        return 0;\n"
        "    }\n"
        "    public static final int32 orchestrate<T>(T v) {\n"
        "        Pair<T, int32>[] slot = new Pair<T, int32>[1];\n"
        "        scope {\n"
        "            spawn producer<T>(slot, v);\n"
        "        }\n"
        "        return slot[0].second();\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        return orchestrate<int32>(7);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 100);
}

// --- Pattern 4: batched return of generic-class array -------------------

// Method-template helper that batches its work into a fixed-size
// generic-class array result. Demonstrates the "give me N buckets"
// ergonomic pattern. partition splits even/odd into two
// ArrayList<T> buckets and returns the pair.
TEST(WrapperArrayInTemplateTests, batchedBucketArrayReturn) {
    auto src =
        "package test;\n"
        "import cajeta.collection.ArrayList;\n"
        "public final class D {\n"
        "    public static final #ArrayList<T>[] partition<T>(T[] xs, int32 n,\n"
        "                                                     (T) -> boolean pred) {\n"
        "        ArrayList<T>[] buckets = new ArrayList<T>[2];\n"
        "        buckets[0] = heap ArrayList<T>();\n"
        "        buckets[1] = heap ArrayList<T>();\n"
        "        int32 i = 0;\n"
        "        while (i < n) {\n"
        "            T elem = xs[i];\n"
        "            if (pred(elem)) {\n"
        "                buckets[0].add(elem);\n"
        "            } else {\n"
        "                buckets[1].add(elem);\n"
        "            }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return #buckets;\n"
        "    }\n"
        "    public static int64 run() {\n"
        "        int32[] xs = new int32[6];\n"
        "        xs[0] = 1; xs[1] = 2; xs[2] = 3;\n"
        "        xs[3] = 4; xs[4] = 5; xs[5] = 6;\n"
        "        ArrayList<int32>[] parts =\n"
        "            partition<int32>(xs, 6, (int32 v) -> v % 2 == 0);\n"
        "        // Encode (evens, odds) counts as one int64.\n"
        "        return ((int64) parts[0].count()) * 10L\n"
        "             +  (int64) parts[1].count();\n"
        "    }\n"
        "}\n";
    // evens [2,4,6] → 3; odds [1,3,5] → 3; encoded 33.
    EXPECT_EQ(runI64(src), 33LL);
}
