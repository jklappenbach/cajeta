//
// nucleo-column Unit 5 — variable-length utf8 columns (spec 2.2): offsets
// (int32, len+1) over one contiguous utf8 data buffer, Arrow format "u",
// export/import through the same matched-struct seam as fixed-width.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <cstring>
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

// Producer of a foreign utf8 array {"ab", "cde"} with counting releases.
struct Utf8Producer {
    ArrowSchemaAbi schema{};
    ArrowArrayAbi array{};
    const void* bufs[3] = {nullptr, nullptr, nullptr};
    int32_t offsets[3] = {0, 2, 5};
    char bytes[5] = {'a', 'b', 'c', 'd', 'e'};
    int releases = 0;
    static void relS(ArrowSchemaAbi* s) {
        static_cast<Utf8Producer*>(s->private_data)->releases++;
        s->release = nullptr;
    }
    static void relA(ArrowArrayAbi* a) {
        static_cast<Utf8Producer*>(a->private_data)->releases++;
        a->release = nullptr;
    }
    void build() {
        schema.format = "u"; schema.name = "";
        schema.release = &Utf8Producer::relS; schema.private_data = this;
        bufs[1] = offsets; bufs[2] = bytes;
        array.length = 2; array.null_count = 0; array.n_buffers = 3;
        array.buffers = bufs;
        array.release = &Utf8Producer::relA; array.private_data = this;
    }
};

// get(i) probes are encoded as count*100 + first byte, so string content
// checks don't depend on String equality semantics.
const char* kProgram =
    "package test;\n"
    "import cajeta.lang.String;\n"
    "import cajeta.nucleo.column.StringColumn;\n"
    "public final class T {\n"
    "    static StringColumn held;\n"
    "    public static int64 build() {\n"
    "        String[] vs = [ \"hola\", \"x\", \"columnas\" ];\n"
    "        StringColumn c = StringColumn.of(vs);\n"
    "        T.held = c;\n"
    "        return T.held.size();\n"
    "    }\n"
    "    public static int64 probe(int64 i) {\n"
    "        String s = T.held.get(i);\n"
    "        return (int64) s.count() * 100 + (int64) s.byteAt(0);\n"
    "    }\n"
    "    public static int64 exportHeld() { return T.held.exportArrow(); }\n"
    "    public static int64 offsAddr() { return T.held.offsetsAddress(); }\n"
    "    public static int64 dataAddr2() { return T.held.dataAddress(); }\n"
    "    public static int64 importProbe(int64 s, int64 a, int64 i) {\n"
    "        StringColumn c = StringColumn.importArrow(s, a);\n"
    "        String v = c.get(i);\n"
    "        return (int64) v.count() * 100 + (int64) v.byteAt(0);\n"
    "    }\n"
    "}\n";
} // namespace

// 5.1.1 — of() lays out offsets/data per Arrow "u"; get() round-trips the
// strings ("hola"=4,'h'=104 -> 504; "x" -> 220... probe = count*100+byte0).
TEST(ArrowString, ofAndGetRoundTrip) {
    auto jit = CajetaJit::compile(kProgram, "test.T");
    ASSERT_EQ(jit->lookup<int64_t (*)()>("build")(), 3);
    auto probe = jit->lookup<int64_t (*)(int64_t)>("probe");
    EXPECT_EQ(probe(0), 4 * 100 + 'h');
    EXPECT_EQ(probe(1), 1 * 100 + 'x');
    EXPECT_EQ(probe(2), 8 * 100 + 'c');
}

// 5.1.2 + 5.1.3 — export: format "u", 3 buffers [null, offsets, data] aimed
// at the LIVE aligned buffers; offsets has len+1 entries ending at total
// bytes; the data bytes are the concatenation.
TEST(ArrowString, exportLayoutMatchesArrowU) {
    auto jit = CajetaJit::compile(kProgram, "test.T");
    ASSERT_EQ(jit->lookup<int64_t (*)()>("build")(), 3);
    int64_t h = jit->lookup<int64_t (*)()>("exportHeld")();
    auto* b = reinterpret_cast<ExportBundleHead*>(h);
    EXPECT_STREQ(b->schema.format, "u");
    EXPECT_EQ(b->array.length, 3);
    ASSERT_EQ(b->array.n_buffers, 3);
    EXPECT_EQ(b->array.buffers[0], nullptr);
    EXPECT_EQ(reinterpret_cast<int64_t>(b->array.buffers[1]),
              jit->lookup<int64_t (*)()>("offsAddr")());
    EXPECT_EQ(reinterpret_cast<int64_t>(b->array.buffers[2]),
              jit->lookup<int64_t (*)()>("dataAddr2")());
    EXPECT_EQ(reinterpret_cast<uintptr_t>(b->array.buffers[1]) % 64, 0u);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(b->array.buffers[2]) % 64, 0u);
    auto* offs = reinterpret_cast<const int32_t*>(b->array.buffers[1]);
    EXPECT_EQ(offs[0], 0);
    EXPECT_EQ(offs[1], 4);
    EXPECT_EQ(offs[2], 5);
    EXPECT_EQ(offs[3], 13);
    EXPECT_EQ(std::memcmp(b->array.buffers[2], "holaxcolumnas", 13), 0);
}

// 5.1.2 (import half) — a foreign utf8 array imports zero-copy; strings read
// through the producer's buffers; releases fire exactly once at drop.
TEST(ArrowString, importReadsForeignUtf8) {
    auto jit = CajetaJit::compile(kProgram, "test.T");
    auto probe = jit->lookup<int64_t (*)(int64_t, int64_t, int64_t)>("importProbe");
    Utf8Producer p;
    p.build();
    EXPECT_EQ(probe(reinterpret_cast<int64_t>(&p.schema),
                    reinterpret_cast<int64_t>(&p.array), 0), 2 * 100 + 'a');
    EXPECT_EQ(p.releases, 2) << "both releases fire when the import drops";
    Utf8Producer q;
    q.build();
    q.bytes[2] = 'z';                        // prove reads hit THIS memory
    EXPECT_EQ(probe(reinterpret_cast<int64_t>(&q.schema),
                    reinterpret_cast<int64_t>(&q.array), 1), 3 * 100 + 'z');
}
