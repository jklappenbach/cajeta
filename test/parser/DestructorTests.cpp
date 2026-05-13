//
// Tests for the C++-style `~ClassName()` destructor syntax. The
// destructor is sugar for the class's drop method — the visitor builds
// it as a method internally named "drop" so the class-drop wrapper
// (CajetaClass::getOrCreateDropFunction) finds it without further
// special-casing. The destructor name must match the enclosing class
// (validated during the visit; mismatch throws CAJETA_ERROR_TYPE).
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

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

// Smoke: a class with `~ClassName()` declares a destructor that runs at
// scope exit. We observe its execution the same way ClassDropTests
// does — the destructor allocates an int32[] whose own drop bumps the
// count, so a destructor-having class contributes 2 increments instead
// of 1.
TEST(DestructorTests, destructorRuns) {
    auto src =
        "package test;\n"
        "public class Resource {\n"
        "    public ~Resource() {\n"
        "        int32[] junk = new int32[1];\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cajeta.dropCountReset();\n"
        "        Resource r = new Resource();\n"
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

// Destructor sees `this`. Verifies the same parameter-injection that
// regular instance methods get applies to destructors too.
TEST(DestructorTests, destructorCanAccessFields) {
    auto src =
        "package test;\n"
        "public class Tracker {\n"
        "    public int32 marker;\n"
        "    public Tracker() {\n"
        "        this.marker = 5;\n"
        "    }\n"
        "    public ~Tracker() {\n"
        "        int32 unused = this.marker;\n"  // exercises this.field
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Tracker t = new Tracker();\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// Constructor + destructor pair owns a paired runtime resource. This
// is the canonical RAII pattern — Lock/Mutex/file handles all follow
// it. The destructor calling Cajeta.lockDestroy is the real test that
// `~ClassName()` integrates with intrinsic dispatch and field access.
TEST(DestructorTests, pairedCtorDtorOwnsLockHandle) {
    auto src =
        "package test;\n"
        "public class MyLock {\n"
        "    private pointer handle;\n"
        "    public MyLock() { this.handle = Cajeta.lockNew(); }\n"
        "    public ~MyLock() { Cajeta.lockDestroy(this.handle); }\n"
        "    public int32 tryAcquire() {\n"
        "        return Cajeta.lockTryAcquire(this.handle);\n"
        "    }\n"
        "    public void releaseLock() {\n"
        "        Cajeta.lockRelease(this.handle);\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        MyLock m = new MyLock();\n"
        "        int32 got = m.tryAcquire();\n"
        "        if (got == 1) m.releaseLock();\n"
        "        return got;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// Name-mismatch: `~Foo()` declared inside `class Bar` should reject at
// compile time. The check makes typos visible early instead of
// silently producing a method that never gets called as a destructor.
TEST(DestructorTests, destructorNameMustMatchClass) {
    auto src =
        "package test;\n"
        "public class Bar {\n"
        "    public ~Foo() { }\n"  // wrong name
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected destructor name mismatch to throw";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_TYPE");
        EXPECT_NE(e.getMessage().find("~Foo"), std::string::npos);
        EXPECT_NE(e.getMessage().find("Bar"), std::string::npos);
    }
}
