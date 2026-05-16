//
// Session 11 — through-interface dispatch + method restrictions.
//
// Sits on top of S9.5 (fat-pointer foundation) + S10 (struct/class →
// interface assignment + ownership). Confirms the dispatch path is
// uniform across struct-rooted and class-rooted receivers, pins the
// dyn-dispatch restrictions (same-concrete-type return rejected at
// call site, method-level generics rejected at interface declaration),
// and verifies mixed-implementer dispatch lands on the correct
// implementer.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// ---------------------------------------------------------------------
// S11.1 — through-interface dispatch lands on the struct implementer.
// Wired by S9.5.6 + exercised by S10's structToInterfaceDispatches.
// Re-pinned here with a richer body (mutating field access from inside
// the dispatched method) to prove the data-pointer swap at the call
// site routes `this` to the struct's body, not the interface fat
// pointer's body.
// ---------------------------------------------------------------------

TEST(FatPointerDispatchTests, dispatchToStructImpl) {
    auto src =
        "package test;\n"
        "public interface Sized { public int32 size(); }\n"
        "public struct Box implements Sized {\n"
        "    int32 width;\n"
        "    int32 height;\n"
        "    public int32 size() { return this.width * this.height; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Box b = Box { width: 4, height: 5 };\n"
        "        Sized s = b;\n"
        "        return s.size();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 20);
}

// ---------------------------------------------------------------------
// S11.1 — through-interface dispatch lands on the class implementer.
// Wired by S9.5.6 + S10's classToInterfaceBorrowedDispatches. Re-pinned
// with field-access from inside the method to confirm the dispatched
// `this` is the heap class instance (not the interface body).
// ---------------------------------------------------------------------

TEST(FatPointerDispatchTests, dispatchToClassImpl) {
    auto src =
        "package test;\n"
        "public interface Sized { public int32 size(); }\n"
        "public class Counter implements Sized {\n"
        "    int32 n;\n"
        "    public Counter() { this.n = 17; }\n"
        "    public int32 size() { return this.n; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Counter c = new Counter();\n"
        "        Sized s = c;\n"
        "        return s.size();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 17);
}

// ---------------------------------------------------------------------
// S11.1 — "polymorphic array iteration" via sequenced interface locals.
//
// True interface-array iteration (`Sized[] arr; for (s in arr) ...`)
// depends on the class-array element-layout gap called out in S7.5 +
// S10.5 — arrays of class/interface bodies need a design call on
// inline-vs-pointer element storage before through-array dispatch can
// land cleanly. The substitute used here is fixed-shape sequenced
// dispatch: three distinct interface locals all of type Sized, each
// rooted in a different concrete implementer, dispatched in sequence.
// Same dispatch path, same correctness pin — without the storage
// shape that's blocked downstream.
// ---------------------------------------------------------------------

TEST(FatPointerDispatchTests, dispatchInPolymorphicSequence) {
    auto src =
        "package test;\n"
        "public interface Sized { public int32 size(); }\n"
        "public struct Tiny implements Sized {\n"
        "    public int32 size() { return 1; }\n"
        "}\n"
        "public struct Medium implements Sized {\n"
        "    public int32 size() { return 10; }\n"
        "}\n"
        "public struct Large implements Sized {\n"
        "    public int32 size() { return 100; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Tiny t = Tiny { };\n"
        "        Medium m = Medium { };\n"
        "        Large l = Large { };\n"
        "        Sized s1 = t;\n"
        "        Sized s2 = m;\n"
        "        Sized s3 = l;\n"
        "        return s1.size() + s2.size() + s3.size();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 111);
}

// ---------------------------------------------------------------------
// S11.2 — reject same-concrete-type return through dyn dispatch.
//
// Foo implements CloneFoo which declares `Foo clone()`. Direct calls on
// the concrete Foo (`f.clone()`) keep working; calls through the
// interface (`c.clone()` where c: CloneFoo) are rejected at codegen
// because the fat pointer doesn't carry the underlying size needed for
// an sret slot. CAJETA_ERROR_DYN_DISPATCH_OWN_TYPE_RETURN points users
// at the direct-call workaround.
// ---------------------------------------------------------------------

