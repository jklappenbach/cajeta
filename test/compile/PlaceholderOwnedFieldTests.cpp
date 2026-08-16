//
// cajeta-ml-v2 U7 (7.2.0) TDD — two registered defects around placeholders
// and owned interface values, both found building dev.cajeta.ml 0.3.0-dev
// on the v0.12.1 toolchain (specs/placeholder-owned-field-spec.md,
// specs/null-owned-interface-arg-spec.md). The multi-source JIT harness
// gives DETERMINISTIC parse order (std::map), unlike the AOT driver's
// readdir order — the wrapper is named to sort FIRST so it walks while its
// inner class and interface are still archive placeholders.
//
// Pre-fix behavior, pinned 2026-07-31 on the v0.12.1 tree:
//  - ownedFieldOfLaterParsedClass: LLJIT "Symbols not found:
//    [ __cajeta_test_ZInner_drop ]" — the wrapper's ctor emits a reference
//    to the placeholder class's DROP FUNCTION that never materializes
//    (the AOT flavor of the same shape calls that unresolved pointer and
//    corrupts the stack). Root-cause anchor for the fix.
//  - nullIntoOwnedInterfaceFormal: WAS SIGSEGV (fault 0x10 — the callee
//    copied the fat body from a raw null pointer). FIXED 2026-07-31: the
//    invokeMethod arg coercion now spills a ZEROED fat body for a null
//    constant bound to an interface formal — the pin is live.
//  - ownedFieldOfLaterParsedClass: FIXED 2026-07-31, two layers —
//    (1) the harness now runs the buildJit backfill/pin pair before its
//    linkModules merge (drop thunks survive lazy linking); (2) the real
//    compiler defect: synthesizeInterfaceVTables pushed RAW Function*
//    entries into the vtable initializer — on the deferred re-synthesis
//    pass (interface was a placeholder at declaration walk) those live in
//    a DIFFERENT llvm::Module, and the dangling cross-module constant
//    became the flaky verifier-print crash here and the AOT wild jump.
//    Entries now route through ensureFunctionInModule. Both pins live.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <map>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// placeholder-owned-field: a class whose OWNED field types a user class
// that is still a placeholder at the declaring class's walk, heap-built in
// a ctor. AWrap sorts before both the interface and the inner class.
TEST(PlaceholderOwnedFieldTests, ownedFieldOfLaterParsedClass) {
    std::map<std::string, std::string> sources;
    sources["test.AWrap"] =
        "package test;\n"
        "public final class AWrap implements ZFace {\n"
        "    private ZInner inner;\n"
        "    public AWrap(float64 a, boolean b, float64 c, int64 d) {\n"
        "        this.inner #= heap ZInner(a, 1.0, b, c, d);\n"
        "        return;\n"
        "    }\n"
        "    public AWrap(float64 a) {\n"
        "        this.inner #= heap ZInner(a, 1.0);\n"
        "        return;\n"
        "    }\n"
        "    public float64 poke(float64 v) { return this.inner.poke(v); }\n"
        "}\n";
    sources["test.ZFace"] =
        "package test;\n"
        "public interface ZFace {\n"
        "    float64 poke(float64 v);\n"
        "}\n";
    sources["test.ZInner"] =
        "package test;\n"
        "public final class ZInner implements ZFace {\n"
        "    private float64 a;\n"
        "    public ZInner(float64 a, float64 r, boolean b, float64 c, int64 d) {\n"
        "        this.a = a;\n"
        "        return;\n"
        "    }\n"
        "    public ZInner(float64 a, float64 r) {\n"
        "        this.a = a;\n"
        "        return;\n"
        "    }\n"
        "    public float64 poke(float64 v) { return this.a + v; }\n"
        "}\n";
    sources["test.ZMain"] =
        "package test;\n"
        "public final class ZMain {\n"
        "    public static int32 run() {\n"
        "        AWrap w = heap AWrap(0.5);\n"
        "        if (w.poke(1.0) == 1.5) { return 42; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(sources, "test.ZMain");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 42);
}

