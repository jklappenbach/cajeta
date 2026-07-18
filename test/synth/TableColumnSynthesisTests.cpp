//
// Unit 5 of the source-synthesis facility
// (docs/specification/nucleo/source-synthesis-spec.md §3.4; plan
// agents/cajeta/nucleo/source-synthesis-plan.md §5). The direct downstream
// consumer of the `T.class`-in-template fix (records 7.1.2): instantiating
// `Table<T>` reflects record `T`'s fields and injects one typed column
// accessor per field, at instantiation time, through the member-synthesis
// seam Unit 3 built.
//
// The `Table<T>` shell is a test-local stand-in (plan 5.2.1) — a reference
// class holding one `T sample` row; each synthesized accessor projects a
// field off it. The built-in `TableSynthesizer` self-selects on the
// instantiation of a template whose origin is named `Table` with a single
// record type argument, so a locally declared `Table<T>` triggers it. The
// production binding is `dev.cajeta.nucleo.frame.Table`; the full lazy
// dataframe is `nucleo-frame`, out of scope here.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

// The test-local Table<T> shell + a record to reflect. Kept as a prefix so
// each case appends its own `D::run`.
const char* kTableSrc =
    "package test;\n"
    "public record Tick {\n"
    "    float64 price;\n"
    "    int32 volume;\n"
    "}\n"
    "public class Table<T> {\n"
    "    public T sample;\n"
    "    public Table(T row) { this.sample = row; }\n"
    "}\n";

} // namespace

// 5.1.1 / 5.1.3 — instantiating Table<Tick> injects one accessor per field of
// Tick, in declared order, each typed to its field. The `volume` accessor is
// int32-typed and the synthesized member re-checks + codegens as an ordinary
// method: calling it returns the reflected field's value.
TEST(TableColumnSynthesisTests, injectsIntColumnAccessor) {
    auto src = std::string(kTableSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Tick t = Tick { price: 12.5, volume: 300 };\n"
        "        Table<Tick> tbl = heap Table<Tick>(t);\n"
        "        return tbl.volume();\n"  // synthesized int32 accessor -> 300
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 300);
}

// 5.1.1 — the `price` accessor is float64-typed (one accessor per field, the
// float column alongside the int column).
TEST(TableColumnSynthesisTests, injectsFloatColumnAccessor) {
    auto src = std::string(kTableSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Tick t = Tick { price: 12.5, volume: 300 };\n"
        "        Table<Tick> tbl = heap Table<Tick>(t);\n"
        "        return (int32)(tbl.price() * 10.0);\n"  // 125
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 125);
}

// 5.1.2 — a call-site typo is caught at COMPILE TIME, not by a runtime lookup
// that silently misses. The accessor set is real typed members: `tbl.volume()`
// resolves; `tbl.prce()` names no member and FAILS THE COMPILE.
//
// This test used to assert on the `"lowered to null"` stderr warning — i.e. it
// pinned the silent miscompile (the call lowered to null and the program ran)
// as its contract, even though its own name says "caught at compile time". Since
// silent-resolution-diagnostics Units 1-2 the miss is a hard, located
// CAJETA_ERROR_MEMBER_NOT_FOUND, so it can assert what it always meant.
//
// The id is the SAME one a typo on a HAND-WRITTEN member produces
// (MemberNotFoundTests) — that identity is the composition-only property source
// synthesis promises: synthesized members are checked exactly like hand-written
// ones. Asserting the id, not merely "it threw", is what pins that.
TEST(TableColumnSynthesisTests, columnTypoIsCaughtAtCompileTime) {
    // The correct accessor resolves and runs.
    auto good = std::string(kTableSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Tick t = Tick { price: 12.5, volume: 300 };\n"
        "        Table<Tick> tbl = heap Table<Tick>(t);\n"
        "        return tbl.volume();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(good), 300);

    // The typo names no member of Table<Tick> — it must not compile.
    auto typo = std::string(kTableSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Tick t = Tick { price: 12.5, volume: 300 };\n"
        "        Table<Tick> tbl = heap Table<Tick>(t);\n"
        "        return tbl.prce();\n"
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(typo, "test.D");
        FAIL() << "expected a typo'd synthesized accessor to fail the compile";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_MEMBER_NOT_FOUND")
            << "got: " << e.getErrorId() << " — " << e.getMessage();
        EXPECT_NE(e.getMessage().find("prce"), std::string::npos)
            << "must name the missing accessor: " << e.getMessage();
    }
}

// 5.1.4 — determinism / memoization: Table<Tick> referenced twice resolves to
// one cached monomorphization, so the accessor set is synthesized once and
// serves both uses.
TEST(TableColumnSynthesisTests, repeatedInstantiationSharesAccessors) {
    auto src = std::string(kTableSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Tick a = Tick { price: 1.0, volume: 100 };\n"
        "        Tick b = Tick { price: 2.0, volume: 200 };\n"
        "        Table<Tick> t1 = heap Table<Tick>(a);\n"
        "        Table<Tick> t2 = heap Table<Tick>(b);\n"
        "        return t1.volume() + t2.volume();\n"  // 300
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 300);
}
