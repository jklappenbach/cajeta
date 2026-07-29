//
// nucleo-column U4 acceptance (spec §9): a full EXPORT -> IMPORT round trip
// through the real C structs — a cajeta column exported over the C Data
// Interface and re-imported (as a foreign consumer would) preserves values
// and the null pattern, zero-copy in both directions (the imported view
// reads the ORIGINAL column's buffer).
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {
struct ArrowSchemaAbi {
    const char* format; const char* name; const char* metadata;
    int64_t flags; int64_t n_children;
    ArrowSchemaAbi** children; ArrowSchemaAbi* dictionary;
    void (*release)(ArrowSchemaAbi*); void* private_data;
};
struct ArrowArrayAbi {
    int64_t length; int64_t null_count; int64_t offset;
    int64_t n_buffers; int64_t n_children;
    const void** buffers; ArrowArrayAbi** children; ArrowArrayAbi* dictionary;
    void (*release)(ArrowArrayAbi*); void* private_data;
};
struct ExportBundleHead {
    ArrowSchemaAbi schema;
    ArrowArrayAbi array;
};

const char* kProgram =
    "package test;\n"
    "import cajeta.nucleo.column.Column;\n"
    "import cajeta.nucleo.column.NullableColumn;\n"
    "public final class T {\n"
    "    static Column<float32> held;\n"
    "    static NullableColumn<float32> heldN;\n"
    "    public static int64 exportPlain() {\n"
    "        float32[] fa = [ 4.5f, 5.5f, 6.5f ];\n"
    "        Column<float32> c = Column.of<float32>(fa);\n"
    "        T.held = c;\n"
    "        return T.held.exportArrow();\n"
    "    }\n"
    "    public static int64 exportHeldAgain() { return T.held.exportArrow(); }\n"
    "    public static float32 reimportRead(int64 s, int64 a, int64 i) {\n"
    "        Column<?> w = Column.importArrow(s, a);\n"
    "        Column<float32> c = (Column<float32>) w;\n"
    "        return c.get(i);\n"
    "    }\n"
    "    public static void pokeHeld(int64 i, float32 v) { T.held.set(i, v); }\n"
    "    public static int64 exportNullable() {\n"
    "        float32[] va = [ 1.0f, 2.0f, 3.0f, 4.0f ];\n"
    "        boolean[] ok = [ false, true, true, false ];\n"
    "        NullableColumn<float32> c = NullableColumn.of<float32>(va, ok);\n"
    "        T.heldN = c;\n"
    "        return T.heldN.exportArrow();\n"
    "    }\n"
    "    public static int64 reimportNullPattern(int64 s, int64 a) {\n"
    "        NullableColumn<?> w = NullableColumn.importArrow(s, a);\n"
    "        NullableColumn<float32> c = (NullableColumn<float32>) w;\n"
    "        int64 m = 0;\n"
    "        if (c.isValid(0)) { m = m + 1; }\n"
    "        if (c.isValid(1)) { m = m + 10; }\n"
    "        if (c.isValid(2)) { m = m + 100; }\n"
    "        if (c.isValid(3)) { m = m + 1000; }\n"
    "        if (c.get(1) == 2.0f) { m = m + 10000; }\n"
    "        return m;\n"
    "    }\n"
    "}\n";
} // namespace

// Values survive the round trip, and the re-imported view is ZERO-COPY over
// the original column's buffer (a poke through the source is visible through
// a subsequent round trip). A bundle is CONSUMED by its import (the drop
// releases it), so each read takes a fresh export — the Arrow contract.
TEST(ArrowRoundTrip, plainValuesSurviveAndStayLive) {
    auto jit = CajetaJit::compile(kProgram, "test.T");
    auto again = jit->lookup<int64_t (*)()>("exportHeldAgain");
    auto rd = jit->lookup<float (*)(int64_t, int64_t, int64_t)>("reimportRead");
    auto addrsOf = [](int64_t h, int64_t& s, int64_t& a) {
        auto* b = reinterpret_cast<ExportBundleHead*>(h);
        s = reinterpret_cast<int64_t>(&b->schema);
        a = reinterpret_cast<int64_t>(&b->array);
    };
    int64_t s, a;
    addrsOf(jit->lookup<int64_t (*)()>("exportPlain")(), s, a);
    EXPECT_FLOAT_EQ(rd(s, a, 0), 4.5f);
    addrsOf(again(), s, a);
    EXPECT_FLOAT_EQ(rd(s, a, 2), 6.5f);
    jit->lookup<void (*)(int64_t, float)>("pokeHeld")(1, 77.0f);
    addrsOf(again(), s, a);
    EXPECT_FLOAT_EQ(rd(s, a, 1), 77.0f)
        << "the round trip copied somewhere - both directions must borrow";
}

// The null pattern survives the round trip bit-for-bit.
TEST(ArrowRoundTrip, nullPatternSurvives) {
    auto jit = CajetaJit::compile(kProgram, "test.T");
    int64_t h = jit->lookup<int64_t (*)()>("exportNullable")();
    auto* b = reinterpret_cast<ExportBundleHead*>(h);
    EXPECT_EQ(jit->lookup<int64_t (*)(int64_t, int64_t)>("reimportNullPattern")(
                  reinterpret_cast<int64_t>(&b->schema),
                  reinterpret_cast<int64_t>(&b->array)), 10110);
}
