#include <gtest/gtest.h>
#include <thread>
#include "cajeta/compile/Compiler.h"

using namespace cajeta;

// Unit 4 (thread-safe compiler): the lazy-stdlib bookkeeping is thread_local, so
// what one thread has parsed is invisible to another. Priming the stdlib on the
// main thread must not make a fresh thread think it is already parsed.
TEST(LazyStdlibThreadLocalTests, parsedPackagesAreThreadLocal) {
    Compiler compiler;
    compiler.ensureStdlibModule();                       // populates main thread's set
    ASSERT_FALSE(Compiler::stdlibParsedPackages().empty());

    bool workerEmpty = false;
    std::thread t([&] {
        // A fresh thread that has parsed nothing sees its own empty set.
        workerEmpty = Compiler::stdlibParsedPackages().empty();
    });
    t.join();

    EXPECT_TRUE(workerEmpty)
        << "main-thread lazy-parse bookkeeping leaked into a fresh thread";
}
