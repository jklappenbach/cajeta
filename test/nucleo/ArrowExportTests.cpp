//
// nucleo-column Unit 3 — the C Data Interface, EXPORT half (spec 4.1, 4.4,
// 6.2). Interop is by MATCHING the frozen Arrow C ABI — the structs below are
// defined here in the test, because this test IS the consumer: no libarrow
// anywhere (spec §1.3).
//
// `exportArrow()` returns the address of a C-allocated bundle whose head is
// { ArrowSchema, ArrowArray } (the documented export ABI in cajeta_arrow.c),
// with buffer pointers aimed at the column's LIVE aligned buffers — the
// export is a zero-copy BORROW (spec 4.4): our release frees only the struct
// shells, never the column's memory, and mutations through the column are
// visible through the exported pointers until release.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// The frozen Arrow C Data Interface ABI (format spec, stable since 2020).
struct ArrowSchemaAbi {
    const char* format;
    const char* name;
    const char* metadata;
    int64_t flags;
    int64_t n_children;
    ArrowSchemaAbi** children;
    ArrowSchemaAbi* dictionary;
    void (*release)(ArrowSchemaAbi*);
    void* private_data;
};
struct ArrowArrayAbi {
    int64_t length;
    int64_t null_count;
    int64_t offset;
    int64_t n_buffers;
    int64_t n_children;
    const void** buffers;
    ArrowArrayAbi** children;
    ArrowArrayAbi* dictionary;
    void (*release)(ArrowArrayAbi*);
    void* private_data;
};
// The export-bundle head contract documented in cajeta_arrow.c.
struct ExportBundleHead {
    ArrowSchemaAbi schema;
    ArrowArrayAbi array;
};

constexpr int64_t kArrowFlagNullable = 2;

const char* kProgram =
    "package test;\n"
    "import cajeta.nucleo.column.Column;\n"
    "import cajeta.nucleo.column.NullableColumn;\n"
    "public final class T {\n"
    "    static Column<float32> held;\n"
    "    static NullableColumn<float32> heldN;\n"
    "    public static int64 run() {\n"
    "        float32[] fa = [ 1.5f, 2.5f, 3.5f ];\n"
    "        Column<float32> c #= Column.of<float32>(fa);\n"
    "        T.held = c;\n"                 // the BackendRegistry.shared idiom
    "        return T.held.exportArrow();\n"
    "    }\n"
    "    public static int64 heldDataAddr() { return T.held.dataAddress(); }\n"
    "    public static float32 readHeld(int64 i) { return T.held.get(i); }\n"
    "    public static void pokeHeld(int64 i, float32 v) { T.held.set(i, v); }\n"
    "    public static int64 runNullable() {\n"
    "        float32[] va = [ 10.0f, 20.0f, 30.0f, 40.0f ];\n"
    "        boolean[] ok = [ true, false, true, false ];\n"
    "        NullableColumn<float32> c #= NullableColumn.of<float32>(va, ok);\n"
    "        T.heldN = c;\n"
    "        return T.heldN.exportArrow();\n"
    "    }\n"
    "    public static int64 heldDataAddrN() { return T.heldN.dataAddress(); }\n"
    "    public static int64 heldValidityAddrN() { return T.heldN.validityAddress(); }\n"
    "}\n";

} // namespace

