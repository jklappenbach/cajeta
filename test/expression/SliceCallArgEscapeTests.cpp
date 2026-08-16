// Slice views must survive their mode-0 source's drop (slice-spec §3, §7.1).
//
// Regression pinned here: __cajeta_string_drop's mode-0 branch freed the
// byte buffer unconditionally — it never consulted the shared registry, so
// when an escaping substring had promoted the root (owner + view stakes),
// the owner's drop freed the buffer out from under every live view. The
// uninit-alloc change (e8550708) unmasked it: reused buffers now hold
// garbage instead of stale-but-plausible bytes. cajeta.search.ngram.Index
// failed exactly this way (NgramIndexTests, tour SearchDemo segfault): its
// grams() builds rolling substrings of a local `lower`, transfers them out
// via `#`, and `lower` drops at return.
//
// The idiom under test is the SUPPORTED one — views escape by `#` transfer
// into a container; the source wrapper and its stake drop with the scope;
// the root buffer must live until the last view releases.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

std::string makeSource(const std::string& body) {
    return "package test;\n"
           "import cajeta.collection.ArrayList;\n"
           "import cajeta.collection.HashMap;\n"
           "public final class Ut {\n"
           "    // Rolling n-grams of a DYING local root — the Index shape.\n"
           "    public static #ArrayList<String> grams(String key, int32 n) {\n"
           "        ArrayList<String> out = heap ArrayList<String>();\n"
           "        String lower #= key.toLowerCase();\n"
           "        int32 len = (int32) lower.count();\n"
           "        if (len < n) {\n"
           "            String whole #= lower.substring(0, len);\n"
           "            out.add(#whole);\n"
           "            return out;\n"
           "        }\n"
           "        int32 i = 0;\n"
           "        while (i + n <= len) {\n"
           "            String g #= lower.substring(i, i + n);\n"
           "            out.add(#g);\n"
           "            i = i + 1;\n"
           "        }\n"
           "        return out;\n"
           "    }\n"
           "    public static int32 run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

int32_t runJit(const std::string& body) {
    auto jit = CajetaJit::compile(makeSource(body), "test.Ut");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// Multiple rolling views of one mode-0 root, transferred into a list; the
// root's owner (grams()'s `lower`) drops before the views are read.
TEST(SliceCallArgEscapeTests, rollingViewsSurviveSourceDrop) {
    EXPECT_EQ(runJit(
        "ArrayList<String> ks #= Ut.grams(\"APPLE\", 3);\n"   // app ppl ple
        "if (ks.count() != 3) { return -1; }\n"
        "String k0 = ks.get(0);\n"
        "if (!(k0 == \"app\")) { return -2; }\n"
        "String k1 = ks.get(1);\n"
        "if (!(k1 == \"ppl\")) { return -3; }\n"
        "String k2 = ks.get(2);\n"
        "if (!(k2 == \"ple\")) { return -4; }\n"
        "return 1;"), 1);
}

// The full Index shape: chained substrings of escaped views become HashMap
// keys; lookups probe with views built from a second dying root. One shared
// gram ("ple") must hit; the others must miss.
TEST(SliceCallArgEscapeTests, escapedViewKeysProbeCorrectly) {
    EXPECT_EQ(runJit(
        "HashMap<String, int32> m = heap HashMap<String, int32>(64);\n"
        "ArrayList<String> ks #= Ut.grams(\"APPLE\", 3);\n"
        "int32 i = 0;\n"
        "while (i < ks.count()) {\n"
        "    String g #= ks.get(i);\n"
        "    if (!m.containsKey(g)) {\n"
        "        String owned #= g.substring(0, (int32) g.count());\n"
        "        m.put(#owned, 1);\n"
        "    }\n"
        "    i = i + 1;\n"
        "}\n"
        "ArrayList<String> qs #= Ut.grams(\"aple\", 3);\n"     // apl ple
        "int32 hits = 0;\n"
        "int32 j = 0;\n"
        "while (j < qs.count()) {\n"
        "    String q = qs.get(j);\n"
        "    if (m.containsKey(q)) { hits = hits + 1; }\n"
        "    j = j + 1;\n"
        "}\n"
        "if (hits != 1) { return -1; }\n"
        "return 1;"), 1);
}
