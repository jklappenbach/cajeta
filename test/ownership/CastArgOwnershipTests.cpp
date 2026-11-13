// A String cast of an owned call result, passed to a `#String` formal, keeps the result's title,
// and `String.clone()` is typed `#String` so the call needs no cast.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runVerdict(const std::string& src) {
    try {
        auto jit = CajetaJit::compile(src.c_str(), "test.A");
        if (!jit) return -1;
        auto fn = jit->lookup<int32_t (*)()>("run");
        if (!fn) return -2;
        return fn();
    } catch (cajeta::Exception& e) {
        ADD_FAILURE() << "unexpected rejection: " << e.getErrorId() << ": " << e.getMessage();
        return -3;
    }
}

// 64 passes of `probeBody` from a borrowed String `h`; `failBase` for a wrong value, +1 for a leak.
std::string loop(const std::string& probeBody, int failBase) {
    return std::string(
        "package test;\n"
        "import cajeta.collection.ArrayList;\n"
        "import cajeta.lang.Cajeta;\n"
        "import cajeta.lang.String;\n"
        "public final class A {\n"
        "    static class SBox {\n"
        "        public String s;\n"
        "        public SBox() { }\n"
        "        public SBox(#String v) { this.s #= v; }\n"
        "        public void take(#String v) { this.s #= v; }\n"
        "    }\n"
        "    static int32 keep(#String v) { SBox b = heap SBox(); b.take(#v); return b.s.byteLength(); }\n"
        "    static int32 probe(ArrayList<String> hs, int32 i) {\n"
        "        String h = hs.get(0);\n")
        + probeBody
        + "    }\n"
        "    public static int32 run() {\n"
        "        ArrayList<String> hs = heap ArrayList<String>();\n"
        "        hs.add(\"a string well past the inline limit\");\n"
        "        int32 want = hs.get(0).byteLength();\n"
        "        if (A.probe(hs, 0) != want) { return " + std::to_string(failBase) + "; }\n"
        "        int64 l0 = Cajeta.liveCount();\n"
        "        int32 i = 1;\n"
        "        while (i < 65) {\n"
        "            if (A.probe(hs, i) != want) { return " + std::to_string(failBase) + "; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (Cajeta.liveCount() != l0) { return " + std::to_string(failBase + 1) + "; }\n"
        "        if (hs.get(0).byteLength() != want) { return " + std::to_string(failBase + 2) + "; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
}

} // namespace

TEST(CastArgOwnershipTests, castCloneToOwnedStaticFormalMoves) {
    EXPECT_EQ(runVerdict(loop("        return A.keep((String) h.clone());\n", 10)), 0)
        << "10 = wrong value; 11 = the clone leaked or was freed twice; 12 = the source was disturbed";
}

TEST(CastArgOwnershipTests, castCloneToOwnedCtorFormalMoves) {
    EXPECT_EQ(runVerdict(loop("        SBox b = heap SBox((String) h.clone());\n"
                              "        return b.s.byteLength();\n", 20)), 0)
        << "20 = wrong value; 21 = the clone leaked or was freed twice; 22 = the source was disturbed";
}

TEST(CastArgOwnershipTests, castCloneToOwnedMethodFormalMoves) {
    EXPECT_EQ(runVerdict(loop("        SBox b = heap SBox();\n"
                              "        b.take((String) h.clone());\n"
                              "        return b.s.byteLength();\n", 30)), 0)
        << "30 = wrong value; 31 = the clone leaked or was freed twice; 32 = the source was disturbed";
}

TEST(CastArgOwnershipTests, sharpCastCloneStillMoves) {
    EXPECT_EQ(runVerdict(loop("        return A.keep(#((String) h.clone()));\n", 40)), 0)
        << "40 = wrong value; 41 = the clone leaked or was freed twice; 42 = the source was disturbed";
}

TEST(CastArgOwnershipTests, stringCloneIsAnOwnedString) {
    EXPECT_EQ(runVerdict(loop("        SBox b = heap SBox(h.clone());\n"
                              "        return A.keep(h.clone()) + b.s.byteLength() - h.byteLength();\n", 50)), 0)
        << "50 = wrong value; 51 = a clone leaked or was freed twice; 52 = the source was disturbed";
}

TEST(CastArgOwnershipTests, sharpStringCloneMoves) {
    EXPECT_EQ(runVerdict(loop("        return A.keep(#h.clone());\n", 60)), 0)
        << "60 = wrong value; 61 = the clone leaked or was freed twice; 62 = the source was disturbed";
}
