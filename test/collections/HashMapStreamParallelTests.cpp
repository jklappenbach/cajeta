// Parallel-terminal tests over HashMap's stream views (keys / values /
// entries). The HashMap stream classes implement `Splittable<T>` so
// `.parallel()` terminals fork through ParallelDriver instead of
// falling back to sequential. Per StreamParallelism.md § P4
// (Source coverage) — this is the verification that HashMap views
// participate in the parallel driver end-to-end.
//
// Each test uses ≥ 64 entries so the source crosses MIN_PER_SPLIT * 2
// (32 * 2) and the driver actually picks nSplits ≥ 2. Smaller-source
// fallback paths are covered by HashMapStreamTests.cpp.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include "../../src/cajeta/error/Exception.h"

#include <cstdint>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

int32_t runI32Diag(const std::string& src) {
    try {
        return runI32(src);
    } catch (cajeta::Exception& e) {
        ADD_FAILURE() << "cajeta::Exception " << e.getErrorId()
                      << ": " << e.getMessage();
        return -1;
    } catch (const std::exception& e) {
        ADD_FAILURE() << "std::exception: " << e.what();
        return -1;
    }
}

int64_t runI64Diag(const std::string& src) {
    try {
        auto jit = CajetaJit::compile(src, "test.D");
        auto fn = jit->lookup<int64_t (*)()>("run");
        return fn();
    } catch (cajeta::Exception& e) {
        ADD_FAILURE() << "cajeta::Exception " << e.getErrorId()
                      << ": " << e.getMessage();
        return -1;
    } catch (const std::exception& e) {
        ADD_FAILURE() << "std::exception: " << e.what();
        return -1;
    }
}

} // namespace

// keys().parallel().count() on a populated map equals the map size.
// 64 entries puts us above MIN_PER_SPLIT*2 so the driver forks rather
// than falling back to sequential. The HashMapKeyStream contributes
// trySplit / splittableSize / trySplitRoot from its Splittable<K>
// implementation; the parallel chain walker uses those to partition
// the slot range across workers.
TEST(HashMapStreamParallelTests, parallelKeysCountMatchesSize) {
    auto src =
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "public class Tag { public Tag() { return; } }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<Tag, int32> m = heap HashMap<Tag, int32>(256);\n"
        "        int32 i = 0;\n"
        "        while (i < 64) {\n"
        "            m.put(heap Tag(), i);\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return m.keys().parallel().count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Diag(src), 64);
}

// values().parallel().count() — same shape over the HashMapValueStream
// view. Each Splittable view splits the same underlying slot range
// independently; this confirms the values view exposes the same
// machinery as the keys view.
TEST(HashMapStreamParallelTests, parallelValuesCountMatchesSize) {
    auto src =
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "public class Tag { public Tag() { return; } }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<Tag, int32> m = heap HashMap<Tag, int32>(256);\n"
        "        int32 i = 0;\n"
        "        while (i < 64) {\n"
        "            m.put(heap Tag(), i);\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return m.values().parallel().count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Diag(src), 64);
}

// entries().parallel().count() — over Pair<K, V>. The entry view
// yields fresh Pair objects per slot; the parallel terminal must
// still count exactly the live-slot count and the per-worker Pair
// allocations must not race (each worker walks a disjoint slot range
// and constructs its own pairs).
TEST(HashMapStreamParallelTests, parallelEntriesCountMatchesSize) {
    auto src =
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "public class Tag { public Tag() { return; } }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<Tag, int32> m = heap HashMap<Tag, int32>(256);\n"
        "        int32 i = 0;\n"
        "        while (i < 64) {\n"
        "            m.put(heap Tag(), i);\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return m.entries().parallel().count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Diag(src), 64);
}

// values().parallel().reduce — actually splits and combines. Sum of
// 0..63 = 2016. `+` is associative and 0 is its identity, so the
// parallel reduce contract is satisfied and the result must match
// the sequential answer.
TEST(HashMapStreamParallelTests, parallelValuesReduceSumsCorrectly) {
    auto src =
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "public class Tag { public Tag() { return; } }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<Tag, int32> m = heap HashMap<Tag, int32>(256);\n"
        "        int32 i = 0;\n"
        "        while (i < 64) {\n"
        "            m.put(heap Tag(), i);\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return m.values().parallel().reduce(0, (a, b) -> a + b);\n"
        "    }\n"
        "}\n";
    // sum(0..63) = 63*64/2 = 2016
    EXPECT_EQ(runI32Diag(src), 2016);
}

// anyMatch on values — true when some element satisfies the
// predicate. Driver flips a shared `hit` flag; sibling workers bail
// at their next pull checkpoint.
TEST(HashMapStreamParallelTests, parallelValuesAnyMatchTrue) {
    auto src =
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "public class Tag { public Tag() { return; } }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<Tag, int32> m = heap HashMap<Tag, int32>(256);\n"
        "        int32 i = 0;\n"
        "        while (i < 64) {\n"
        "            m.put(heap Tag(), i);\n"
        "            i = i + 1;\n"
        "        }\n"
        "        boolean b = m.values().parallel().anyMatch((v) -> v == 42);\n"
        "        if (b) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Diag(src), 1);
}

