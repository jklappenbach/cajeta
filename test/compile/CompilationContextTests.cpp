#include <gtest/gtest.h>
#include <thread>
#include "cajeta/compile/CompilationContext.h"

using namespace cajeta;

// Unit 1 (thread-safe compiler): the thread-local current-context seam.

// 1.1.2 — ScopedCompilationContext sets then restores the previous current,
// nesting-safe (whatever was current before is restored on exit).
TEST(CompilationContextTests, scopedSetsAndRestoresPrev) {
    CompilationContext* before = currentCompilationContext();
    CompilationContext a;
    {
        ScopedCompilationContext sa(&a);
        EXPECT_EQ(currentCompilationContext(), &a);
        CompilationContext b;
        {
            ScopedCompilationContext sb(&b);
            EXPECT_EQ(currentCompilationContext(), &b);
        }
        EXPECT_EQ(currentCompilationContext(), &a);
    }
    EXPECT_EQ(currentCompilationContext(), before);
}

// 1.1.1 — each thread resolves to its own current context; a fresh thread
// starts with none, and one thread's scope never bleeds into another's.
TEST(CompilationContextTests, perThreadIsolation) {
    CompilationContext mainCtx;
    ScopedCompilationContext sm(&mainCtx);
    ASSERT_EQ(currentCompilationContext(), &mainCtx);

    CompilationContext* freshThreadSaw = reinterpret_cast<CompilationContext*>(0x1);
    CompilationContext* otherDuringScope = nullptr;
    std::thread t([&] {
        freshThreadSaw = currentCompilationContext();   // expect nullptr
        CompilationContext other;
        ScopedCompilationContext so(&other);
        otherDuringScope = currentCompilationContext();  // expect &other
    });
    t.join();

    EXPECT_EQ(freshThreadSaw, nullptr);                  // fresh thread: no ctx
    EXPECT_EQ(currentCompilationContext(), &mainCtx);    // main thread unaffected
    // The worker's &other is a dangling compare-only value; assert it was set to
    // a non-null, non-main pointer during its scope.
    EXPECT_NE(otherDuringScope, nullptr);
    EXPECT_NE(otherDuringScope, &mainCtx);
}
