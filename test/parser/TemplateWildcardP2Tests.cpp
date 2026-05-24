// Step 2 of template wildcards (`<?>`). The drop-chain ABI.
// Verifies wildcard-typed locals/fields compile, run, and drop
// correctly via __cajeta_class_virtual_drop dispatch — Cajeta's
// existing virtual-drop machinery already covers the runtime side,
// so Step 2 reduces to making the wildcard proxy class (Step 1) slot
// into the same paths plain class types use.
//
// See cajeta-docs/TemplateWildcard.md and todo.md.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/type/CajetaType.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// Mirrors the RAII guard from the P1 test file. Tests that exercise
// the gated parser path or the gated assignability flip the flag for
// the duration of the test and restore env-var-only behavior after.
struct WildcardsOn {
    WildcardsOn() {
        cajeta::CajetaType::setWildcardsEnabledForTest(true);
    }
    ~WildcardsOn() {
        cajeta::CajetaType::clearWildcardsTestOverride();
    }
};

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// A wildcard local initialized from a concrete heap instance must
// compile, run, and exit cleanly. Clean exit proves the drop chain
// dispatched correctly (live-set Strict mode would flag a leaked
// instance or double-free).
TEST(TemplateWildcardP2Tests, wildcardLocalRunsAndDropsCleanly) {
    WildcardsOn flag;
    auto src = std::string(
        "package test;\n"
        "public class Box<T> {\n"
        "    T value;\n"
        "    public Box(T v) { this.value = v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box<?> bw = heap Box<int32>(42);\n"
        "        return 7;\n"
        "    }\n"
        "}\n");
    EXPECT_EQ(runI32(src), 7);
}

// The wildcard local's drop must dispatch via vtable to the DYNAMIC
// type's destructor — not the proxy. Pattern adapted from
// ClassDropTests.userDropMethodIsInvoked: a ~Box() that allocates a
// small array bumps the drop counter as a side effect, so the
// observed count distinguishes "user destructor fired" from "only
// __cajeta_free ran" from "nothing fired at all".
TEST(TemplateWildcardP2Tests, wildcardDropDispatchesToUserDestructor) {
    WildcardsOn flag;
    auto src = std::string(
        "package test;\n"
        "public class Box<T> {\n"
        "    T value;\n"
        "    public Box(T v) { this.value = v; }\n"
        "    public void drop() {\n"
        "        int32[] junk = new int32[1];\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cajeta.dropCountReset();\n"
        "        Box<?> bw = heap Box<int32>(42);\n"
        "        return 0;\n"
        "    }\n"
        "    public static int64 read() {\n"
        "        return Cajeta.dropCount();\n"
        "    }\n"
        "}\n");
    auto jit = CajetaJit::compile(src, "test.D");
    jit->lookup<int32_t (*)()>("run")();
    // 1 — the wildcard local's drop entry (pop_run pre-increment).
    // 1 — the int32[] junk array drop fired at the destructor's
    //     scope exit; proves ~Box() body actually ran.
    EXPECT_EQ(jit->lookup<int64_t (*)()>("read")(), 2);
}

// Step 5a — method calls on wildcard locals dispatch via vtable to
// the dynamic type's implementation. Box<?> has a method get() (a
// fully-substituted Stream<?>-style method from Step 5a's full
// instantiation path); calling it on a wildcard local holding a
// Box<int32> returns the int32 the box was constructed with. This
// is the load-bearing capability that unblocks the parallel
// chain-walk in Step 5b.
TEST(TemplateWildcardP2Tests, wildcardLocalSupportsMethodDispatch) {
    WildcardsOn flag;
    auto src = std::string(
        "package test;\n"
        "public class Box<T> {\n"
        "    T value;\n"
        "    public Box(T v) { this.value = v; }\n"
        "    public int32 tag() { return 99; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box<?> bw = heap Box<int32>(42);\n"
        "        return bw.tag();\n"
        "    }\n"
        "}\n");
    EXPECT_EQ(runI32(src), 99);
}

// Baseline sanity check — same source minus the wildcard. Validates
// that the same surface (Box<T>.tag()) works against a concrete
// instantiation. If this passes and the wildcard version above
// segfaults, the gap is specifically in wildcard method dispatch.
TEST(TemplateWildcardP2Tests, concreteLocalMethodDispatchBaseline) {
    WildcardsOn flag;
    auto src = std::string(
        "package test;\n"
        "public class Box<T> {\n"
        "    T value;\n"
        "    public Box(T v) { this.value = v; }\n"
        "    public int32 tag() { return 99; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box<int32> bw = heap Box<int32>(42);\n"
        "        return bw.tag();\n"
        "    }\n"
        "}\n");
    EXPECT_EQ(runI32(src), 99);
}

// Two wildcard locals fire two drops, LIFO. Verifies the drop-entry
// per local registration loop walks wildcard-typed declarators just
// like concrete-typed ones.
TEST(TemplateWildcardP2Tests, multipleWildcardLocalsAllFire) {
    WildcardsOn flag;
    auto src = std::string(
        "package test;\n"
        "public class Box<T> {\n"
        "    T value;\n"
        "    public Box(T v) { this.value = v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cajeta.dropCountReset();\n"
        "        Box<?> a = heap Box<int32>(1);\n"
        "        Box<?> b = heap Box<int32>(2);\n"
        "        return 0;\n"
        "    }\n"
        "    public static int64 read() {\n"
        "        return Cajeta.dropCount();\n"
        "    }\n"
        "}\n");
    auto jit = CajetaJit::compile(src, "test.D");
    jit->lookup<int32_t (*)()>("run")();
    EXPECT_EQ(jit->lookup<int64_t (*)()>("read")(), 2);
}