// anyMatch on values — false when no element matches. Empty-flag
// boundary: every worker drains its share without finding a hit, the
// orchestrator's tail walk also finds none, returns false.
TEST(HashMapStreamParallelTests, parallelValuesAnyMatchFalse) {
    auto src =
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "public class Tag { public Tag() { return; } }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<Tag, int32> m = heap HashMap<Tag, int32>(256);\n"
        "        int32 i = 0;\n"
        "        while (i < 64) {\n"
        "            m.put(heap Tag(), i);\n"
        "            i = i + 1;\n"
        "        }\n"
        "        boolean b = m.values().parallel().anyMatch((v) -> v > 999);\n"
        "        if (b) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Diag(src), 0);
}

// allMatch on values — true when every element satisfies the
// predicate. The driver uses findFailWorker; here no worker finds a
// fail, so the result is true.
TEST(HashMapStreamParallelTests, parallelValuesAllMatchTrue) {
    auto src =
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "public class Tag { public Tag() { return; } }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<Tag, int32> m = heap HashMap<Tag, int32>(256);\n"
        "        int32 i = 0;\n"
        "        while (i < 64) {\n"
        "            m.put(heap Tag(), i);\n"
        "            i = i + 1;\n"
        "        }\n"
        "        boolean b = m.values().parallel().allMatch((v) -> v >= 0);\n"
        "        if (b) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Diag(src), 1);
}

// noneMatch — true when no element satisfies the predicate. Drives
// findHitWorker; a hit means the result is false.
TEST(HashMapStreamParallelTests, parallelValuesNoneMatchTrue) {
    auto src =
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "public class Tag { public Tag() { return; } }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<Tag, int32> m = heap HashMap<Tag, int32>(256);\n"
        "        int32 i = 0;\n"
        "        while (i < 64) {\n"
        "            m.put(heap Tag(), i);\n"
        "            i = i + 1;\n"
        "        }\n"
        "        boolean b = m.values().parallel().noneMatch((v) -> v < 0);\n"
        "        if (b) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Diag(src), 1);
}

// forEach over values under .parallel() — workers accumulate side
// effects into a shared static counter via atomic add. The visit
// order is unspecified across workers, but every element must be
// visited exactly once, so the final sum matches the sequential
// total. The accumulator MUST be AtomicInt32.fetchAdd: the first cut
// wrote a plain `total = total + v`, whose lost updates never showed
// on x86 (TSO + partitioning luck) and then failed the v0.12.1
// release's aarch64-apple-darwin leg by exactly a dropped chunk —
// a racy test, not a stream defect.
TEST(HashMapStreamParallelTests, parallelValuesForEachSumsCorrectly) {
    auto src =
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "import cajeta.concurrent.AtomicInt32;\n"
        "public class Tag { public Tag() { return; } }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<Tag, int32> m = heap HashMap<Tag, int32>(256);\n"
        "        int32 i = 0;\n"
        "        while (i < 64) {\n"
        "            m.put(heap Tag(), i + 1);\n"
        "            i = i + 1;\n"
        "        }\n"
        "        AtomicInt32 total = heap AtomicInt32(0);\n"
        "        m.values().parallel().forEach((int32 v) -> total.fetchAdd(v));\n"
        "        return total.load();\n"
        "    }\n"
        "}\n";
    // sum(1..64) = 2080
    EXPECT_EQ(runI32Diag(src), 2080);
}

// fold with combiner — cross-type fold into int64 accumulator.
// Exercises foldParallelChain<T, R> over a HashMap view. The combiner
// `(R, R) -> R` runs at join-time, separately from the per-element
// accumulator `(R, T) -> R`.
TEST(HashMapStreamParallelTests, parallelValuesFoldWithCombiner) {
    auto src =
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "public class Tag { public Tag() { return; } }\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        HashMap<Tag, int32> m = heap HashMap<Tag, int32>(256);\n"
        "        int32 i = 0;\n"
        "        while (i < 64) {\n"
        "            m.put(heap Tag(), i + 1);\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return m.values().parallel().fold<int64>(\n"
        "            0L,\n"
        "            (acc, v) -> acc + (int64) v,\n"
        "            (a, b) -> a + b);\n"
        "    }\n"
        "}\n";
    // sum(1..64) = 2080
    EXPECT_EQ(runI64Diag(src), 2080L);
}

// sequential() flips the parallel flag back. Count must still be
// correct, but the dispatch goes through Stream<T>.count's sequential
// next() loop — confirms the HashMap view inherits the base terminal
// path when the flag is cleared.
TEST(HashMapStreamParallelTests, valuesParallelThenSequentialCountCorrect) {
    auto src =
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "public class Tag { public Tag() { return; } }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<Tag, int32> m = heap HashMap<Tag, int32>(256);\n"
        "        int32 i = 0;\n"
        "        while (i < 64) {\n"
        "            m.put(heap Tag(), i);\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return m.values().parallel().sequential().count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Diag(src), 64);
}

// Below MIN_PER_SPLIT*2 (64), the driver falls back to sequential
// even with .parallel() set. The terminal must still return the
// correct count; this exercises the small-source branch of the
// chain walker's `if (nSplits < 2)` guard.
TEST(HashMapStreamParallelTests, parallelValuesSmallSourceStillCorrect) {
    auto src =
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "public class Tag { public Tag() { return; } }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<Tag, int32> m = heap HashMap<Tag, int32>(16);\n"
        "        m.put(heap Tag(), 1);\n"
        "        m.put(heap Tag(), 2);\n"
        "        m.put(heap Tag(), 3);\n"
        "        return m.values().parallel().count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Diag(src), 3);
}
