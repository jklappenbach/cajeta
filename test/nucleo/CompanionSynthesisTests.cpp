//
// nucleo-frame U1 (spike) — companion-CLASS synthesis (plan 1.1.2).
//
// A `Table<Record>` instantiation synthesizes the `<Record>Cols` builder as
// a REAL named class in the record's package: registered (canonicalMap +
// module structures), prototyped at the instantiation hook, and emitted by
// the codegen fixed-point loop (registration-only — the hook has no live
// builder; the enum-companion discipline). Forward references work: the
// prescan archives `<Record>Cols` beside every record (the @GenerateMock
// sibling pattern), so a signature naming it BEFORE the instantiation fires
// resolves to a placeholder the companion runner FILLS — never a hollow
// shell, never a skip.
//
// Builders return the TYPED node family (1.2.2): `c.price()` is a
// `#ColF64` colRef carrying its schema ordinal + name.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {
const char* kPrefix =
    "package test;\n"
    "import cajeta.nucleo.frame.ColF64;\n"
    "public record Tick {\n"
    "    float64 price;\n"
    "    float64 size;\n"
    "    public Tick(float64 price, float64 size) {\n"
    "        this.price #= price;\n"
    "        this.size #= size;\n"
    "    }\n"
    "}\n"
    "public class Table<T> {\n"
    "    T sample;\n"
    "    public Table(T sample) {\n"
    "        this.sample #= sample;\n"
    "    }\n"
    "}\n";
} // namespace

// Same-method use: instantiate Table<Tick>, then name TickCols.
TEST(CompanionSynthesisTests, colsCompanionResolvesAfterInstantiation) {
    std::string src = std::string(kPrefix) +
        "public final class T {\n"
        "    public static int32 run() {\n"
        "        Table<Tick> ticks = heap Table<Tick>(stack Tick(1.0, 2.0));\n"
        "        TickCols c = heap TickCols();\n"
        "        return c.price().ordinal() * 10 + c.size().ordinal() + 700;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.T");
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 701);   // ordinals: price 0, size 1
}

// FORWARD reference: a method declared BEFORE the instantiation site names
// TickCols in its signature — the archived placeholder gets filled.
TEST(CompanionSynthesisTests, colsCompanionForwardReferenceFills) {
    std::string src = std::string(kPrefix) +
        "public final class T {\n"
        "    static int32 earlier(TickCols c) {\n"
        "        return c.price().ordinal() * 100 + c.size().ordinal() * 10;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Table<Tick> ticks = heap Table<Tick>(stack Tick(1.0, 2.0));\n"
        "        TickCols c = heap TickCols();\n"
        "        return earlier(c) + c.price().ordinal() * 10 + c.size().ordinal() + 700;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.T");
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 711);   // earlier: 0*100 + 1*10; then 0*10 + 1 + 700
}
