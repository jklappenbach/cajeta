// title-stores Unit 6 (spec §2.4) — the loud-plain-store diagnostic. A plain
// `=` retaining store (field or slot destination) whose RHS is a class-typed
// runtime owner (armed-capable formal, or a local carrying a runtime title
// flag) warns with the `#=`/clone fix-it. Exclusions stay quiet: `#=`/`#= v`
// spellings, primitives, Strings (plain store resolves to a copy),
// statically-borrowed locals, statically-owned locals (no runtime
// conditionality — the last-use advisory owns that family).
// RED until 6.2.1 lands.

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

// Did the last compile warn about this variable's plain retaining store?
// Filtering by code + name keeps the assertion immune to other advisories.
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

// 6.1.1a — field store of an armed-capable formal warns; the message carries
// the `#=` fix-it. The program still compiles and runs (WARNING severity).
TEST(PlainStoreDiagnosticTests, fieldStoreOfFormalWarns) {
    std::string src = std::string(kCellSrc) +
        "public class Holder {\n"
        "    public Cell held;\n"
        "    public void keep(Cell v) { this.held = v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Holder h = heap Holder();\n"
        "        Cell c = heap Cell(7);\n"
        "        h.keep(c);\n"
        "        return h.held.n;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
    EXPECT_TRUE(plainStoreWarnedAbout("v"));
    bool sawFixit = false;
    for (auto& d : CajetaJit::lastDiagnostics()) {
        if (d.code == "CAJETA_WARN_PLAIN_RETAIN_STORE"
                && d.message.find("#=") != std::string::npos) {
            sawFixit = true;
        }
    }
    EXPECT_TRUE(sawFixit);
}

// 6.1.1b — element-slot store of an armed-capable formal warns.
TEST(PlainStoreDiagnosticTests, slotStoreOfFormalWarns) {
    std::string src = std::string(kCellSrc) +
        "public class Box {\n"
        "    public Cell[] data;\n"
        "    public Box() { this.data = heap Cell[4]; }\n"
        "    public void put(Cell w) { this.data[0] = w; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box b = heap Box();\n"
        "        Cell c = heap Cell(9);\n"
        "        b.put(c);\n"
        "        return b.data[0].n;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 9);
    EXPECT_TRUE(plainStoreWarnedAbout("w"));
}

// 6.1.1c — the title-assign spelling stays quiet: `dst #= v` moves the title,
// so there is nothing to warn about.
//
// This case tested BOTH spellings until Unit 7 respelled the stdlib and tests
// (7.2.1); its `keepOld` half was the legacy `dst = #v`. That spelling is now
// exercised only where it is still written on purpose — the deprecation pin
// (TransferAssignDeprecationTests.legacyTransferAssignIsNotAPlainStore), which
// keeps this exclusion under test until the Phase 3 error flip retires it.
TEST(PlainStoreDiagnosticTests, sharpSpellingsQuiet) {
    std::string src = std::string(kCellSrc) +
        "public class Holder {\n"
        "    public Cell a;\n"
        "    public Cell b;\n"
        "    public void keepNew(Cell v) { this.a #= v; }\n"
        "    public void keepOld(Cell u) { this.b #= u; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Holder h = heap Holder();\n"
        "        h.keepNew(#heap Cell(3));\n"
        "        h.keepOld(#heap Cell(4));\n"
        "        return h.a.n * 10 + h.b.n;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 34);
    EXPECT_FALSE(plainStoreWarnedAbout("v"));
    EXPECT_FALSE(plainStoreWarnedAbout("u"));
}

// 6.1.1d — primitive formals stay quiet (no title to move).
TEST(PlainStoreDiagnosticTests, primitiveFormalQuiet) {
    std::string src =
        "package test;\n"
        "public class Counter {\n"
        "    public int32 n;\n"
        "    public void set(int32 k) { this.n = k; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Counter c = heap Counter();\n"
        "        c.set(41);\n"
        "        return c.n;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 41);
    EXPECT_FALSE(plainStoreWarnedAbout("k"));
}

// 6.1.1e — String formals stay quiet: a plain String store resolves to a
// copy/stake (dual-role), so nothing dangles. (The slice-resolved NOTE may
// still fire; different code.)
TEST(PlainStoreDiagnosticTests, stringFormalQuiet) {
    std::string src =
        "package test;\n"
        "public class Tag {\n"
        "    public String name;\n"
        "    public void label(String s) { this.name = s; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Tag t = heap Tag();\n"
        "        String v = \"abcde\";\n"
        "        t.label(v);\n"
        "        return t.name.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
    EXPECT_FALSE(plainStoreWarnedAbout("s"));
}

// 6.1.1f — statically-borrowed and statically-owned LOCALS stay quiet under
// THIS code: neither is runtime-conditional. (The owned-local dangling-store
// family belongs to the last-use advisory, not the loud-plain-store.)
TEST(PlainStoreDiagnosticTests, plainLocalsQuiet) {
    std::string src = std::string(kCellSrc) +
        "public class Holder {\n"
        "    public Cell src;\n"
        "    public Cell held;\n"
        "    public Holder() { this.src #= heap Cell(6); }\n"
        "    public void borrowStore() {\n"
        "        Cell t = this.src;\n"      // statically borrowed
        "        this.held = t;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Holder h = heap Holder();\n"
        "        h.borrowStore();\n"
        "        return h.held.n;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 6);
    EXPECT_FALSE(plainStoreWarnedAbout("t"));
}

// 6.1.1g — a local whose ownership is decided at RUNTIME is a runtime owner,
// so a later plain retaining store of it warns. The runtime-conditional part
// is load-bearing: 6.1.1f above pins that a statically-owned local stays quiet,
// because the compiler can already see what happens to it.
//
// uniform-transfer Unit 3 rewrote this fixture. It used to get its runtime flag
// by claiming from a BORROWED array slot with the fused claim
// (`Cell f #= #this.slots[0]`), which forwarded the slot's mode verbatim and so
// quietly produced a BORROW where the code said "claim". That spelling is gone:
// `#=` from a titleless slot now means what it says — a demand for a title the
// slot does not have — and panics CAJETA_PANIC_TITLE_MISS.
//
// The runtime-conditional shape that remains is `#=` from a PLAIN FORMAL, which
// is conditional acquisition: whether `f` ends up owning depends on what the
// CALLER did, and only the caller knows. Here the caller LENDS, so `f` is a
// borrow, `this.parked` aliases a Cell the Shelf still owns, and the trailing
// read is sound — while the compile-time warning still fires, because a
// different caller passing `#c` would leave `parked` dangling. That is exactly
// the hazard CAJETA_WARN_PLAIN_RETAIN_STORE names.
TEST(PlainStoreDiagnosticTests, flaggedLocalClaimWarns) {
    std::string src = std::string(kCellSrc) +
        "public class Shelf {\n"
        "    public Cell keeper;\n"
        "    public Cell parked;\n"
        "    public Shelf() {\n"
        "        this.keeper #= heap Cell(8);\n"
        "    }\n"
        "    public void park(Cell c) {\n"
        "        Cell f #= c;\n"          // conditional acquisition -> runtime flag
        "        this.parked = f;\n"      // plain retain -> WARN
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Shelf s = heap Shelf();\n"
        "        s.park(s.keeper);\n"     // LEND — keeper keeps the title
        "        return s.parked.n;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 8);
    EXPECT_TRUE(plainStoreWarnedAbout("f"));
}