// 3.1.1 — a non-null fixed-width export: format/length/null_count per spec,
// buffers = [validity=NULL, data], and the data pointer IS the column's live
// buffer (zero-copy, same address).
TEST(ArrowExport, fixedWidthNonNullMatchesSpec) {
    auto jit = CajetaJit::compile(kProgram, "test.T");
    int64_t h = jit->lookup<int64_t (*)()>("run")();
    ASSERT_NE(h, 0);
    auto* b = reinterpret_cast<ExportBundleHead*>(h);

    EXPECT_STREQ(b->schema.format, "f");           // float32
    EXPECT_EQ(b->schema.n_children, 0);
    EXPECT_EQ(b->schema.flags & kArrowFlagNullable, 0);
    ASSERT_NE(b->schema.release, nullptr);

    EXPECT_EQ(b->array.length, 3);
    EXPECT_EQ(b->array.null_count, 0);
    EXPECT_EQ(b->array.offset, 0);
    ASSERT_EQ(b->array.n_buffers, 2);
    EXPECT_EQ(b->array.buffers[0], nullptr);       // no validity bitmap
    int64_t liveAddr = jit->lookup<int64_t (*)()>("heldDataAddr")();
    EXPECT_EQ(reinterpret_cast<int64_t>(b->array.buffers[1]), liveAddr)
        << "export copied the buffer instead of borrowing it";
    auto* vals = reinterpret_cast<const float*>(b->array.buffers[1]);
    EXPECT_FLOAT_EQ(vals[0], 1.5f);
    EXPECT_FLOAT_EQ(vals[2], 3.5f);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(b->array.buffers[1]) % 64, 0u)
        << "exported data pointer not 64-byte aligned (6.2)";
}

// 3.1.2 — a nullable export: buffers = [validity, data], true null_count,
// nullable flag set, LSB-first bitmap bytes.
TEST(ArrowExport, nullableCarriesValidityAndNullCount) {
    auto jit = CajetaJit::compile(kProgram, "test.T");
    int64_t h = jit->lookup<int64_t (*)()>("runNullable")();
    ASSERT_NE(h, 0);
    auto* b = reinterpret_cast<ExportBundleHead*>(h);

    EXPECT_STREQ(b->schema.format, "f");
    EXPECT_EQ(b->schema.flags & kArrowFlagNullable, kArrowFlagNullable);
    EXPECT_EQ(b->array.length, 4);
    EXPECT_EQ(b->array.null_count, 2);
    ASSERT_EQ(b->array.n_buffers, 2);
    int64_t va = jit->lookup<int64_t (*)()>("heldValidityAddrN")();
    int64_t da = jit->lookup<int64_t (*)()>("heldDataAddrN")();
    EXPECT_EQ(reinterpret_cast<int64_t>(b->array.buffers[0]), va);
    EXPECT_EQ(reinterpret_cast<int64_t>(b->array.buffers[1]), da);
    // valid {t,f,t,f} LSB-first -> 0b0101 = 5.
    EXPECT_EQ(reinterpret_cast<const uint8_t*>(b->array.buffers[0])[0], 5);
}

// 3.1.4 — the export is a view of LIVE buffers, not a snapshot: a mutation
// through the column is visible through the exported data pointer.
TEST(ArrowExport, exportSeesLiveMutation) {
    auto jit = CajetaJit::compile(kProgram, "test.T");
    int64_t h = jit->lookup<int64_t (*)()>("run")();
    auto* b = reinterpret_cast<ExportBundleHead*>(h);
    jit->lookup<void (*)(int64_t, float)>("pokeHeld")(1, 99.0f);
    auto* vals = reinterpret_cast<const float*>(b->array.buffers[1]);
    EXPECT_FLOAT_EQ(vals[1], 99.0f);
}

// 3.1.3 — release semantics (spec 4.4): the consumer calling release frees
// the SHELLS exactly once (the released marker nulls the callback per the
// Arrow contract), and the column's own buffers stay alive and readable.
TEST(ArrowExport, releaseFreesShellsNotBuffers) {
    auto jit = CajetaJit::compile(kProgram, "test.T");
    int64_t h = jit->lookup<int64_t (*)()>("run")();
    auto* b = reinterpret_cast<ExportBundleHead*>(h);

    ASSERT_NE(b->schema.release, nullptr);
    ASSERT_NE(b->array.release, nullptr);
    b->schema.release(&b->schema);
    EXPECT_EQ(b->schema.release, nullptr)
        << "released schema must null its callback (Arrow released marker)";
    b->array.release(&b->array);
    // The bundle is freed on the second release — b is dangling now; the
    // COLUMN must still be alive and correct through the language surface.
    EXPECT_FLOAT_EQ(jit->lookup<float (*)(int64_t)>("readHeld")(2), 3.5f)
        << "release freed the column's buffers (must free shells only)";
}
