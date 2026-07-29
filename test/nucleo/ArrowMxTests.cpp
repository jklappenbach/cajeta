//
// nucleo-column Unit 7 — MX extension types (spec 5.1-5.3), v1 mechanism:
// cajeta.mxfp4 = a LOGICAL view over packed uint8 physical bytes, the
// extension name + block size riding the schema's key-value metadata blob.
// Bytes move even where the semantics are unknown (5.2).
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

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

// Parse the C-Data metadata blob into (key, value) pairs.
std::vector<std::pair<std::string, std::string>> parseMeta(const char* m) {
    std::vector<std::pair<std::string, std::string>> out;
    if (!m) return out;
    int32_t pairs;
    std::memcpy(&pairs, m, 4);
    const char* p = m + 4;
    for (int32_t k = 0; k < pairs; ++k) {
        int32_t kl; std::memcpy(&kl, p, 4); p += 4;
        std::string key(p, p + kl); p += kl;
        int32_t vl; std::memcpy(&vl, p, 4); p += 4;
        std::string val(p, p + vl); p += vl;
        out.emplace_back(key, val);
    }
    return out;
}

// A producer of an extension-tagged uint8 array.
struct ExtProducer {
    ArrowSchemaAbi schema{};
    ArrowArrayAbi array{};
    const void* bufs[2] = {nullptr, nullptr};
    uint8_t bytes[4] = {0x12, 0x34, 0x56, 0x78};
    std::string meta;
    int releases = 0;
    static void relS(ArrowSchemaAbi* s) {
        static_cast<ExtProducer*>(s->private_data)->releases++;
        s->release = nullptr;
    }
    static void relA(ArrowArrayAbi* a) {
        static_cast<ExtProducer*>(a->private_data)->releases++;
        a->release = nullptr;
    }
    void build(const char* extName, const char* extMeta) {
        auto put = [&](const std::string& k, const std::string& v) {
            int32_t n = (int32_t) k.size();
            meta.append(reinterpret_cast<const char*>(&n), 4);
            meta += k;
            n = (int32_t) v.size();
            meta.append(reinterpret_cast<const char*>(&n), 4);
            meta += v;
        };
        int32_t pairs = 2;
        meta.append(reinterpret_cast<const char*>(&pairs), 4);
        put("ARROW:extension:name", extName);
        put("ARROW:extension:metadata", extMeta);
        schema.format = "C"; schema.name = ""; schema.metadata = meta.data();
        schema.release = &ExtProducer::relS; schema.private_data = this;
        bufs[1] = bytes;
        array.length = 4; array.n_buffers = 2; array.buffers = bufs;
        array.release = &ExtProducer::relA; array.private_data = this;
    }
};

const char* kProgram =
    "package test;\n"
    "import cajeta.nucleo.column.Column;\n"
    "import cajeta.nucleo.column.MxColumn;\n"
    "import cajeta.nucleo.column.ColumnTypeException;\n"
    "public final class T {\n"
    "    static MxColumn held;\n"
    "    public static int64 build() {\n"
    "        Column<uint8> p = Column.zeros<uint8>(4);\n"
    "        p.set(0, (uint8) 18);\n"
    "        p.set(1, (uint8) 52);\n"
    "        p.set(2, (uint8) 86);\n"
    "        p.set(3, (uint8) 120);\n"
    "        int64 physAddr = p.dataAddress();\n"
    "        MxColumn m = MxColumn.mxfp4(#p, 32);\n"
    "        T.held = m;\n"
    "        return physAddr - T.held.dataAddress();\n"  // 0 = view, no re-encode
    "    }\n"
    "    public static int64 exportHeld() { return T.held.exportArrow(); }\n"
    "    public static int64 heldAddr() { return T.held.dataAddress(); }\n"
    "    public static int64 importMx(int64 s, int64 a) {\n"
    "        MxColumn m = MxColumn.importArrow(s, a);\n"
    "        return (int64) m.block() * 1000 + (int64) m.byteAt(0);\n"
    "    }\n"
    "    public static int64 importMxRejected(int64 s, int64 a) {\n"
    "        try {\n"
    "            MxColumn m = MxColumn.importArrow(s, a);\n"
    "            return 0;\n"
    "        } catch (ColumnTypeException e) {\n"
    "            return 1;\n"
    "        }\n"
    "    }\n"
    "    public static int64 importPhysical(int64 s, int64 a) {\n"
    "        Column<?> w = Column.importArrow(s, a);\n"
    "        if (!(w instanceof Column<uint8>)) { return -1; }\n"
    "        Column<uint8> c = (Column<uint8>) w;\n"
    "        return (int64) c.get(1);\n"
    "    }\n"
    "}\n";
} // namespace

// 5.1 — the logical view shares the physical buffer (no re-encode), and the
// export carries the extension name + block metadata while a metadata-blind
// consumer still reads the physical bytes (5.2).
TEST(ArrowMx, viewSharesBytesAndExportCarriesExtension) {
    auto jit = CajetaJit::compile(kProgram, "test.T");
    ASSERT_EQ(jit->lookup<int64_t (*)()>("build")(), 0)
        << "mxfp4 wrap re-encoded or copied the physical bytes";
    int64_t h = jit->lookup<int64_t (*)()>("exportHeld")();
    auto* b = reinterpret_cast<ExportBundleHead*>(h);
    EXPECT_STREQ(b->schema.format, "C");
    auto pairs = parseMeta(b->schema.metadata);
    ASSERT_EQ(pairs.size(), 2u);
    EXPECT_EQ(pairs[0].first, "ARROW:extension:name");
    EXPECT_EQ(pairs[0].second, "cajeta.mxfp4");
    EXPECT_EQ(pairs[1].first, "ARROW:extension:metadata");
    EXPECT_EQ(pairs[1].second, "{\"block\":32}");
    EXPECT_EQ(reinterpret_cast<int64_t>(b->array.buffers[1]),
              jit->lookup<int64_t (*)()>("heldAddr")());
    EXPECT_EQ(reinterpret_cast<const uint8_t*>(b->array.buffers[1])[1], 52)
        << "a consumer ignoring the extension must still read the bytes";
}

// 5.3 — a cajeta.mxfp4-tagged import reconstructs the logical column (block
// size + bytes); a DIFFERENT extension is a named refusal on the MX airlock
// but its PHYSICAL bytes import through Column.importArrow.
TEST(ArrowMx, importKnownAndUnknownExtensions) {
    auto jit = CajetaJit::compile(kProgram, "test.T");
    ExtProducer p;
    p.build("cajeta.mxfp4", "{\"block\":16}");
    EXPECT_EQ(jit->lookup<int64_t (*)(int64_t, int64_t)>("importMx")(
                  reinterpret_cast<int64_t>(&p.schema),
                  reinterpret_cast<int64_t>(&p.array)), 16 * 1000 + 0x12);
    EXPECT_EQ(p.releases, 2);
    ExtProducer q;
    q.build("someone.else", "{}");
    EXPECT_EQ(jit->lookup<int64_t (*)(int64_t, int64_t)>("importMxRejected")(
                  reinterpret_cast<int64_t>(&q.schema),
                  reinterpret_cast<int64_t>(&q.array)), 1);
    ExtProducer r;
    r.build("someone.else", "{}");
    EXPECT_EQ(jit->lookup<int64_t (*)(int64_t, int64_t)>("importPhysical")(
                  reinterpret_cast<int64_t>(&r.schema),
                  reinterpret_cast<int64_t>(&r.array)), 0x34)
        << "unknown extensions must still import physically (5.3)";
}
