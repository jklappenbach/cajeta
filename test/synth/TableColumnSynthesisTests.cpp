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

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

std::string compileCaptureStderr(const std::string& src) {
    testing::internal::CaptureStderr();
    try {
        CajetaJit::compile(src, "test.D");
    } catch (...) {
        // A hard compile failure still leaves its diagnostic on stderr.
    }
    return testing::internal::GetCapturedStderr();
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
// resolves, `tbl.prce()` resolves to no member. Cajeta diagnoses the miss at
// compile time (it never does a runtime string lookup) — the codegen symptom
// is the unresolved call lowering to null with a compile-time diagnostic. The
// correct call produces no such diagnostic; the typo does.
TEST(TableColumnSynthesisTests, columnTypoIsCaughtAtCompileTime) {
    const char* good =
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Tick t = Tick { price: 12.5, volume: 300 };\n"
        "        Table<Tick> tbl = heap Table<Tick>(t);\n"
        "        return tbl.volume();\n"  // resolves to the synthesized accessor
        "    }\n"
        "}\n";
    const char* typo =
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Tick t = Tick { price: 12.5, volume: 300 };\n"
        "        Table<Tick> tbl = heap Table<Tick>(t);\n"
        "        return tbl.prce();\n"  // typo -> no such member
        "    }\n"
        "}\n";
    std::string goodErr = compileCaptureStderr(std::string(kTableSrc) + good);
    std::string typoErr = compileCaptureStderr(std::string(kTableSrc) + typo);
    EXPECT_EQ(goodErr.find("lowered to null"), std::string::npos)
        << "the correct accessor must resolve cleanly, got: " << goodErr;
    EXPECT_NE(typoErr.find("lowered to null"), std::string::npos)
        << "the typo must be diagnosed at compile time, got: " << typoErr;
    EXPECT_NE(typoErr.find("test.D::run()"), std::string::npos)
        << "the diagnostic must attribute the unresolved call, got: " << typoErr;
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