TEST(FatPointerDispatchTests, sameConcreteTypeReturnThroughInterfaceRejected) {
    auto src =
        "package test;\n"
        "public interface CloneFoo { public Foo clone(); }\n"
        "public struct Foo implements CloneFoo {\n"
        "    int32 v;\n"
        "    public Foo clone() { return Foo { v: this.v }; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Foo f = Foo { v: 3 };\n"
        "        CloneFoo c = f;\n"
        "        Foo other = c.clone();\n"  // ← rejected
        "        return other.v;\n"
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.S");
        FAIL() << "expected CAJETA_ERROR_DYN_DISPATCH_OWN_TYPE_RETURN";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_DYN_DISPATCH_OWN_TYPE_RETURN");
    }
}

// ---------------------------------------------------------------------
// S11.2 positive companion — the same method called directly on the
// concrete struct type still works. The restriction is dyn-dispatch-
// only; direct calls bypass invokeMethod's interface branch entirely
// via the `isAggregate` skip and stay monomorphized.
// ---------------------------------------------------------------------

TEST(FatPointerDispatchTests, directSameConcreteTypeReturnOnConcreteStructStillWorks) {
    auto src =
        "package test;\n"
        "public interface CloneFoo { public Foo clone(); }\n"
        "public struct Foo implements CloneFoo {\n"
        "    int32 v;\n"
        "    public Foo clone() { return Foo { v: this.v + 1 }; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Foo f = Foo { v: 41 };\n"
        "        Foo g = f.clone();\n"  // direct call — fine
        "        return g.v;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// ---------------------------------------------------------------------
// S11.5 — method called through interface value matches direct-call
// result. Same receiver, same args, both paths produce the same value.
// Pins the dispatch path lands on the same compiled function the
// direct call uses (the per-(impl, iface) vtable's slot holds the
// implementer's getLlvmFunction(), the same fn-pointer direct calls
// resolve to statically — see synthesizeInterfaceVTables).
// ---------------------------------------------------------------------

TEST(FatPointerDispatchTests, throughInterfaceMatchesDirectCall) {
    auto src =
        "package test;\n"
        "public interface Sized { public int32 size(); }\n"
        "public struct Box implements Sized {\n"
        "    int32 width;\n"
        "    int32 height;\n"
        "    public int32 size() { return this.width * this.height; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Box b = Box { width: 6, height: 7 };\n"
        "        int32 direct = b.size();\n"
        "        Sized s = b;\n"
        "        int32 viaIface = s.size();\n"
        "        if (direct == viaIface) { return direct; }\n"
        "        return -1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// ---------------------------------------------------------------------
// S11.5 — vtable indexing correctness. Three methods on the same
// interface, in declaration order. The dispatch path computes the
// method index by walking the interface's method list; an off-by-one
// or stale-cache bug would cross-wire the slots and produce wrong
// (but consistent) results per method. Each method here has a
// distinctive return value, so any cross-wiring is visible in the
// sum.
// ---------------------------------------------------------------------

TEST(FatPointerDispatchTests, vtableMethodIndexingCorrect) {
    auto src =
        "package test;\n"
        "public interface Triple {\n"
        "    public int32 first();\n"
        "    public int32 second();\n"
        "    public int32 third();\n"
        "}\n"
        "public struct Impl implements Triple {\n"
        "    public int32 first() { return 1; }\n"
        "    public int32 second() { return 20; }\n"
        "    public int32 third() { return 300; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Impl i = Impl { };\n"
        "        Triple t = i;\n"
        "        return t.first() + t.second() + t.third();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 321);
}

// ---------------------------------------------------------------------
// S11.4 — mixed class + struct implementers behind the same interface
// dispatched in the same scope. The call shape is uniform — both
// receiver kinds go through the same load-vtable-from-word-1 / GEP-
// to-method-index path. Drop chain dispatches via kind tag through
// __cajeta_iface_drop at scope exit; the test simply asserts both
// returns are correct.
//
// The companion "polymorphic Vec" shape (single array holding mixed
// kinds) sits behind the same S7.5 / S10.5 class-array element layout
// gap noted in dispatchInPolymorphicSequence; this sequenced shape
// gets the same correctness pin without the array dependency.
// ---------------------------------------------------------------------

TEST(FatPointerDispatchTests, mixedClassStructDispatchesUniformly) {
    auto src =
        "package test;\n"
        "public interface Sized { public int32 size(); }\n"
        "public struct StructImpl implements Sized {\n"
        "    public int32 size() { return 7; }\n"
        "}\n"
        "public class ClassImpl implements Sized {\n"
        "    public int32 size() { return 13; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        StructImpl si = StructImpl { };\n"
        "        ClassImpl ci = new ClassImpl();\n"
        "        Sized s1 = si;\n"
        "        Sized s2 = ci;\n"
        "        return s1.size() + s2.size();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 20);
}
