// Tier-1 codec synthesizers — Avro / Ion / Protobuf typed-bind round-trips.
//
// The `dev.cajeta.codec.*` facades live in the standalone codec library, not
// the stdlib, so each test compiles a fixture snapshot of that package
// (test/codec/fixtures/dev/cajeta/codec/<fmt>/) alongside the test unit via
// the multi-source JIT overload. The snapshot pins the exact class surface the
// synthesizers in src/cajeta/codec/ emit calls against — if a synthesizer's
// generated body drifts from these classes, these tests break first.
//
// Every test is a toBytes<T> → parse<T> round-trip: one pass covers both the
// encode and decode emit paths without hand-authored wire bytes. Failures
// return a distinct nonzero code per check; 0 is a clean round-trip.

#include <gtest/gtest.h>
#include "../CajetaUnitTest.h"
#include "../jit/JitTestHelper.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

using cajeta_test::CajetaJit;

namespace {

namespace fs = std::filesystem;

// Load every .cajeta file of one fixture package into a (fqClassName → source)
// map. Cached per package — the files never change within a test run.
const std::map<std::string, std::string>& fixturePackage(const std::string& fmt) {
    static std::map<std::string, std::map<std::string, std::string>> cache;
    auto it = cache.find(fmt);
    if (it != cache.end()) return it->second;

    std::map<std::string, std::string> sources;
    fs::path dir = fs::path(CAJETA_TEST_ROOT) / "codec" / "fixtures"
                 / "dev" / "cajeta" / "codec" / fmt;
    for (auto& e : fs::directory_iterator(dir)) {
        if (e.path().extension() != ".cajeta") continue;
        std::ifstream in(e.path());
        std::stringstream ss;
        ss << in.rdbuf();
        sources["dev.cajeta.codec." + fmt + "." + e.path().stem().string()]
            = ss.str();
    }
    cache[fmt] = std::move(sources);
    return cache[fmt];
}

int32_t runCodec(const std::string& fmt, const std::string& testSrc) {
    auto sources = fixturePackage(fmt);
    sources["test.D"] = testSrc;
    auto jit = CajetaJit::compile(sources, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// ---------------------------------------------------------------- Avro ----

// One record carrying every decode kind the Avro synthesizer classifies:
// varint longs (int32 cast + int64 direct), boolean, String, bytes, and a
// nested record (positional inline recurse). Encode → decode → field checks.
TEST(AvroSynthesizerTests, recordRoundTripAllFieldKinds) {
    auto src =
        "package test;\n"
        "import dev.cajeta.codec.avro.Avro;\n"
        "public class Sub { public int32 x; }\n"
        "public class Rec {\n"
        "    public int32 a;\n"
        "    public int64 b;\n"
        "    public boolean c;\n"
        "    public String s;\n"
        "    public int8[] blob;\n"
        "    public Sub child;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Rec r = heap Rec();\n"
        "        r.a = 100000;\n"
        "        r.b = (int64) 1234567890123;\n"
        "        r.c = true;\n"
        "        String nm = \"hello\";\n"
        "        r.s = nm;\n"
        "        int8[] bb = heap int8[3];\n"
        "        bb[0] = (int8) 1; bb[1] = (int8) 2; bb[2] = (int8) 9;\n"
        "        r.blob = bb;\n"
        "        Sub sub = heap Sub();\n"
        "        sub.x = 7;\n"
        "        r.child = sub;\n"
        "        int8[] wire = Avro.toBytes<Rec>(r);\n"
        "        int64 n = (int64) wire.count();\n"
        "        Rec q = Avro.parse<Rec>(wire, n);\n"
        "        if (q.a != 100000) { return 1; }\n"
        "        if (q.b != (int64) 1234567890123) { return 2; }\n"
        "        if (!q.c) { return 3; }\n"
        "        if (!q.s.equals(\"hello\")) { return 4; }\n"
        "        if ((int32) q.blob.count() != 3) { return 5; }\n"
        "        if (q.blob[2] != (int8) 9) { return 6; }\n"
        "        if (q.child.x != 7) { return 7; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runCodec("avro", src), 0);
}

// T[] routes through the Object Container File writer/reader (header with an
// emitted JSON schema, blocks, sync markers) instead of the single-datum path.
TEST(AvroSynthesizerTests, containerFileRoundTrip) {
    auto src =
        "package test;\n"
        "import dev.cajeta.codec.avro.Avro;\n"
        "public class Rec {\n"
        "    public int32 a;\n"
        "    public String s;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Rec r0 = heap Rec();\n"
        "        r0.a = 11;\n"
        "        String s0 = \"first\";\n"
        "        r0.s = s0;\n"
        "        Rec r1 = heap Rec();\n"
        "        r1.a = 22;\n"
        "        String s1 = \"second\";\n"
        "        r1.s = s1;\n"
        "        Rec[] arr = heap Rec[2];\n"
        "        arr[0] = r0;\n"
        "        arr[1] = r1;\n"
        "        int8[] wire = Avro.toBytes<Rec[]>(arr);\n"
        "        int64 n = (int64) wire.count();\n"
        "        Rec[] got = Avro.parse<Rec[]>(wire, n);\n"
        "        if ((int32) got.count() != 2) { return 1; }\n"
        "        if (got[0].a != 11) { return 2; }\n"
        "        if (!got[0].s.equals(\"first\")) { return 3; }\n"
        "        if (got[1].a != 22) { return 4; }\n"
        "        if (!got[1].s.equals(\"second\")) { return 5; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runCodec("avro", src), 0);
}

// ----------------------------------------------------------------- Ion ----

// Ion binds by NAME against the self-describing struct (BVM + local symbol
// table + struct). Same field-kind matrix as Avro: ints, boolean, String,
// bytes, nested struct.
TEST(IonSynthesizerTests, structRoundTripAllFieldKinds) {
    auto src =
        "package test;\n"
        "import dev.cajeta.codec.ion.Ion;\n"
        "public class Sub { public int32 x; }\n"
        "public class Rec {\n"
        "    public int32 a;\n"
        "    public int64 b;\n"
        "    public boolean c;\n"
        "    public String s;\n"
        "    public int8[] blob;\n"
        "    public Sub child;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Rec r = heap Rec();\n"
        "        r.a = 100000;\n"
        "        r.b = (int64) 1234567890123;\n"
        "        r.c = true;\n"
        "        String nm = \"hello\";\n"
        "        r.s = nm;\n"
        "        int8[] bb = heap int8[3];\n"
        "        bb[0] = (int8) 1; bb[1] = (int8) 2; bb[2] = (int8) 9;\n"
        "        r.blob = bb;\n"
        "        Sub sub = heap Sub();\n"
        "        sub.x = 7;\n"
        "        r.child = sub;\n"
        "        int8[] wire = Ion.toBytes<Rec>(r);\n"
        "        int64 n = (int64) wire.count();\n"
        "        Rec q = Ion.parse<Rec>(wire, n);\n"
        "        if (q.a != 100000) { return 1; }\n"
        "        if (q.b != (int64) 1234567890123) { return 2; }\n"
        "        if (!q.c) { return 3; }\n"
        "        if (!q.s.equals(\"hello\")) { return 4; }\n"
        "        if ((int32) q.blob.count() != 3) { return 5; }\n"
        "        if (q.blob[2] != (int8) 9) { return 6; }\n"
        "        if (q.child.x != 7) { return 7; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runCodec("ion", src), 0);
}

// T[] is a top-level Ion value stream: consecutive structs after the BVM/LST.
TEST(IonSynthesizerTests, topLevelStreamRoundTrip) {
    auto src =
        "package test;\n"
        "import dev.cajeta.codec.ion.Ion;\n"
        "public class Rec {\n"
        "    public int32 a;\n"
        "    public String s;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Rec r0 = heap Rec();\n"
        "        r0.a = 11;\n"
        "        String s0 = \"first\";\n"
        "        r0.s = s0;\n"
        "        Rec r1 = heap Rec();\n"
        "        r1.a = 22;\n"
        "        String s1 = \"second\";\n"
        "        r1.s = s1;\n"
        "        Rec[] arr = heap Rec[2];\n"
        "        arr[0] = r0;\n"
        "        arr[1] = r1;\n"
        "        int8[] wire = Ion.toBytes<Rec[]>(arr);\n"
        "        int64 n = (int64) wire.count();\n"
        "        Rec[] got = Ion.parse<Rec[]>(wire, n);\n"
        "        if ((int32) got.count() != 2) { return 1; }\n"
        "        if (got[0].a != 11) { return 2; }\n"
        "        if (!got[0].s.equals(\"first\")) { return 3; }\n"
        "        if (got[1].a != 22) { return 4; }\n"
        "        if (!got[1].s.equals(\"second\")) { return 5; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runCodec("ion", src), 0);
}

// ------------------------------------------------------------ Protobuf ----

// Scalar matrix: VARINT ints + bool, LEN String/bytes/nested message, I32
// float32, I64 float64. Values chosen exactly representable so equality is
// exact after the round-trip.
TEST(ProtobufSynthesizerTests, messageRoundTripAllFieldKinds) {
    auto src =
        "package test;\n"
        "import dev.cajeta.codec.protobuf.Protobuf;\n"
        "import dev.cajeta.codec.protobuf.ProtoField;\n"
        "public class Sub {\n"
        "    @ProtoField(1) public int32 x;\n"
        "}\n"
        "public class Msg {\n"
        "    @ProtoField(1) public int32 a;\n"
        "    @ProtoField(2) public int64 b;\n"
        "    @ProtoField(3) public boolean c;\n"
        "    @ProtoField(4) public String s;\n"
        "    @ProtoField(5) public int8[] blob;\n"
        "    @ProtoField(6) public Sub child;\n"
        "    @ProtoField(7) public float32 f;\n"
        "    @ProtoField(8) public float64 g;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Msg m = heap Msg();\n"
        "        m.a = 100000;\n"
        "        m.b = (int64) 1234567890123;\n"
        "        m.c = true;\n"
        "        String nm = \"hello\";\n"
        "        m.s = nm;\n"
        "        int8[] bb = heap int8[3];\n"
        "        bb[0] = (int8) 1; bb[1] = (int8) 2; bb[2] = (int8) 9;\n"
        "        m.blob = bb;\n"
        "        Sub sub = heap Sub();\n"
        "        sub.x = 7;\n"
        "        m.child = sub;\n"
        "        m.f = (float32) 1.5;\n"
        "        m.g = -2.25;\n"
        "        int8[] wire = Protobuf.toBytes<Msg>(m);\n"
        "        int64 n = (int64) wire.count();\n"
        "        Msg q = Protobuf.parse<Msg>(wire, n);\n"
        "        if (q.a != 100000) { return 1; }\n"
        "        if (q.b != (int64) 1234567890123) { return 2; }\n"
        "        if (!q.c) { return 3; }\n"
        "        if (!q.s.equals(\"hello\")) { return 4; }\n"
        "        if ((int32) q.blob.count() != 3) { return 5; }\n"
        "        if (q.blob[2] != (int8) 9) { return 6; }\n"
        "        if (q.child.x != 7) { return 7; }\n"
        "        if (q.f != (float32) 1.5) { return 8; }\n"
        "        if (q.g != -2.25) { return 9; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runCodec("protobuf", src), 0);
}

// The `encoding` option: zigzag (sint32/sint64 — negatives stay short) and
// fixed (fixed32/fixed64 — constant width). Both directions of each.
TEST(ProtobufSynthesizerTests, zigzagAndFixedEncodingsRoundTrip) {
    auto src =
        "package test;\n"
        "import dev.cajeta.codec.protobuf.Protobuf;\n"
        "import dev.cajeta.codec.protobuf.ProtoField;\n"
        "public class Msg {\n"
        "    @ProtoField(value = 1, encoding = \"zigzag\") public int32 d1;\n"
        "    @ProtoField(value = 2, encoding = \"zigzag\") public int64 d2;\n"
        "    @ProtoField(value = 3, encoding = \"fixed\")  public int32 e1;\n"
        "    @ProtoField(value = 4, encoding = \"fixed\")  public int64 e2;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Msg m = heap Msg();\n"
        "        m.d1 = -1;\n"
        "        m.d2 = (int64) -123456789;\n"
        "        m.e1 = 305419896;\n"
        "        m.e2 = (int64) 1234567890123;\n"
        "        int8[] wire = Protobuf.toBytes<Msg>(m);\n"
        "        int64 n = (int64) wire.count();\n"
        "        Msg q = Protobuf.parse<Msg>(wire, n);\n"
        "        if (q.d1 != -1) { return 1; }\n"
        "        if (q.d2 != (int64) -123456789) { return 2; }\n"
        "        if (q.e1 != 305419896) { return 3; }\n"
        "        if (q.e2 != (int64) 1234567890123) { return 4; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runCodec("protobuf", src), 0);
}

// Repeated fields: packed numeric (default), expanded numeric
// (packed = false), repeated String, repeated nested message.
TEST(ProtobufSynthesizerTests, repeatedFieldsRoundTrip) {
    auto src =
        "package test;\n"
        "import dev.cajeta.codec.protobuf.Protobuf;\n"
        "import dev.cajeta.codec.protobuf.ProtoField;\n"
        "public class Sub {\n"
        "    @ProtoField(1) public int32 x;\n"
        "}\n"
        "public class Msg {\n"
        "    @ProtoField(1)                         public int64[] scores;\n"
        "    @ProtoField(value = 2, packed = false) public int32[] counts;\n"
        "    @ProtoField(3)                         public String[] tags;\n"
        "    @ProtoField(4)                         public Sub[] items;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Msg m = heap Msg();\n"
        "        int64[] sc = heap int64[3];\n"
        "        sc[0] = (int64) 1; sc[1] = (int64) 2; sc[2] = (int64) 300;\n"
        "        m.scores = sc;\n"
        "        int32[] ct = heap int32[2];\n"
        "        ct[0] = 4; ct[1] = 5;\n"
        "        m.counts = ct;\n"
        "        String[] tg = heap String[2];\n"
        "        String t0 = \"a\";\n"
        "        String t1 = \"bb\";\n"
        "        tg[0] = t0;\n"
        "        tg[1] = t1;\n"
        "        m.tags = tg;\n"
        "        Sub i0 = heap Sub();\n"
        "        i0.x = 10;\n"
        "        Sub i1 = heap Sub();\n"
        "        i1.x = 20;\n"
        "        Sub[] it = heap Sub[2];\n"
        "        it[0] = i0;\n"
        "        it[1] = i1;\n"
        "        m.items = it;\n"
        "        int8[] wire = Protobuf.toBytes<Msg>(m);\n"
        "        int64 n = (int64) wire.count();\n"
        "        Msg q = Protobuf.parse<Msg>(wire, n);\n"
        "        if ((int32) q.scores.count() != 3) { return 1; }\n"
        "        if (q.scores[2] != (int64) 300) { return 2; }\n"
        "        if ((int32) q.counts.count() != 2) { return 3; }\n"
        "        if (q.counts[1] != 5) { return 4; }\n"
        "        if ((int32) q.tags.count() != 2) { return 5; }\n"
        "        if (!q.tags[1].equals(\"bb\")) { return 6; }\n"
        "        if ((int32) q.items.count() != 2) { return 7; }\n"
        "        if (q.items[1].x != 20) { return 8; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runCodec("protobuf", src), 0);
}

// T[] at top level: a length-delimited message stream.
TEST(ProtobufSynthesizerTests, lengthDelimitedStreamRoundTrip) {
    auto src =
        "package test;\n"
        "import dev.cajeta.codec.protobuf.Protobuf;\n"
        "import dev.cajeta.codec.protobuf.ProtoField;\n"
        "public class Msg {\n"
        "    @ProtoField(1) public int32 a;\n"
        "    @ProtoField(2) public String s;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Msg m0 = heap Msg();\n"
        "        m0.a = 11;\n"
        "        String s0 = \"first\";\n"
        "        m0.s = s0;\n"
        "        Msg m1 = heap Msg();\n"
        "        m1.a = 22;\n"
        "        String s1 = \"second\";\n"
        "        m1.s = s1;\n"
        "        Msg[] arr = heap Msg[2];\n"
        "        arr[0] = m0;\n"
        "        arr[1] = m1;\n"
        "        int8[] wire = Protobuf.toBytes<Msg[]>(arr);\n"
        "        int64 n = (int64) wire.count();\n"
        "        Msg[] got = Protobuf.parse<Msg[]>(wire, n);\n"
        "        if ((int32) got.count() != 2) { return 1; }\n"
        "        if (got[0].a != 11) { return 2; }\n"
        "        if (!got[0].s.equals(\"first\")) { return 3; }\n"
        "        if (got[1].a != 22) { return 4; }\n"
        "        if (!got[1].s.equals(\"second\")) { return 5; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runCodec("protobuf", src), 0);
}
