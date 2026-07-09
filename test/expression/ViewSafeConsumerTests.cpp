// Slices plan 10.3.1 (+ 6.1.4 intrinsic audit): raw `.bytes` consumers outside
// io.net must be view-safe. A mode-2 windowed String keeps `bytes` = ROOT base
// with the window offset in ssoCount, so any consumer reading `bytes` raw
// silently operates on the root's prefix instead of the window. Each test
// feeds a real mode-2 view (heap root > SSO cap, so substring shares — never
// materializes) to one consumer family and asserts window semantics.
//
// Roots are built by concatenating two literals to > 24 bytes (past the SSO
// cap) so the concat mallocs and substring produces a genuine shared view.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <memory>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// One module holding every test body as its own static entry (shared-compile
// fixture — one JIT compile for the whole suite). Every run_* returns 0 on
// success and a distinct nonzero code per failing sub-check. The common
// preamble builds a 36-byte heap root and windows "klmnop" at offset 10.
const std::string MODULE_SRC =
    "package test;\n"
    "import cajeta.hash.XXHash3;\n"
    "import cajeta.hash.Sha256;\n"
    "import cajeta.codec.Base64;\n"
    "import cajeta.codec.json.Json;\n"
    "import cajeta.codec.json.JsonValue;\n"
    "import cajeta.codec.json.JsonWriter;\n"
    "import cajeta.io.file.File;\n"
    "import cajeta.io.file.FileReader;\n"
    "import cajeta.io.file.FileWriter;\n"
    "import cajeta.io.file.OpenMode;\n"
    "import cajeta.io.file.Path;\n"
    "import cajeta.reflect.Class;\n"
    "\n"
    "@ToString public class Held {\n"
    "    public String name;\n"
    "    public Held() { this.name = \"\"; }\n"
    "}\n"
    "\n"
    "public final class D {\n"

    // XXHash3 String overload sees the window, not the root prefix.
    "    public static int32 run_xxhashViewParity() {\n"
    "        String a = \"abcdefghijklmnopqrstuvwxyz\";\n"
    "        String b = \"0123456789\";\n"
    "        String s = a + b;\n"
    "        String v = s.substring(10, 16);\n"
    "        if (!v.equals(\"klmnop\")) { return 90; }\n"
    "        if (XXHash3.hashString(v) != XXHash3.hashString(\"klmnop\")) { return 1; }\n"
    "        if (XXHash3.hashStringSeeded(v, (int64) 7)\n"
    "                != XXHash3.hashStringSeeded(\"klmnop\", (int64) 7)) { return 2; }\n"
    "        return 0;\n"
    "    }\n"

    // Sha256 String overloads (oneshot + hex) window-correct.
    "    public static int32 run_sha256ViewParity() {\n"
    "        String a = \"abcdefghijklmnopqrstuvwxyz\";\n"
    "        String b = \"0123456789\";\n"
    "        String s = a + b;\n"
    "        String v = s.substring(10, 16);\n"
    "        String hv = Sha256.hashStringHex(v);\n"
    "        String hk = Sha256.hashStringHex(\"klmnop\");\n"
    "        if (!hv.equals(hk)) { return 1; }\n"
    "        return 0;\n"
    "    }\n"

    // StringBuilder.append(String) — unspilled (small[]) path.
    "    public static int32 run_sbAppendViewSmall() {\n"
    "        String a = \"abcdefghijklmnopqrstuvwxyz\";\n"
    "        String b = \"0123456789\";\n"
    "        String s = a + b;\n"
    "        String v = s.substring(10, 16);\n"
    "        StringBuilder sb = heap StringBuilder();\n"
    "        sb.append(v);\n"
    "        String out = sb.toString();\n"
    "        if (!out.equals(\"klmnop\")) { return 1; }\n"
    "        return 0;\n"
    "    }\n"

    // StringBuilder.append(String) — spilled (large[] blit) path.
    "    public static int32 run_sbAppendViewSpilled() {\n"
    "        String a = \"abcdefghijklmnopqrstuvwxyz\";\n"
    "        String b = \"0123456789\";\n"
    "        String s = a + b;\n"
    "        String v = s.substring(10, 16);\n"
    "        StringBuilder sb = heap StringBuilder();\n"
    "        int32 i = 0;\n"
    "        while (i < 8) { sb.append(\"12345678\"); i = i + 1; }\n"  // 64B: spilled
    "        sb.append(v);\n"
    "        String out = sb.toString();\n"
    "        if (out.size() != 70) { return 1; }\n"
    "        if (!out.endsWith(\"klmnop\")) { return 2; }\n"
    "        return 0;\n"
    "    }\n"

    // JsonWriter.writeString(String)/key(String) quote the window.
    "    public static int32 run_jsonWriterView() {\n"
    "        String a = \"abcdefghijklmnopqrstuvwxyz\";\n"
    "        String b = \"0123456789\";\n"
    "        String s = a + b;\n"
    "        String v = s.substring(10, 16);\n"
    "        JsonWriter w = heap JsonWriter();\n"
    "        w.writeString(v);\n"
    "        int8[] outBytes = w.toBytes();\n"
    "        int32 n = (int32) outBytes.count();\n"
    "        String got = heap String(#outBytes, n);\n"
    "        if (!got.equals(\"\\\"klmnop\\\"\")) { return 1; }\n"
    "        JsonWriter w2 = heap JsonWriter();\n"
    "        w2.beginObject();\n"
    "        w2.key(v);\n"
    "        w2.writeNumber((int64) 3);\n"
    "        w2.endObject();\n"
    "        int8[] out2 = w2.toBytes();\n"
    "        int32 n2 = (int32) out2.count();\n"
    "        String got2 = heap String(#out2, n2);\n"
    "        if (!got2.equals(\"{\\\"klmnop\\\":3}\")) { return 2; }\n"
    "        return 0;\n"
    "    }\n"

    // Json.parse(String) parses the window ("123"), not the root prefix ("456").
    "    public static int32 run_jsonParseView() {\n"
    "        String a = \"45600000000000000000000000000\";\n"
    "        String b = \"123\";\n"
    "        String s = a + b;\n"
    "        String v = s.substring(29, 32);\n"
    "        if (!v.equals(\"123\")) { return 90; }\n"
    "        JsonValue jv = Json.parse(v);\n"
    "        if (jv.asInt64() != (int64) 123) { return 1; }\n"
    "        return 0;\n"
    "    }\n"

    // Base64.decode(String) decodes the window.
    "    public static int32 run_base64ViewDecode() {\n"
    "        String a = \"abcdefghijklmnopqrstuvwxyz\";\n"
    "        String b = \"aGVsbG8=00\";\n"
    "        String s = a + b;\n"
    "        String v = s.substring(26, 34);\n"
    "        if (!v.equals(\"aGVsbG8=\")) { return 90; }\n"
    "        int8[] out = Base64.decode(v);\n"
    "        int32 n = (int32) out.count();\n"
    "        if (n != 5) { return 1; }\n"
    "        if (out[0] != (int8) 104) { return 2; }\n"   // 'h'
    "        if (out[4] != (int8) 111) { return 3; }\n"   // 'o'
    "        return 0;\n"
    "    }\n"

    // Class.forName(String) — a view's length is byteLength, not the root
    // array's count word (offset-0 view), and the window offset applies.
    "    public static int32 run_classForNameView() {\n"
    "        String a = \"cajeta.lang.\";\n"
    "        String b = \"StringZZZZZZZZZZZZZZZ\";\n"
    "        String s = a + b;\n"
    "        String cn = s.substring(0, 18);\n"
    "        if (!cn.equals(\"cajeta.lang.String\")) { return 90; }\n"
    "        if (!Class.forName(cn).isPresent()) { return 1; }\n"
    "        String p = \"XX\";\n"
    "        String q = \"cajeta.lang.StringZZZZZZZ\";\n"
    "        String s2 = p + q;\n"
    "        String cn2 = s2.substring(2, 20);\n"
    "        if (!Class.forName(cn2).isPresent()) { return 2; }\n"
    "        return 0;\n"
    "    }\n"

    // @ToString synthesized toString on a String field holding a view (moved
    // in with `#` so field-store escape resolution can't materialize it).
    "    public static int32 run_toStringViewField() {\n"
    "        String a = \"abcdefghijklmnopqrstuvwxyz\";\n"
    "        String b = \"0123456789\";\n"
    "        String s = a + b;\n"
    "        String v = s.substring(10, 16);\n"
    "        Held h = heap Held();\n"
    "        h.name = #v;\n"
    "        String out = h.toString();\n"
    "        if (!out.contains(\"klmnop\")) { return 1; }\n"
    "        return 0;\n"
    "    }\n"

    // FileWriter.writeString intrinsic writes the window bytes.
    "    public static int32 run_fileWriterView() {\n"
    "        String a = \"abcdefghijklmnopqrstuvwxyz\";\n"
    "        String b = \"0123456789\";\n"
    "        String s = a + b;\n"
    "        String v = s.substring(10, 16);\n"
    "        String path = \"/tmp/caj_viewsafe_fw.txt\";\n"
    "        FileWriter w = File.openWrite(path, OpenMode.WRITE);\n"
    "        w.writeString(v);\n"
    "        w.close();\n"
    "        FileReader r = File.openRead(path);\n"
    "        String got = r.readString(64);\n"
    "        r.close();\n"
    "        if (got.size() != 6) { return 1; }\n"
    "        if (!got.equals(\"klmnop\")) { return 2; }\n"
    "        return 0;\n"
    "    }\n"

    // Path.of(String) copies the window, not the root prefix.
    "    public static int32 run_pathOfView() {\n"
    "        String a = \"XXXXXXXXXXXXXXXXXXXXXXXXXX\";\n"
    "        String b = \"/tmp/abc\";\n"
    "        String s = a + b;\n"
    "        String v = s.substring(26, 34);\n"
    "        if (!v.equals(\"/tmp/abc\")) { return 90; }\n"
    "        Path p = Path.of(v);\n"
    "        if (!p.isAbsolute()) { return 1; }\n"
    "        return 0;\n"
    "    }\n"

    "}\n";

