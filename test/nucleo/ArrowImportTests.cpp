//
// nucleo-column Unit 4 — the C Data Interface, IMPORT half (spec 3.5, 4.2,
// 4.3): the foreign-backed column (the developer-decided Q-import shape).
//
// This test is the PRODUCER: it builds frozen-ABI ArrowSchema/ArrowArray
// structs over its own buffers, with release callbacks that count. Import
// wraps those buffers ZERO-COPY (reads go through the producer's memory —
// proven by poking the buffer between imports); the producer's releases run
// EXACTLY ONCE, when the imported column drops; null_count > 0 admits only
// as NullableColumn (the type reflects the physical reality, 3.5);
// `materialize()` is the explicit copy at the compute boundary, and
// `asTensor()` on a foreign-backed column is a named error directing there.
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

// A counting producer over its own float32 buffer (+ optional validity).
struct Producer {
    ArrowSchemaAbi schema{};
    ArrowArrayAbi array{};
    const void* bufs[2] = {nullptr, nullptr};
    float vals[8] = {7, 8, 9, 10, 0, 0, 0, 0};
    uint8_t validity = 0;
    int schemaReleases = 0;
    int arrayReleases = 0;

    static void releaseSchema(ArrowSchemaAbi* s) {
        auto* p = static_cast<Producer*>(s->private_data);
        p->schemaReleases++;
        s->release = nullptr;
    }
    static void releaseArray(ArrowArrayAbi* a) {
        auto* p = static_cast<Producer*>(a->private_data);
        p->arrayReleases++;
        a->release = nullptr;
    }

    // fmt: "f" float32 (or an unknown like "u" for the error test).
    // nullCount < 0 means: no validity buffer, null_count 0.
    void build(const char* fmt, int64_t length, int64_t nullCount,
               uint8_t validityBits) {
        schema.format = fmt;
        schema.name = "";
        schema.flags = nullCount > 0 ? 2 : 0;
        schema.release = &Producer::releaseSchema;
        schema.private_data = this;
        validity = validityBits;
        bufs[0] = nullCount > 0 ? &validity : nullptr;
        bufs[1] = vals;
        array.length = length;
        array.null_count = nullCount > 0 ? nullCount : 0;
        array.n_buffers = 2;
        array.buffers = bufs;
        array.release = &Producer::releaseArray;
        array.private_data = this;
    }
};

const char* kProgram =
    "package test;\n"
    "import cajeta.math.Tensor;\n"
    "import cajeta.nucleo.column.Column;\n"
    "import cajeta.nucleo.column.NullableColumn;\n"
    "import cajeta.nucleo.column.ColumnTypeException;\n"
    "public final class T {\n"
    "    static Column<float32> kept;\n"
    "    public static float32 importAndRead(int64 s, int64 a) {\n"
    "        Column<?> w = Column.importArrow(s, a);\n"
    "        if (!(w instanceof Column<float32>)) { return -1.0f; }\n"
    "        Column<float32> c = (Column<float32>) w;\n"
    "        return c.get(2);\n"
    "    }\n"
    "    public static int64 importNullable(int64 s, int64 a) {\n"
    "        NullableColumn<?> w = NullableColumn.importArrow(s, a);\n"
    "        if (!(w instanceof NullableColumn<float32>)) { return -1; }\n"
    "        NullableColumn<float32> c = (NullableColumn<float32>) w;\n"
    "        int64 m = 0;\n"
    "        if (c.isValid(0)) { m = m + 1; }\n"
    "        if (c.isValid(1)) { m = m + 10; }\n"
    "        if (c.isValid(2)) { m = m + 100; }\n"
    "        if (c.isValid(3)) { m = m + 1000; }\n"
    "        if (c.get(0) == 7.0f) { m = m + 10000; }\n"
    "        return m;\n"
    "    }\n"
    "    public static int64 importWrongType(int64 s, int64 a) {\n"
    "        try {\n"
    "            Column<?> w = Column.importArrow(s, a);\n"
    "            return 0;\n"
    "        } catch (ColumnTypeException e) {\n"
    "            return 1;\n"
    "        }\n"
    "    }\n"
    "    public static float32 importMaterialize(int64 s, int64 a) {\n"
    "        Column<?> w = Column.importArrow(s, a);\n"
    "        Column<float32> c = (Column<float32>) w;\n"
    "        T.kept = c.materialize();\n"
    "        return T.kept.get(1);\n"
    "    }\n"
    "    public static float32 readKept(int64 i) { return T.kept.get(i); }\n"
    "    public static int64 keptAligned() { return T.kept.dataAddress() % 64; }\n"
    "    public static int64 foreignAsTensorRejected(int64 s, int64 a) {\n"
    "        Column<?> w = Column.importArrow(s, a);\n"
    "        Column<float32> c = (Column<float32>) w;\n"
    "        try {\n"
    "            Tensor<float32> t = c.asTensor();\n"
    "            return 0;\n"
    "        } catch (ColumnTypeException e) {\n"
    "            return 1;\n"
    "        }\n"
    "    }\n"
    "}\n";

