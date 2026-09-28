// A throw tenders the thrown value's title when it holds one, and the catch
// variable owns what it was handed: a caught exception is freed at the end of
// its clause, a thrown local survives to its handler, a rethrow forwards the
// title, and a borrowed exception is never freed by the handler.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.U");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

// `body(i)` runs 200 times after a warm-up; returns its last value when the
// live count held, -1 when it moved.
std::string loop(const std::string& members, const std::string& body) {
    return
        "package test;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.error.RecoverableException;\n"
        "class Oops extends RecoverableException {\n"
        "    String extra;\n"
        "    Oops(String m) { this.message = \"\" + m; this.extra = \"\" + m; this.cause = 0; }\n"
        "}\n"
        "class Other extends RecoverableException {\n"
        "    Other(String m) { this.message = \"\" + m; this.cause = 0; }\n"
        "}\n"
        "public final class U {\n" + members +
        "    static int32 body(int32 i) {\n" + body +
        "    }\n"
        "    public static int32 run() {\n"
        "        int32 last = U.body(0);\n"
        "        int64 live = Cajeta.liveCount();\n"
        "        int32 i = 1;\n"
        "        while (i <= 200) { last = U.body(i); i = i + 1; }\n"
        "        return Cajeta.liveCount() == live ? last : -1;\n"
        "    }\n"
        "}\n";
}

const char* kThrowers =
    "    static int32 fresh(int32 i) { throw heap Oops(\"fresh \" + i); }\n"
    "    static int32 local(int32 i) {\n"
    "        Oops o = heap Oops(\"local \" + i);\n"
    "        throw o;\n"
    "    }\n";

} // namespace

TEST(ExceptionTitleTests, caughtFreshExceptionIsFreed) {
    auto src = loop(kThrowers,
        "        try { return U.fresh(i); } catch (Oops e) { return e.extra.byteLength(); }\n");
    EXPECT_EQ(runI32(src), 9);
}

TEST(ExceptionTitleTests, thrownOwnedLocalSurvivesToItsHandler) {
    auto src = loop(kThrowers,
        "        try { return U.local(i); } catch (Oops e) {\n"
        "            String churn = \"churn churn churn churn churn churn churn \" + i;\n"
        "            return e.extra.equals(\"local \" + i) ? 1 : 0;\n"
        "        }\n");
    EXPECT_EQ(runI32(src), 1);
}

TEST(ExceptionTitleTests, rethrowForwardsTheTitle) {
    auto src = loop(std::string(kThrowers) +
        "    static int32 relay(int32 i) {\n"
        "        try { return U.local(i); } catch (Oops e) { throw e; }\n"
        "    }\n",
        "        try { return U.relay(i); } catch (Oops e) { return e.extra.equals(\"local \" + i) ? 1 : 0; }\n");
    EXPECT_EQ(runI32(src), 1);
}

TEST(ExceptionTitleTests, borrowedExceptionIsNotFreedByTheHandler) {
    auto src = loop(
        "    static Oops kept = heap Oops(\"kept for every throw\");\n"
        "    static int32 raise() { throw U.kept; }\n",
        "        try { return U.raise(); } catch (Oops e) { return 0; }\n"
        "        return 0;\n");
    std::string full = src;
    auto at = full.find("        return Cajeta.liveCount() == live ? last : -1;\n");
    full.insert(at, "        if (!U.kept.extra.equals(\"kept for every throw\")) { return -2; }\n");
    EXPECT_EQ(runI32(full), 0);
}

TEST(ExceptionTitleTests, unmatchedClausePropagatesTheTitle) {
    auto src = loop(std::string(kThrowers) +
        "    static int32 wrongClause(int32 i) {\n"
        "        try { return U.fresh(i); } catch (Other e) { return -5; }\n"
        "    }\n",
        "        try { return U.wrongClause(i); } catch (Oops e) { return 3; }\n");
    EXPECT_EQ(runI32(src), 3);
}

TEST(ExceptionTitleTests, finallyPathsFreeAndPropagate) {
    auto src = loop(std::string(kThrowers) +
        "    static int32 fin(int32 i) {\n"
        "        try { return U.fresh(i); } catch (Oops e) { throw heap Other(\"from catch \" + i); } finally { i = i + 0; }\n"
        "    }\n",
        "        int32 got = 0;\n"
        "        try { U.fresh(i); } catch (Oops e) { got = got + 1; } finally { got = got + 1; }\n"
        "        try { return U.fin(i); } catch (Other e) { return got + 2; }\n");
    EXPECT_EQ(runI32(src), 4);
}

TEST(ExceptionTitleTests, returnInsideTheCatchFreesIt) {
    auto src = loop(kThrowers,
        "        int32 k = 0;\n"
        "        while (k < 3) {\n"
        "            try { U.fresh(i); } catch (Oops e) { if (k == 1) { return 7; } }\n"
        "            k = k + 1;\n"
        "        }\n"
        "        return 0;\n");
    EXPECT_EQ(runI32(src), 7);
}

TEST(ExceptionTitleTests, throwingAFieldThrowsTheObject) {
    auto src = loop(
        "    Oops held;\n"
        "    U() { this.held = heap Oops(\"held by an instance\"); }\n"
        "    int32 raise() { throw this.held; }\n"
        "    static U one = heap U();\n",
        "        try { return U.one.raise(); } catch (Oops e) { return e.extra.equals(\"held by an instance\") ? 5 : 0; }\n");
    EXPECT_EQ(runI32(src), 5);
}
