// title-stores Unit 7, task 7.1.1 (spec §2.3 Phase 2) — the `= #v` deprecation
// pin. One spelling has to survive: `#=` is the assignment form, so `= #v` at an
// ASSIGNMENT site warns with the `use #=` fix-it.
//
// The whole point of the pin is the BOUNDARY. `#v` is not deprecated — it stays
// the ONLY spelling at call arguments, returns, and extraction reads, because
// those are not assignments. A warning that fired there would be wrong, and the
// mechanical respell in 7.2.1 would then "fix" correct code into nonsense. So the
// quiet cases below are load-bearing, not padding.
//
// Severity is WARNING (Phase 2): every program here still compiles and runs. The
// Phase-3 error flip is deliberately NOT pinned here — it is parked until this
// plan closes and title-tracking Unit 8 is archived (plan 7.2.2).
//
// RED until 7.2.2 lands the warning.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

const char* kCellSrc =
    "package test;\n"
    "public class Cell {\n"
    "    public int32 n;\n"
    "    public Cell(int32 nn) { this.n = nn; }\n"
    "}\n";

int32_t runI32(const std::string& src, const char* entryClass = "test.D") {
    auto jit = CajetaJit::compile(src, entryClass);
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

// Did the last compile emit the `= #v` deprecation? Filter by code + severity so
// the assertion is immune to other advisories firing on the same fixture (the
// last-use and plain-store diagnostics both key off transfers too).
bool deprecationWarned() {
    for (auto& d : CajetaJit::lastDiagnostics()) {
        if (d.code == "CAJETA_WARN_DEPRECATED_TRANSFER_ASSIGN"
                && d.severity == "warning") {
            return true;
        }
    }
    return false;
}

// The fix-it has to name the replacement spelling — a deprecation that doesn't
// say what to write instead just makes the developer grep the spec.
bool deprecationMessageOffersSharpAssign() {
    for (auto& d : CajetaJit::lastDiagnostics()) {
        if (d.code == "CAJETA_WARN_DEPRECATED_TRANSFER_ASSIGN"
                && d.message.find("#=") != std::string::npos) {
            return true;
        }
    }
    return false;
}

// Unit 6's loud-plain-store diagnostic (spec §2.4) — deprecating a spelling
// must not reclassify it.
bool plainStoreWarnedAbout(const char* varName) {
    for (auto& d : CajetaJit::lastDiagnostics()) {
        if (d.code == "CAJETA_WARN_PLAIN_RETAIN_STORE"
                && d.severity == "warning"
                && d.message.find(std::string("`") + varName + "`")
                       != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

// 7.1.1a — local declaration-initializer: `T x = #v` is an assignment site.

// 7.1.1b — field store: `this.f = #v` is an assignment site.

// 7.1.1c — element slot: `arr[i] = #v` is an assignment site.

// 7.1.1d — call argument: NOT an assignment. `#v` is the only spelling here and
// must stay quiet.
TEST(TransferAssignDeprecationTests, callArgTransferStaysQuiet) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 take(#Cell v) { return v.n; }\n"
        "    public static int32 run() {\n"
        "        Cell c = heap Cell(3);\n"
        "        return take(#c);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 3);
    EXPECT_FALSE(deprecationWarned());
}

// 7.1.1e — return: NOT an assignment. Stays quiet.
TEST(TransferAssignDeprecationTests, returnTransferStaysQuiet) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static #Cell make() {\n"
        "        Cell c = heap Cell(4);\n"
        "        return #c;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Cell c = make();\n"
        "        return c.n;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 4);
    EXPECT_FALSE(deprecationWarned());
}

// 7.1.1f — the replacement spelling must not warn about itself. If `#=` tripped
// the deprecation, 7.2.1's respell would trade one warning for another.
TEST(TransferAssignDeprecationTests, sharpAssignStaysQuiet) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cell c = heap Cell(8);\n"
        "        Cell d #= c;\n"
        "        return d.n;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 8);
    EXPECT_FALSE(deprecationWarned());
}

// 7.1.1h — `= #v` is DEPRECATED, not demoted. It still moves the title, so it
// must not trip Unit 6's loud-plain-store warning (spec §2.4): the two
// diagnostics have to stay orthogonal, or the fix-it would tell the developer
// to write `#=` on a store that already transfers.
//
// Inherited from PlainStoreDiagnosticTests.sharpSpellingsQuiet (6.1.1c), whose
// `keepOld` half was the legacy spelling until 7.2.1 respelled it away. This is
// the last place the exclusion is exercised; it retires with the Phase 3 flip.
TEST(TransferAssignDeprecationTests, legacyTransferAssignIsNotAPlainStore) {
    std::string src = std::string(kCellSrc) +
        "public class Holder {\n"
        "    public Cell b;\n"
        "    public void keepOld(Cell u) { this.b = #u; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Holder h = heap Holder();\n"
        "        h.keepOld(#heap Cell(4));\n"
        "        return h.b.n;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 4);
    EXPECT_FALSE(plainStoreWarnedAbout("u"));  // it transfers — not a plain store
    EXPECT_TRUE(deprecationWarned());          // ...but it is on the way out
}

// 7.1.1g — a plain (non-transfer) assignment is untouched by this diagnostic.
