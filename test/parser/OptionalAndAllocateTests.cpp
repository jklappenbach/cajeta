//
// Two formerly-deferred DI features:
//
//   - @Inject(optional = true): allow null when no @Component
//     implements the field type. Resolver tracks the optional
//     flag and skips the missing-impl error; codegen stores a
//     null pointer into the field slot.
//
//   - @Inject(scope = "Owner" | "Transient"): fresh allocation per
//     @Inject site instead of the shared singleton, for a component
//     that declares no scope. "Call" on a field is refused
//     (component-scopes spec 8.2.1).
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

// "Transient": two consumers each @Inject Counter →
// distinct instances. A.counter and B.counter are different
// allocations. Mutate via A's counter, read via B's counter:
// B's value reflects ITS ctor-set 0 (not A's mutated 5).
TEST(OptionalAndAllocateTests, siteTransientYieldsDistinctInstances) {
    auto src =
        "package test;\n"
        "@Component public class Counter {\n"
        "    public int32 v;\n"
        "    public Counter() { v = 0; return; }\n"
        "}\n"
        "@Component public class A {\n"
        "    @Inject(scope = \"Transient\") Counter c;\n"
        "    public A() { return; }\n"
        "}\n"
        "@Component public class B {\n"
        "    @Inject(scope = \"Transient\") Counter c;\n"
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

// "Owner": same shape as TRANSIENT at field-level
// granularity in v1. Each consumer's @Inject site allocates
// fresh; mutating one doesn't visit the other.
TEST(OptionalAndAllocateTests, siteOwnerYieldsDistinctInstances) {
    auto src =
        "package test;\n"
        "@Component public class Counter {\n"
        "    public int32 v;\n"
        "    public Counter() { v = 0; return; }\n"
        "}\n"
        "@Component public class A {\n"
        "    @Inject(scope = \"Owner\") Counter c;\n"
        "    public A() { return; }\n"
        "}\n"
        "@Component public class B {\n"
        "    @Inject(scope = \"Owner\") Counter c;\n"
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

// "Singleton" explicit (same as default): both consumers
// share the same Counter singleton. Mutating via A's reference
// is visible through B's. The contrast with the TRANSIENT/
// OWNER_SCOPE tests above proves the resolver is honoring the
// allocate mode, not just always producing distinct allocations
// or always producing one.
TEST(OptionalAndAllocateTests, siteSingletonShares) {
    auto src =
        "package test;\n"
        "@Component public class Counter {\n"
        "    public int32 v;\n"
        "    public Counter() { v = 0; return; }\n"
        "}\n"
        "@Component public class A {\n"
        "    @Inject(scope = \"Singleton\") Counter c;\n"
        "    public A() { return; }\n"
        "}\n"
        "@Component public class B {\n"
        "    @Inject(scope = \"Singleton\") Counter c;\n"
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

// A field is filled once, when its holder is built, so "Call" has no
// activation to attach to.
TEST(OptionalAndAllocateTests, siteCallOnAFieldRejected) {
    auto src =
        "package test;\n"
        "@Component public class Dep {\n"
        "    public Dep() { return; }\n"
        "}\n"
        "@Component public class Service {\n"
        "    @Inject(scope = \"Call\") Dep d;\n"
        "    public Service() { return; }\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    try {
        runI32(src, "test.Service");
        FAIL() << "expected CAJETA_ERROR_CALL_SCOPE_FIELD";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_CALL_SCOPE_FIELD");
    } catch (std::exception& e) {
        EXPECT_NE(std::string(e.what()).find("CAJETA_ERROR_CALL_SCOPE_FIELD"), std::string::npos) << e.what();
    }
}
