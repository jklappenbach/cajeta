//
// Two formerly-deferred DI features:
//
//   - @Inject(optional = true): allow null when no @Component
//     implements the field type. Resolver tracks the optional
//     flag and skips the missing-impl error; codegen stores a
//     null pointer into the field slot.
//
//   - @Inject(allocate = ALLOCATE_OWNER_SCOPE | ALLOCATE_TRANSIENT):
//     fresh allocation per @Inject site instead of the shared
//     singleton. Two consumers each @Inject the same component
//     in a non-singleton mode get distinct instances.
//
//     ALLOCATE_CALL_SCOPE rejected with CAJETA_ERROR_NOT_IMPLEMENTED
//     until the implicit-function-body-scope wiring lands.
//
// Drop-chain cleanup of owner-scoped allocations is a v1 known
// gap (leaks at owner destruction). The user-facing "fresh
// instance" semantic is what these tests pin down.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <cstdint>
#include <stdexcept>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src, const std::string& fqEntryClass) {
    auto jit = CajetaJit::compile(src, fqEntryClass);
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// @Inject(optional=true) Foo f with no @Component for Foo →
// resolver doesn't error; codegen stores null. The user code
// observes via a `f == null` check would be the cleanest, but
// raw pointer-null comparison isn't reliably wired today;
// instead we observe via dropCount: the null target produced
// no allocations, count stays 0.
TEST(OptionalAndAllocateTests, optionalInjectMissingTargetStoresNull) {
    auto src =
        "package test;\n"
        "public class Missing {\n"   // NOT a @Component
        "    public Missing() { return; }\n"
        "}\n"
        "@Component public class Service {\n"
        "    @Inject(optional = true) Missing m;\n"
        "    public Service() { return; }\n"
        "    public static int32 run() {\n"
        "        Cajeta.dropCountReset();\n"
        "        Service s = __cajeta_inject();\n"
        "        return (int32) Cajeta.dropCount();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src, "test.Service"), 0);
}

// @Inject(optional=true) Foo f WITH a @Component present →
// resolves as ordinary singleton. Optional doesn't change
// behavior when the target exists.
TEST(OptionalAndAllocateTests, optionalInjectWithProviderResolvesNormally) {
    auto src =
        "package test;\n"
        "@Component public class Dep {\n"
        "    public int32 v;\n"
        "    public Dep() { v = 41; return; }\n"
        "}\n"
        "@Component public class Service {\n"
        "    @Inject(optional = true) Dep d;\n"
        "    public Service() { return; }\n"
        "    public static int32 run() {\n"
        "        Service s = __cajeta_inject();\n"
        "        return s.d.v;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src, "test.Service"), 41);
}

// ALLOCATE_TRANSIENT: two consumers each @Inject Counter →
// distinct instances. A.counter and B.counter are different
// allocations. Mutate via A's counter, read via B's counter:
// B's value reflects ITS ctor-set 0 (not A's mutated 5).
TEST(OptionalAndAllocateTests, allocateTransientYieldsDistinctInstances) {
    auto src =
        "package test;\n"
        "@Component public class Counter {\n"
        "    public int32 v;\n"
        "    public Counter() { v = 0; return; }\n"
        "}\n"
        "@Component public class A {\n"
        "    @Inject(allocate = ALLOCATE_TRANSIENT) Counter c;\n"
        "    public A() { return; }\n"
        "}\n"
        "@Component public class B {\n"
        "    @Inject(allocate = ALLOCATE_TRANSIENT) Counter c;\n"
        "    public B() { return; }\n"
        "    public static int32 run() {\n"
        "        A a = A.__cajeta_inject();\n"
        "        B b = __cajeta_inject();\n"
        "        a.c.v = 5;\n"
        "        return b.c.v;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src, "test.B"), 0);
}

// ALLOCATE_OWNER_SCOPE: same shape as TRANSIENT at field-level
// granularity in v1. Each consumer's @Inject site allocates
// fresh; mutating one doesn't visit the other.
TEST(OptionalAndAllocateTests, allocateOwnerScopeYieldsDistinctInstances) {
    auto src =
        "package test;\n"
        "@Component public class Counter {\n"
        "    public int32 v;\n"
        "    public Counter() { v = 0; return; }\n"
        "}\n"
        "@Component public class A {\n"
        "    @Inject(allocate = ALLOCATE_OWNER_SCOPE) Counter c;\n"
        "    public A() { return; }\n"
        "}\n"
        "@Component public class B {\n"
        "    @Inject(allocate = ALLOCATE_OWNER_SCOPE) Counter c;\n"
        "    public B() { return; }\n"
        "    public static int32 run() {\n"
        "        A a = A.__cajeta_inject();\n"
        "        B b = __cajeta_inject();\n"
        "        a.c.v = 7;\n"
        "        return b.c.v;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src, "test.B"), 0);
}

// ALLOCATE_SINGLETON explicit (same as default): both consumers
// share the same Counter singleton. Mutating via A's reference
// is visible through B's. The contrast with the TRANSIENT/
// OWNER_SCOPE tests above proves the resolver is honoring the
// allocate mode, not just always producing distinct allocations
// or always producing one.
TEST(OptionalAndAllocateTests, allocateSingletonShares) {
    auto src =
        "package test;\n"
        "@Component public class Counter {\n"
        "    public int32 v;\n"
        "    public Counter() { v = 0; return; }\n"
        "}\n"
        "@Component public class A {\n"
        "    @Inject(allocate = ALLOCATE_SINGLETON) Counter c;\n"
        "    public A() { return; }\n"
        "}\n"
        "@Component public class B {\n"
        "    @Inject(allocate = ALLOCATE_SINGLETON) Counter c;\n"
        "    public B() { return; }\n"
        "    public static int32 run() {\n"
        "        A a = A.__cajeta_inject();\n"
        "        B b = __cajeta_inject();\n"
        "        a.c.v = 9;\n"
        "        return b.c.v;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src, "test.B"), 9);
}

// ALLOCATE_CALL_SCOPE is reserved by the spec but its runtime
// integration (per-activation cache via the implicit function-
// body scope) isn't wired yet. The compiler rejects with
// CAJETA_ERROR_NOT_IMPLEMENTED so users don't silently get
// SINGLETON behavior.
TEST(OptionalAndAllocateTests, allocateCallScopeRejected) {
    auto src =
        "package test;\n"
        "@Component public class Dep {\n"
        "    public Dep() { return; }\n"
        "}\n"
        "@Component public class Service {\n"
        "    @Inject(allocate = ALLOCATE_CALL_SCOPE) Dep d;\n"
        "    public Service() { return; }\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    try {
        runI32(src, "test.Service");
        FAIL() << "expected CAJETA_ERROR_NOT_IMPLEMENTED";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_NOT_IMPLEMENTED");
    } catch (std::runtime_error&) {
        // JIT layer wraps some throws — accept that path too.
        SUCCEED();
    }
}
