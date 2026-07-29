//
// nucleo-frame U2 (spike) — member resolution on wildcard receivers
// (plan 2.1.1, 2.1.2). The gradual-typing model needs method calls on
// `Table<?>` (`.as<R>()`, `col("...")`, terminals) and inherited members
// through `Table<? extends Tick>`. Both WORK natively — including
// `Class<?>` members (the nucleo-nn-optim-era wall no longer reproduces;
// its ledger item is retired by this pin). No compiler fix was needed
// (plan 2.2.1: not applicable).
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <string>

using cajeta_test::CajetaJit;

TEST(TableWildcardSpikeTests, wildcardReceiversResolveMembers) {
    std::string src =
        "package test;\n"
        "import cajeta.reflect.Class;\n"
        "public record Tick {\n"
        "    float64 price;\n"
        "    public Tick(float64 price) { this.price #= price; }\n"
        "}\n"
        "public class Table<T> {\n"
        "    T sample;\n"
        "    int32 rows;\n"
        "    public Table(T sample, int32 rows) {\n"
        "        this.sample #= sample;\n"
        "        this.rows = rows;\n"
        "    }\n"
        "    public int32 rowCount() { return this.rows; }\n"
        "}\n"
        "public final class T {\n"
        "    static int32 useWild(Table<?> t) {\n"          // unbounded param
        "        return t.rowCount();\n"
        "    }\n"
        "    static int32 useBounded(Table<? extends Tick> t) {\n"  // bounded
        "        return t.rowCount() * 2;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Table<Tick> ticks = heap Table<Tick>(stack Tick(1.0), 42);\n"
        "        Table<?> w = ticks;\n"                     // wildcard local
        "        int32 direct = w.rowCount();\n"
        "        int32 viaParam = useWild(ticks);\n"
        "        int32 bounded = useBounded(ticks);\n"
        "        Class<?> c = Class.of(ticks);\n"           // the old nn wall
        "        int32 fc = c.getFieldCount();\n"
        "        return direct + viaParam + bounded + fc * 1000;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.T");
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 2168);   // 42 + 42 + 84 + 2*1000
}
