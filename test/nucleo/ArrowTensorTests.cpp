//
// nucleo-column Unit 6 — the Tensor Arrow retrofit (spec 7.2, 7.3 as
// narrowed), ADDITIVE: `tensor.exportArrow()` beside the untouched
// `tensor.protocol()`, and the import-to-tensor crossing
// (`Column.importAsTensor` — one explicit materializing copy; it lives
// column-side because cajeta.math cannot import the column package without
// a lazy-parse cycle).
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

struct Producer {
    ArrowSchemaAbi schema{};
    ArrowArrayAbi array{};
    const void* bufs[2] = {nullptr, nullptr};
    float vals[4] = {7, 8, 9, 10};
    int releases = 0;
    static void relS(ArrowSchemaAbi* s) {
        static_cast<Producer*>(s->private_data)->releases++;
        s->release = nullptr;
    }
    static void relA(ArrowArrayAbi* a) {
        static_cast<Producer*>(a->private_data)->releases++;
        a->release = nullptr;
    }
    void build() {
        schema.format = "f"; schema.name = "";
        schema.release = &Producer::relS; schema.private_data = this;
        bufs[1] = vals;
        array.length = 4; array.n_buffers = 2; array.buffers = bufs;
        array.release = &Producer::relA; array.private_data = this;
    }
};

const char* kProgram =
    "package test;\n"
    "import cajeta.math.Tensor;\n"
    "import cajeta.nucleo.column.Column;\n"
    "public final class T {\n"
    "    static Tensor<float32> held;\n"
    "    public static int64 exportTensor() {\n"
    "        float32[] fa = [ 2.5f, 3.5f, 4.5f ];\n"
    "        int64[] s = heap int64[1]; s[0] = 3;\n"
    "        Tensor<float32> t = Tensor.of<float32>(fa, s);\n"
    "        T.held = t;\n"
    "        return T.held.exportArrow();\n"
    "    }\n"
    "    public static int64 heldAddr() { return T.held.hostAddress(); }\n"
    "    public static void pokeHeld(int64 i, float32 v) { T.held.set1(i, v); }\n"
    "    public static int64 protocolStillWorks() {\n"
    "        return (int64) T.held.protocol().ndim();\n"
    "    }\n"
    "    public static float32 importTensor(int64 s, int64 a) {\n"
    "        Tensor<?> w = Column.importAsTensor(s, a);\n"
    "        if (!(w instanceof Tensor<float32>)) { return -1.0f; }\n"
    "        Tensor<float32> t = (Tensor<float32>) w;\n"
    "        return t.get1(0) + t.get1(3) * 10.0f;\n"
    "    }\n"
    "}\n";
} // namespace

// 7.2 — tensor export is zero-copy over the LIVE buffer, additive beside
// protocol() (which keeps working on the same tensor).
TEST(ArrowTensor, tensorExportZeroCopyBesideProtocol) {
    auto jit = CajetaJit::compile(kProgram, "test.T");
    int64_t h = jit->lookup<int64_t (*)()>("exportTensor")();
    ASSERT_NE(h, 0);
    auto* b = reinterpret_cast<ExportBundleHead*>(h);
    EXPECT_STREQ(b->schema.format, "f");
    EXPECT_EQ(b->array.length, 3);
    EXPECT_EQ(reinterpret_cast<int64_t>(b->array.buffers[1]),
              jit->lookup<int64_t (*)()>("heldAddr")());
    jit->lookup<void (*)(int64_t, float)>("pokeHeld")(2, 55.0f);
    EXPECT_FLOAT_EQ(reinterpret_cast<const float*>(b->array.buffers[1])[2],
                    55.0f);
    EXPECT_EQ(jit->lookup<int64_t (*)()>("protocolStillWorks")(), 1)
        << "the retrofit must be additive - protocol() unchanged";
}

// 7.3 (narrowed) — importAsTensor: one explicit copy; the producer is
// released (transient borrow dropped) and post-import pokes are invisible.
TEST(ArrowTensor, importAsTensorMaterializes) {
    auto jit = CajetaJit::compile(kProgram, "test.T");
    auto fn = jit->lookup<float (*)(int64_t, int64_t)>("importTensor");
    Producer p;
    p.build();
    float v = fn(reinterpret_cast<int64_t>(&p.schema),
                 reinterpret_cast<int64_t>(&p.array));
    EXPECT_FLOAT_EQ(v, 7.0f + 100.0f);
    EXPECT_EQ(p.releases, 2)
        << "the transient foreign borrow must release the producer";
}
