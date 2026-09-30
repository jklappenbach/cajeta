// An owned temporary receiver dies at the end of its statement: a chain frees it,
// a borrow of it is usable inside the statement, and binding, storing or returning
// that borrow is a compile error.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

const char* PRE =
    "package test;\n"
    "import cajeta.lang.Cajeta;\n"
    "import cajeta.lang.String;\n"
    "public class Doc {\n"
    "    String head;\n"
    "    int32 n;\n"
    "    public Doc(int32 n) { this.n = n; this.head = \"title-\" + n + \"-abcdefghijklmnopqrstuvwxyz\"; }\n"
    "    public static #Doc parse(int32 n) { return heap Doc(n); }\n"
    "    public String title() { return this.head; }\n"
    "    public #String titleCopy() { return \"\" + this.head; }\n"
    "    public int32 words() { return this.n; }\n"
    "    public Doc self() { return this; }\n"
    "}\n"
    "public class Built { public int32 a; public int32 b; }\n"
    "public class Builder {\n"
    "    int32 a; int32 b;\n"
    "    public static #Builder make() { return heap Builder(); }\n"
    "    public Builder withA(int32 v) { this.a = v; return this; }\n"
    "    public Builder withB(int32 v) { this.b = v; return this; }\n"
    "    public #Built build() { Built x = heap Built(); x.a = this.a; x.b = this.b; return x; }\n"
    "}\n"
    "public class Keep {\n"
    "    public static Doc shared;\n"
    "    public static Doc get() { return Keep.shared; }\n"
    "    public static Doc wrapped(int32 n) { return Doc.parse(n); }\n"
    "}\n"
    "public final class T {\n"
    "    static int32 len(String s) { return (int32) s.byteLength(); }\n";

std::string body(const std::string& helpers, const std::string& run) {
    return std::string(PRE) + helpers + "    public static int32 run() {\n" + run + "    }\n}\n";
}

int32_t runI32(const std::string& src) {
    try {
        auto jit = CajetaJit::compile(src.c_str(), "test.T");
        if (!jit) return -1;
        auto fn = jit->lookup<int32_t (*)()>("run");
        if (!fn) return -2;
        return fn();
    } catch (cajeta::Exception& e) {
        ADD_FAILURE() << "unexpected rejection: " << e.getErrorId() << ": " << e.getMessage();
        return -3;
    }
}

void expectRejected(const std::string& src) {
    try {
        CajetaJit::compile(src.c_str(), "test.T");
        ADD_FAILURE() << "expected CAJETA_ERROR_BORROW_OF_TEMPORARY";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_BORROW_OF_TEMPORARY") << e.getMessage();
    }
}

// Runs `stmt` 200 times after a warm-up and returns the live-count growth.
std::string growth(const std::string& stmt) {
    return
        "        int32 acc = 0;\n"
        "        int32 i = 0;\n"
        "        " + stmt + "\n"
        "        int64 l0 = Cajeta.liveCount();\n"
        "        while (i < 200) { " + stmt + " i = i + 1; }\n"
        "        if (acc < 0) { return -9; }\n"
        "        return (int32) (Cajeta.liveCount() - l0);\n";
}

} // namespace

TEST(StatementTempTests, ownedReceiverIsFreedAtStatementEnd) {
    EXPECT_EQ(runI32(body("", growth("acc = acc + Doc.parse(i).words();"))), 0);
    EXPECT_EQ(runI32(body("", growth("acc = acc + T.len(Doc.parse(i).title());"))), 0);
    EXPECT_EQ(runI32(body("", growth("if (Doc.parse(i).words() > 3) { acc = acc + 1; }"))), 0);
    EXPECT_EQ(runI32(body("", growth("acc = acc + Keep.wrapped(i).words();"))), 0);
}

TEST(StatementTempTests, builderChainDoesNotLeak) {
    EXPECT_EQ(runI32(body("", growth(
        "Built x #= Builder.make().withA(i).withB(2).build(); acc = acc + x.a + x.b;"))), 0);
    EXPECT_EQ(runI32(body("",
        "        Built x #= Builder.make().withA(40).withB(2).build();\n"
        "        return x.a + x.b;\n")), 42);
}

TEST(StatementTempTests, borrowUsedInsideTheStatementIsIntact) {
    EXPECT_EQ(runI32(body("",
        "        int32 bad = 0;\n"
        "        int32 i = 0;\n"
        "        while (i < 100) {\n"
        "            String want #= \"title-\" + i + \"-abcdefghijklmnopqrstuvwxyz\";\n"
        "            if (!Doc.parse(i).title().equals(want)) { bad = bad + 1; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return bad;\n")), 0);
}