// null-owned-interface-arg: null passed to a `#`-interface formal and
// stored with `#=` must be a legal empty value (a zeroed 24-byte fat body;
// transfer and drop no-ops), never a fat-pointer half-dereference. The pin
// is crash-freedom through construct + store + drop; observing emptiness
// via `f == null` is separate interface-compare semantics, deliberately
// NOT pinned here.
TEST(PlaceholderOwnedFieldTests, nullIntoOwnedInterfaceFormal) {
    std::string src =
        "package test;\n"
        "public interface Face {\n"
        "    float64 poke(float64 v);\n"
        "}\n"
        "public final class Holder {\n"
        "    private Face f;\n"
        "    public Holder(#Face f) {\n"
        "        this.f #= f;\n"
        "        return;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Holder h = heap Holder(null);\n"
        "        return 42;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 42);
}

// ---- fat-aware interface `== null` ----------------------------------------
//
// The follow-up the test above explicitly does not pin: `nullIntoOwnedInterfaceFormal`
// proves a null interface value CONSTRUCTS, STORES and DROPS without faulting,
// and says observing its emptiness via `==` is separate semantics left open.
// It is still open — an interface is a 24-byte fat body, and `==` against
// `null` has to compare the body's data pointer rather than the address of
// the body, so a null interface currently is not observable at all.
//
// Red-first: this is expected to fail until the comparison is made fat-aware.
// It is written as the contract, not as the bug, so it flips to green on the
// fix instead of needing a rewrite.
TEST(PlaceholderOwnedFieldTests, nullInterfaceIsObservableViaEquals) {
    std::string src =
        "package test;\n"
        "public interface Face {\n"
        "    int32 poke(int32 v);\n"
        "}\n"
        "public final class Plus implements Face {\n"
        "    public int32 poke(int32 v) { return v + 1; }\n"
        "}\n"
        "public final class Holder2 {\n"
        "    public Face f;\n"
        "    public Holder2(#Face f) { this.f #= f; }\n"
        "    public boolean empty() { return this.f == null; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Holder2 a = heap Holder2(null);\n"
        "        Holder2 b = heap Holder2(heap Plus());\n"
        "        int32 t = 0;\n"
        "        if (a.empty()) { t = t + 1; }\n"        // null IS empty
        "        if (!b.empty()) { t = t + 10; }\n"      // a real value is NOT
        "        return t;\n"                            // 11
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 11);
}

// ---- owned-interface-return-fault (spec 4.2/4.3) --------------------------
//
// A `#<Interface>`-returning factory hands back a FAT (24-byte) body. The
// reported fault is that the value goes bad once the caller parks it in a
// container FIELD and reads it back in a later call — SIGSEGV on the first
// dispatch, consistent with a pointer into a dead return slot.
//
// The spec ruled out every neighbouring suspect in isolation (the container,
// a variable index, storing a `heap Concrete(...)` through an interface,
// `add`/`set` ownership, the whole surrounding algorithm inline). The one
// remaining difference is the `#Interface` return, so these reduce exactly
// that: factory -> container field -> retrieve in another method -> dispatch.

// 4.2 — the reported shape, reduced. The store and the read happen in
// different frames, which is what the cajeta-ml repro did.

// --- bisection probes: which STEP of the reported shape faults? -----------
// Each adds one element of §1's repro. Named PROBE_ so they read as
// diagnosis, not contract.

// P1: `#Interface` return -> local -> dispatch, same method. No container,
// no field.
TEST(PlaceholderOwnedFieldTests, PROBE_ifaceReturnToLocalDispatch) {
    std::string src =
        "package test;\n"
        "public interface Face {\n"
        "    int32 poke(int32 v);\n"
        "}\n"
        "public final class Plus implements Face {\n"
        "    public int32 k;\n"
        "    public Plus(int32 kk) { this.k = kk; }\n"
        "    public int32 poke(int32 v) { return v + this.k; }\n"
        "}\n"
        "public final class D {\n"
        "    public static #Face make(int32 k) { return heap Plus(k); }\n"
        "    public static int32 run() {\n"
        "        Face f #= D.make(10);\n"
        "        return f.poke(5);\n"          // 15
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 15);
}