class ViewSafeConsumerTests : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        jit = CajetaJit::compile(MODULE_SRC, "test.D");
    }
    static void TearDownTestSuite() {
        jit.reset();
    }
    static int32_t i32(const char* name) {
        auto fn = jit->lookup<int32_t (*)()>(name);
        return fn();
    }
    static std::unique_ptr<CajetaJit> jit;
};

std::unique_ptr<CajetaJit> ViewSafeConsumerTests::jit;

}  // namespace

TEST_F(ViewSafeConsumerTests, xxhashViewParity)    { EXPECT_EQ(i32("run_xxhashViewParity"), 0); }
TEST_F(ViewSafeConsumerTests, sha256ViewParity)    { EXPECT_EQ(i32("run_sha256ViewParity"), 0); }
TEST_F(ViewSafeConsumerTests, sbAppendViewSmall)   { EXPECT_EQ(i32("run_sbAppendViewSmall"), 0); }
TEST_F(ViewSafeConsumerTests, sbAppendViewSpilled) { EXPECT_EQ(i32("run_sbAppendViewSpilled"), 0); }
TEST_F(ViewSafeConsumerTests, jsonWriterView)      { EXPECT_EQ(i32("run_jsonWriterView"), 0); }
TEST_F(ViewSafeConsumerTests, jsonParseView)       { EXPECT_EQ(i32("run_jsonParseView"), 0); }
TEST_F(ViewSafeConsumerTests, base64ViewDecode)    { EXPECT_EQ(i32("run_base64ViewDecode"), 0); }
TEST_F(ViewSafeConsumerTests, classForNameView)    { EXPECT_EQ(i32("run_classForNameView"), 0); }
TEST_F(ViewSafeConsumerTests, toStringViewField)   { EXPECT_EQ(i32("run_toStringViewField"), 0); }
TEST_F(ViewSafeConsumerTests, fileWriterView)      { EXPECT_EQ(i32("run_fileWriterView"), 0); }
TEST_F(ViewSafeConsumerTests, pathOfView)          { EXPECT_EQ(i32("run_pathOfView"), 0); }
