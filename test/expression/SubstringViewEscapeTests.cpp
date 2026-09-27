// A String local from substring or trim becomes a zero-copy view only when it
// never leaves the method. A `#` transfer into another local's initializer is
// a way out, and must keep the local a real copy.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

std::string makeSource(const std::string& body) {
    return "package test;\n"
           "class Box {\n"
           "    public String v;\n"
           "    public Box(#String v) { this.v = #v; }\n"
           "}\n"
           "class Table {\n"
           "    Box[] boxes;\n"
           "    int32 n;\n"
           "    public Table() { this.boxes = heap Box[8]; this.n = 0; }\n"
           "    public void keep(#Box b) { this.boxes[this.n] = #b; this.n = this.n + 1; }\n"
           "    public String at(int32 i) { return this.boxes[i].v; }\n"
           "}\n"
           "public final class V {\n"
           "    static #String source(String tag) {\n"
           "        return tag + \"-0123456789-abcdefghij-0123456789\";\n"
           "    }\n"
           "    static void named(Table t, String tag) {\n"
           "        String value #= V.source(tag);\n"
           "        String tv #= value.substring(0, value.byteLength());\n"
           "        Box b = heap Box(#tv);\n"
           "        t.keep(#b);\n"
           "    }\n"
           "    static void inBlock(Table t, String tag) {\n"
           "        String value #= V.source(tag);\n"
           "        if (tag.byteLength() > 0) {\n"
           "            String tv #= value.substring(0, value.byteLength());\n"
           "            Box b = heap Box(#tv);\n"
           "            t.keep(#b);\n"
           "        }\n"
           "    }\n"
           "    public static int32 run() {\n"
           + body +
           "    }\n"
           "}\n";
}

int32_t run(const std::string& body) {
    auto jit = CajetaJit::compile(makeSource(body), "test.V");
    return jit->lookup<int32_t (*)()>("run")();
}

}  // namespace

TEST(SubstringViewEscapeTests, aSubstringTransferredIntoAnotherLocalsInitializerIsACopy) {
    EXPECT_EQ(run("        Table t = heap Table();\n"
                  "        V.named(t, \"one\");\n"
                  "        V.named(t, \"two\");\n"
                  "        String churn = V.source(\"churn\") + V.source(\"more\");\n"
                  "        boolean ok = t.at(0).equals(V.source(\"one\")) && t.at(1).equals(V.source(\"two\"));\n"
                  "        return ok ? 1 : 0;\n"),
              1);
}

TEST(SubstringViewEscapeTests, theSameInsideABlockIsACopy) {
    EXPECT_EQ(run("        Table t = heap Table();\n"
                  "        V.inBlock(t, \"one\");\n"
                  "        V.inBlock(t, \"two\");\n"
                  "        String churn = V.source(\"churn\") + V.source(\"more\");\n"
                  "        boolean ok = t.at(0).equals(V.source(\"one\")) && t.at(1).equals(V.source(\"two\"));\n"
                  "        return ok ? 1 : 0;\n"),
              1);
}

TEST(SubstringViewEscapeTests, aSubstringThatStaysLocalIsStillAView) {
    EXPECT_EQ(run("        String big #= V.source(\"local-0123456789-abcdefghij-0123456789\");\n"
                  "        int64 st0 = Cajeta.sharedPopulation();\n"
                  "        String s #= big.substring(2, 60);\n"
                  "        int64 during = Cajeta.sharedPopulation() - st0;\n"
                  "        int32 len = s.byteLength();\n"
                  "        return during == 0 && len == 58 ? 1 : 0;\n"),
              1);
}