TEST(StatementTempTests, borrowedReceiverIsNotFreed) {
    EXPECT_EQ(runI32(body("",
        "        Keep.shared = heap Doc(7);\n"
        "        Doc d #= Doc.parse(5);\n"
        "        int32 acc = 0;\n"
        "        int32 i = 0;\n"
        "        while (i < 50) { acc = acc + d.self().words() + Keep.get().words(); i = i + 1; }\n"
        "        String t = Keep.get().title();\n"
        "        return acc + T.len(t) + d.words();\n")), 50 * 12 + 34 + 5);
}

TEST(StatementTempTests, keepingABorrowOfATemporaryIsRejected) {
    expectRejected(body("", "        String s = Doc.parse(1).title();\n        return T.len(s);\n"));
    expectRejected(body("", "        Doc d = Doc.parse(1).self();\n        return d.words();\n"));
    expectRejected(body("    static String leak() { return Doc.parse(1).title(); }\n",
                        "        return T.len(T.leak());\n"));
    expectRejected(std::string(PRE) +
        "    String kept;\n"
        "    void keep() { this.kept = Doc.parse(1).title(); }\n"
        "    public static int32 run() { return 0; }\n}\n");
}

TEST(StatementTempTests, ownedResultsAndNamedOwnersAreAccepted) {
    EXPECT_EQ(runI32(body("",
        "        String s #= Doc.parse(3).titleCopy();\n"
        "        Doc d #= Doc.parse(4);\n"
        "        String t = d.title();\n"
        "        int32 n = Doc.parse(5).words();\n"
        "        return T.len(s) + T.len(t) + n;\n")), 34 + 34 + 5);
}

// A borrow of a temporary passed to a formal the callee keeps would outlive its statement.
TEST(StatementTempTests, borrowOfATemporaryPassedToAKeeperIsRejected) {
    const char* KEEP =
        "    static String held;\n"
        "    static void keep(String s) { T.held #= s; }\n"
        "    static void relay(String s) { T.keep(s); }\n"
        "    static int32 take(#String s) { return (int32) s.byteLength(); }\n";
    expectRejected(body(KEEP, "        T.keep(Doc.parse(1).title());\n        return 0;\n"));
    expectRejected(body(KEEP, "        T.relay(Doc.parse(1).title());\n        return 0;\n"));
    expectRejected(body(KEEP, "        return T.take(Doc.parse(1).title());\n"));
    expectRejected(std::string(PRE) +
        "    public static int32 run() {\n"
        "        cajeta.collection.ArrayList<Doc> rows = heap cajeta.collection.ArrayList<Doc>();\n"
        "        rows.add(#heap Doc(1).self());\n"
        "        return rows.count();\n"
        "    }\n}\n");
}

TEST(StatementTempTests, borrowOfATemporaryPassedToAReaderIsAccepted) {
    const char* READ =
        "    static int32 outer(String s) { return T.len(s); }\n"
        "    static int32 twice(String s) { String t = s; return T.len(t) * 2; }\n";
    EXPECT_EQ(runI32(body(READ,
        "        return T.outer(Doc.parse(1).title()) + T.twice(Doc.parse(2).title());\n")),
        34 + 68);
}

// A by-value result that holds no reference cannot reach the temporary, so it may be kept.
TEST(StatementTempTests, referenceFreeValueResultIsKept) {
    EXPECT_EQ(runI32(std::string(PRE) +
        "    public static int32 run() {\n"
        "        cajeta.collection.ArrayList<int32> xs = heap cajeta.collection.ArrayList<int32>();\n"
        "        xs.add(2); xs.add(5); xs.add(8);\n"
        "        Optional<int32> hit = xs.stream().findFirst((int32 v) -> { return v > 4; });\n"
        "        return hit.get();\n"
        "    }\n}\n"), 5);
    expectRejected(std::string(PRE) +
        "    public static int32 run() {\n"
        "        cajeta.collection.ArrayList<Doc> ds = heap cajeta.collection.ArrayList<Doc>();\n"
        "        ds.add(heap Doc(3));\n"
        "        Optional<Doc> hit = ds.stream().findFirst((Doc d) -> { return d.words() > 1; });\n"
        "        return hit.get().words();\n"
        "    }\n}\n");
}
