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
TEST(PlaceholderOwnedFieldTests, ownedInterfaceReturnSurvivesContainerField) {
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
        "public final class Times implements Face {\n"
        "    public int32 k;\n"
        "    public Times(int32 kk) { this.k = kk; }\n"
        "    public int32 poke(int32 v) { return v * this.k; }\n"
        "}\n"
        "public final class Reg {\n"
        "    public ArrayList<Face> faces;\n"
        "    public Reg() {\n"
        "        this.faces #= heap ArrayList<Face>();\n"
        "    }\n"
        // the `#Interface` return under test — two arms, so the concrete
        // type is not statically pinned at the call site.
        "    public #Face make(int32 which, int32 k) {\n"
        "        if (which == 0) { return heap Plus(k); }\n"
        "        return heap Times(k);\n"
        "    }\n"
        "    public void enroll(int32 which, int32 k) {\n"
        "        Face f = this.make(which, k);\n"
        "        this.faces.add(#f);\n"
        "    }\n"
        // read back in a LATER call — the frame that produced the value is
        // long gone by here.
        "    public int32 pokeAll(int32 v) {\n"
        "        int32 t = 0;\n"
        "        int32 i = 0;\n"
        "        while (i < (int32) this.faces.count()) {\n"
        "            Face g = this.faces.get(i);\n"
        "            t = t + g.poke(v);\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return t;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Reg r = heap Reg();\n"
        "        r.enroll(0, 10);\n"       // Plus(10):  poke(5) = 15
        "        r.enroll(1, 3);\n"        // Times(3):  poke(5) = 15
        "        return r.pokeAll(5);\n"   // 30
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 30);
}

// 4.3 — the discriminating control. A `#<BaseClass>` return of a derived
// instance shares "return type wider than the allocated type" but is a THIN
// pointer, not a fat body. If this passes while the interface version faults,
// the defect is fat-value handling; if both fault, it is return-slot
// lifetime. Either way the answer names where the fix belongs.
TEST(PlaceholderOwnedFieldTests, ownedBaseClassReturnSurvivesContainerField) {
    std::string src =
        "package test;\n"
        "import cajeta.collection.ArrayList;\n"
        "public class Base {\n"
        "    public int32 k;\n"
        "    public Base(int32 kk) { this.k = kk; }\n"
        "    public int32 poke(int32 v) { return v + this.k; }\n"
        "}\n"
        "public final class Derived extends Base {\n"
        "    public Derived(int32 kk) { super(kk); }\n"
        "    public int32 poke(int32 v) { return v * this.k; }\n"
        "}\n"
        "public final class Reg2 {\n"
        "    public ArrayList<Base> items;\n"
        "    public Reg2() {\n"
        "        this.items #= heap ArrayList<Base>();\n"
        "    }\n"
        "    public #Base make(int32 which, int32 k) {\n"
        "        if (which == 0) { return heap Base(k); }\n"
        "        return heap Derived(k);\n"
        "    }\n"
        "    public void enroll(int32 which, int32 k) {\n"
        "        Base b = this.make(which, k);\n"
        "        this.items.add(#b);\n"
        "    }\n"
        "    public int32 pokeAll(int32 v) {\n"
        "        int32 t = 0;\n"
        "        int32 i = 0;\n"
        "        while (i < (int32) this.items.count()) {\n"
        "            Base g = this.items.get(i);\n"
        "            t = t + g.poke(v);\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return t;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Reg2 r = heap Reg2();\n"
        "        r.enroll(0, 10);\n"       // Base(10):    poke(5) = 15
        "        r.enroll(1, 3);\n"        // Derived(3):  poke(5) = 15
        "        return r.pokeAll(5);\n"   // 30
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 30);
}

// The workaround the spec records — build inline at the storing site, no
// `#Interface` return anywhere — must keep working. It is what cajeta-ml
// ships today, so a regression here breaks a released library.
TEST(PlaceholderOwnedFieldTests, inlineInterfaceBuildIsTheWorkingBaseline) {
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
        "public final class Reg3 {\n"
        "    public ArrayList<Face> faces;\n"
        "    public Reg3() {\n"
        "        this.faces #= heap ArrayList<Face>();\n"
        "        Face f = heap Plus(10);\n"      // built AT the storing site
        "        this.faces.add(#f);\n"
        "    }\n"
        "    public int32 poke(int32 v) { return this.faces.get(0).poke(v); }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Reg3 r = heap Reg3();\n"
        "        return r.poke(5);\n"            // 15
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 15);
}

} // namespace
