//
// #61: `heap T<int32>(literal, null)` against a ctor whose formal at the
// null position resolves to a primitive. `null` literal types as the
// `pointer` canonical (LiteralExpression.cpp:28), which has no subtype-
// distance to a primitive int32 formal — resolveMethod returns nullptr
// and CreatorRest silently leaves the slot zero-init'd. The user sees
// fields zeroed instead of either the value they passed (impossible —
// null isn't an int) or a diagnostic.
//
// The narrow fix shipped here is the symmetric null→ref relaxation:
// subtypeDistance now accepts a `pointer` arg against any non-primitive
// declared formal at high distance, gated to constructor lookup
// (`relaxNullToRef = isConstructor`). That fixes the stdlib pattern
// `heap Optional<SelectResult<T>>(false, null)` (the all-closed branch
// of `Tasks.selectReceive`), which pre-fix was *also* silently relying
// on zero-init coincidence.
//
// Surfacing the primitive-formal case loudly is deferred: stdlib
// stream-pipeline code emits `heap Optional<int32>(false, null)` where
// the value formal is a primitive int32, and currently depends on the
// silent zero-init behavior to mean "empty Optional<primitive>". A
// loud rejection would require the stdlib side to switch to typed-
// zero or restructure the Optional<primitive> empty path first.
//
// Coverage below:
//   - real second arg: works (baseline)
//   - null + class formal: works (the relaxation case — sym to the
//     selectReceive stdlib path)
//   - null + primitive formal: still silently zero-inits (the residual
//     #61 quirk; pinned here so a future fix flips the assertion)
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>

#include "cajeta/error/Exception.h"
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

TEST(HeapCtorPrimitiveNullProbe, primitiveCtorWithRealSecondArg) {
    auto src =
        "package test;\n"
        "public final class W<P> {\n"
        "    public int32 a;\n"
        "    public P b;\n"
        "    public W(int32 a, P b) { this.a = a; this.b = b; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        W<int32> w = heap W<int32>(99, 7);\n"
        "        return w.a;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 99);
}

// Residual #61 quirk RESOLVED LOUD: null in a primitive-resolved slot no
// longer silently zero-inits — ctor lookup rejects with
// NO_MATCHING_CONSTRUCTOR (silent-resolution-diagnostics; surfaced when the
// retired decl-# spelling came off this probe, title-tracking 7.1.3).
// Flipped to expect the throw per the original note.
TEST(HeapCtorPrimitiveNullProbe, primitiveCtorWithNullSecondArgSilentlyZeroInits) {
    auto src =
        "package test;\n"
        "public final class W<P> {\n"
        "    public int32 a;\n"
        "    public P b;\n"
        "    public W(int32 a, P b) { this.a = a; this.b = b; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        W<int32> w = heap W<int32>(99, null);\n"
        "        return w.a;\n"
        "    }\n"
        "}\n";
    try {
        runI32(src);
        ADD_FAILURE() << "expected NO_MATCHING_CONSTRUCTOR";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_NO_MATCHING_CONSTRUCTOR");
    }
}

// The symmetric class-T-with-null counter-case is exercised end-to-end
// by the stdlib path: `Tasks.selectReceive<T>` calls
// `heap Optional<SelectResult<T>>(false, null)` on the all-closed branch,
// and ChannelSelectTests.allClosedReturnsEmpty pins that the relaxation
// resolves the ctor (rather than silently dropping it as it did pre-fix).
// A self-contained probe of the same shape (`heap W<Box>(99, null)`)
// SIGSEGVs in the scope-exit drop chain — separate null-class-field
// drop-guard issue, out of scope for the relaxation fix.