int64_t sa(Producer& p) { return reinterpret_cast<int64_t>(&p.schema); }
int64_t aa(Producer& p) { return reinterpret_cast<int64_t>(&p.array); }

} // namespace

// 4.1.1 — zero-copy: reads go through the PRODUCER's buffer (poke between
// imports and the imported view sees it), airlock returns Column<?> captured
// by instanceof + cast.
TEST(ArrowImport, zeroCopyReadsForeignBuffer) {
    auto jit = CajetaJit::compile(kProgram, "test.T");
    auto fn = jit->lookup<float (*)(int64_t, int64_t)>("importAndRead");
    Producer p;
    p.build("f", 4, 0, 0);
    EXPECT_FLOAT_EQ(fn(sa(p), aa(p)), 9.0f);
    Producer q;
    q.build("f", 4, 0, 0);
    q.vals[2] = 42.0f;                       // the same physical memory rule
    EXPECT_FLOAT_EQ(fn(sa(q), aa(q)), 42.0f)
        << "import copied instead of borrowing the producer's buffer";
}

// 4.1.3 — the producer's releases fire EXACTLY ONCE, when the imported
// column drops (not on import, not twice).
TEST(ArrowImport, producerReleaseExactlyOnceOnDrop) {
    auto jit = CajetaJit::compile(kProgram, "test.T");
    auto fn = jit->lookup<float (*)(int64_t, int64_t)>("importAndRead");
    Producer p;
    p.build("f", 4, 0, 0);
    ASSERT_FLOAT_EQ(fn(sa(p), aa(p)), 9.0f);
    EXPECT_EQ(p.schemaReleases, 1);
    EXPECT_EQ(p.arrayReleases, 1);
}

// 4.1.2 — null_count > 0 admits only as NullableColumn: Column.importArrow
// rejects by name; NullableColumn.importArrow honors the bitmap.
TEST(ArrowImport, nullCountRoutesToNullable) {
    auto jit = CajetaJit::compile(kProgram, "test.T");
    Producer p;
    p.build("f", 4, 2, /*validity LSB-first {t,f,t,f}=*/5);
    EXPECT_EQ(jit->lookup<int64_t (*)(int64_t, int64_t)>("importWrongType")(
                  sa(p), aa(p)), 1)
        << "Column.importArrow accepted a null-carrying array";
    Producer q;
    q.build("f", 4, 2, 5);
    EXPECT_EQ(jit->lookup<int64_t (*)(int64_t, int64_t)>("importNullable")(
                  sa(q), aa(q)), 10101);
}

// 4.1.4 — materialize() copies into an OWNED aligned column that survives
// the foreign borrow: after the source is poked post-materialize, the kept
// column still reads the ORIGINAL values; its buffer is 64-byte aligned.
TEST(ArrowImport, materializeOwnsAndDetaches) {
    auto jit = CajetaJit::compile(kProgram, "test.T");
    Producer p;
    p.build("f", 4, 0, 0);
    ASSERT_FLOAT_EQ(jit->lookup<float (*)(int64_t, int64_t)>("importMaterialize")(
                        sa(p), aa(p)), 8.0f);
    EXPECT_EQ(p.arrayReleases, 1) << "foreign borrow not released after scope";
    p.vals[1] = -5.0f;                       // mutate the producer AFTER
    EXPECT_FLOAT_EQ(jit->lookup<float (*)(int64_t)>("readKept")(1), 8.0f)
        << "materialize did not detach from the foreign buffer";
    EXPECT_EQ(jit->lookup<int64_t (*)()>("keptAligned")(), 0);
}

// 4.1.4 (refusal half) — asTensor() on a foreign-backed column is a named
// error directing to materialize(), never a view over unowned memory.
TEST(ArrowImport, foreignAsTensorIsNamedError) {
    auto jit = CajetaJit::compile(kProgram, "test.T");
    Producer p;
    p.build("f", 4, 0, 0);
    EXPECT_EQ(jit->lookup<int64_t (*)(int64_t, int64_t)>("foreignAsTensorRejected")(
                  sa(p), aa(p)), 1);
}

// 4.1.5 — an unknown format string is a NAMED error, never a misread.
TEST(ArrowImport, unknownFormatIsNamedError) {
    auto jit = CajetaJit::compile(kProgram, "test.T");
    Producer p;
    p.build("Z", 4, 0, 0);                   // not a v1 fixed-width format
    EXPECT_EQ(jit->lookup<int64_t (*)(int64_t, int64_t)>("importWrongType")(
                  sa(p), aa(p)), 1);
}