// P2: same, but the value goes through a LOCAL container first.
TEST(PlaceholderOwnedFieldTests, PROBE_ifaceReturnToLocalContainer) {
    std::string src =
        "package test;\n"
        "import cajeta.collection.ArrayList;\n"
        "public interface Face {\n"
        "    int32 poke(int32 v);\n"
        "}\n"
        "public final class Plus implements Face {\n"
        "    public int32 k;\n"
        "    public Plus(int32 kk) { this.k = kk; }\n"
        "    public int32 poke(int32 v) { return v + this.k; }\n"
        "}\n"
        "public final class D {\n"
        "    public static #Face make(int32 k) { return heap Plus(k); }\n"
        "    public static int32 run() {\n"
        "        ArrayList<Face> xs = heap ArrayList<Face>();\n"
        "        Face f #= D.make(10);\n"
        "        xs.add(#f);\n"
        "        return xs.get(0).poke(5);\n"   // 15
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 15);
}

// P3: container FIELD, but stored and read in the SAME method — isolates the
// field round-trip from the cross-frame part.
TEST(PlaceholderOwnedFieldTests, PROBE_ifaceReturnToFieldContainerSameFrame) {
    std::string src =
        "package test;\n"
        "import cajeta.collection.ArrayList;\n"
        "public interface Face {\n"
        "    int32 poke(int32 v);\n"
        "}\n"
        "public final class Plus implements Face {\n"
        "    public int32 k;\n"
        "    public Plus(int32 kk) { this.k = kk; }\n"
        "    public int32 poke(int32 v) { return v + this.k; }\n"
        "}\n"
        "public final class RegP {\n"
        "    public ArrayList<Face> faces;\n"
        "    public RegP() { this.faces #= heap ArrayList<Face>(); }\n"
        "    public #Face make(int32 k) { return heap Plus(k); }\n"
        "    public int32 both(int32 v) {\n"
        "        Face f #= this.make(10);\n"
        "        this.faces.add(#f);\n"
        "        return this.faces.get(0).poke(v);\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        RegP r = heap RegP();\n"
        "        return r.both(5);\n"           // 15
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 15);
}

// P4: NO `#Interface` return anywhere, but the store and read are in
// different frames — the other half of the difference the spec isolated.
TEST(PlaceholderOwnedFieldTests, PROBE_inlineBuildCrossFrame) {
    std::string src =
        "package test;\n"
        "import cajeta.collection.ArrayList;\n"
        "public interface Face {\n"
        "    int32 poke(int32 v);\n"
        "}\n"
        "public final class Plus implements Face {\n"
        "    public int32 k;\n"
        "    public Plus(int32 kk) { this.k = kk; }\n"
        "    public int32 poke(int32 v) { return v + this.k; }\n"
        "}\n"
        "public final class RegQ {\n"
        "    public ArrayList<Face> faces;\n"
        "    public RegQ() { this.faces #= heap ArrayList<Face>(); }\n"
        "    public void enroll(int32 k) {\n"
        "        Face f = heap Plus(k);\n"      // built inline, no `#Face` return
        "        this.faces.add(#f);\n"
        "    }\n"
        "    public int32 poke(int32 v) { return this.faces.get(0).poke(v); }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        RegQ r = heap RegQ();\n"
        "        r.enroll(10);\n"
        "        return r.poke(5);\n"           // 15
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 15);
}

// 4.3 — the discriminating control. A `#<BaseClass>` return of a derived
// instance shares "return type wider than the allocated type" but is a THIN
// pointer, not a fat body. If this passes while the interface version faults,
// the defect is fat-value handling; if both fault, it is return-slot
// lifetime. Either way the answer names where the fix belongs.

// The workaround the spec records — build inline at the storing site, no
// `#Interface` return anywhere — must keep working. It is what cajeta-ml
// ships today, so a regression here breaks a released library.

} // namespace
