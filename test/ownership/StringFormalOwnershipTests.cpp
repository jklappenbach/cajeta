// A `#String` formal holds the title its caller tendered: the callee frees it at return
// unless a store or a `#` forward took it, and a borrow tendered at runtime stays a borrow.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.S");
    if (!jit) return -1;
    auto fn = jit->lookup<int32_t (*)()>("run");
    if (!fn) return -2;
    return fn();
}

const char* PRE =
    "package test;\n"
    "import cajeta.lang.Cajeta;\n"
    "import cajeta.lang.String;\n"
    "import cajeta.collection.ArrayList;\n"
    "public class Holder {\n"
    "    public String m;\n"
    "    public String[] slots = heap String[1];\n"
    "    public Holder(#String s) { this.m #= s; }\n"
    "    public void put(#String s) { this.slots[0] #= s; }\n"
    "    public String peek() { return this.m; }\n"
    "}\n"
    "public class Moved {\n"
    "    public String m;\n"
    "    public Moved(#String s) { this.m #= s; }\n"
    "}\n"
    "public final class S {\n"
    "    static int32 keep(#String s) { return (int32) s.byteLength(); }\n"
    "    static #Exception wrap(#String d) { return heap Exception(#d); }\n"
    "    static String mk(int32 i) { return \"value-\" + i + \"-abcdefghijklmnopqrstuvwxyz0123456789\"; }\n"
    "    static #String mkOwned(int32 i) { return \"value-\" + i + \"-abcdefghijklmnopqrstuvwxyz0123456789\"; }\n";

// Runs `body` 200 times after a warm-up; returns the live-count growth.
std::string growth(const std::string& body) {
    return std::string(PRE) +
        "    static void once(int32 i) {\n        " + body + "\n    }\n"
        "    public static int32 run() {\n"
        "        S.once(0);\n"
        "        int64 l0 = Cajeta.liveCount();\n"
        "        int32 i = 0;\n"
        "        while (i < 200) { S.once(i); i = i + 1; }\n"
        "        return (int32) (Cajeta.liveCount() - l0);\n"
        "    }\n"
        "}\n";
}

} // namespace

TEST(StringFormalOwnershipTests, freshArgumentIsFreedByTheCallee) {
    EXPECT_EQ(runI32(growth("S.keep(S.mk(i));")), 0);
    EXPECT_EQ(runI32(growth("S.keep(S.mkOwned(i));")), 0);
    EXPECT_EQ(runI32(growth("String b #= S.mkOwned(i); S.keep(\"<\" + b);")), 0);
    EXPECT_EQ(runI32(growth("String b #= S.mkOwned(i); S.keep(#b);")), 0);
    EXPECT_EQ(runI32(growth("S.keep(\"literal\");")), 0);
}

TEST(StringFormalOwnershipTests, forwardAndStoreTakeTheTitle) {
    EXPECT_EQ(runI32(growth("Exception e #= S.wrap(S.mk(i));")), 0);
    EXPECT_EQ(runI32(growth("Holder h = heap Holder(S.mk(i));")), 0);
    EXPECT_EQ(runI32(growth("Moved m = heap Moved(S.mk(i));")), 0);
    EXPECT_EQ(runI32(growth("Holder h = heap Holder(S.mk(i)); h.put(S.mk(i + 1));")), 0);
}

// Stored strings survive churn intact, and a runtime borrow stored through the
// formal is copied, so the lender's string outlives every holder.
TEST(StringFormalOwnershipTests, storedStringsStayIntact) {
    std::string src = std::string(PRE) +
        "    public static int32 run() {\n"
        "        Holder src = heap Holder(S.mk(5));\n"
        "        ArrayList<Holder> hs = heap ArrayList<Holder>();\n"
        "        ArrayList<Moved> ms = heap ArrayList<Moved>();\n"
        "        int32 i = 0;\n"
        "        while (i < 100) {\n"
        "            Holder h = heap Holder(S.mk(i));\n"
        "            h.put(src.peek());\n"
        "            hs.add(#h);\n"
        "            ms.add(heap Moved(S.mk(i)));\n"
        "            i = i + 1;\n"
        "        }\n"
        "        int32 c = 0;\n"
        "        while (c < 300) { String junk #= S.mkOwned(c + 9000); c = c + 1; }\n"
        "        hs = heap ArrayList<Holder>();\n"
        "        int32 bad = 0;\n"
        "        i = 0;\n"
        "        while (i < 100) {\n"
        "            String want #= S.mk(i);\n"
        "            if (!ms.get(i).m.equals(want)) { bad = bad + 1; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        String five #= S.mk(5);\n"
        "        if (!src.peek().equals(five)) { bad = bad + 1000; }\n"
        "        return bad;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}

// Forwarding a titled `#String` formal plainly into another `#String` formal is rejected.
TEST(StringFormalOwnershipTests, plainForwardOfTitledFormalIsRejected) {
    std::string src = std::string(PRE) +
        "    static #Exception bad(#String d) { return heap Exception(d); }\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    EXPECT_ANY_THROW(CajetaJit::compile(src, "test.S"));
}
