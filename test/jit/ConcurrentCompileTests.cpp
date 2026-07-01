//
// ConcurrentCompileTests — threadsafe-compiler U5 (concurrency-first): prove that
// two independent compiles can run on two threads without corrupting each other.
// After the thread_local migration (U2-U4) + per-thread stdlib (U5), each thread
// fully primes its own stdlib and compiles in its own LLVMContext. This is the
// probe that reveals any remaining process-global mutable state shared across the
// compile/JIT path. (Memory-heavy by design — the frozen-shared optimization
// later shares one stdlib image; this step only proves correctness.)
//

#include <gtest/gtest.h>

#include "JitTestHelper.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

using cajeta_test::CajetaJit;

namespace {

int32_t compileAndRun(int seed) {
    std::string cls = "Conc" + std::to_string(seed);
    std::string src =
        "package test;\n"
        "public final class " + cls + " {\n"
        "    public static int32 run() { return " + std::to_string(seed) + " * 7; }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test." + cls);
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

TEST(ConcurrentCompileTests, twoThreadsCompileIndependently) {
    // Warm up one-time LLVM target-registry / JIT init on the main thread, so the
    // test isolates compiler-STATE concurrency (the thread_local migration) from
    // LLVM's one-time global init.
    ASSERT_EQ(compileAndRun(1), 7);

    int32_t a = 0, b = 0;
    std::thread t1([&] { a = compileAndRun(3); });
    std::thread t2([&] { b = compileAndRun(5); });
    t1.join();
    t2.join();

    EXPECT_EQ(a, 21) << "thread 1 result corrupted by concurrent compile";
    EXPECT_EQ(b, 35) << "thread 2 result corrupted by concurrent compile";
}

// Stronger probe: several threads each running multiple independent compiles
// concurrently. A single 2-thread pass can mask a race; this exercises many
// concurrent compiles + per-thread reuse (sequential compiles on one thread).
TEST(ConcurrentCompileTests, stressManyConcurrentCompiles) {
    ASSERT_EQ(compileAndRun(1), 7);  // warm up one-time init on the main thread

    constexpr int THREADS = 4;
    constexpr int ROUNDS = 4;
    std::atomic<int> failures{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < THREADS; ++t) {
        ts.emplace_back([t, &failures] {
            for (int r = 0; r < ROUNDS; ++r) {
                int seed = (t + 1) * 100 + r;          // distinct per (thread, round)
                if (compileAndRun(seed) != seed * 7) ++failures;
            }
        });
    }
    for (auto& th : ts) th.join();

    EXPECT_EQ(failures.load(), 0) << "a concurrent compile produced a wrong result";
}
