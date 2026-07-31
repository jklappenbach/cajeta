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
//  - ownedFieldOfLaterParsedClass stays DISABLED_: with the harness
//    backfill fix it is FLAKY (alternating green / verifier-print crash on
//    identical input) — nondeterministic codegen emits a sometimes-
//    malformed constant; same character as the AOT wild jump. The
//    remaining 7.2.0 hunt.
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
TEST(PlaceholderOwnedFieldTests, DISABLED_ownedFieldOfLaterParsedClass) {
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

} // namespace
