// field-store-ownership 6.2.4 (spec 5.2.6): a String array carries a title per slot that travels
// with the array, so a slot stored with `=` from a name borrows, like every other element type.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

std::string makeSource(const std::string& workBody) {
    return "package test;\n"
           "public class K {\n"
           "    public String[] names;\n"
           "    public K() { this.names = heap String[4]; }\n"
           "    public void put(int32 i, ^String s) { this.names[i] = s; }\n"
           "    public void adopt(#String[] a) { this.names #= a; }\n"
           "    public static int64 localBytes(^String s) {\n"
           "        String[] a = heap String[2];\n"
           "        int64 b0 = Cajeta.allocatedBytes();\n"
           "        a[0] = s;\n"
           "        int64 d = Cajeta.allocatedBytes() - b0;\n"
           "        if (a[0].size() != s.size()) { return -1L; }\n"
           "        return d;\n"
           "    }\n"
           "    public static int64 moveIn(^String s) {\n"
           "        String[] a = heap String[2];\n"
           "        a[0] = s;\n"
           "        a[1] #= Ut.heapString(30);\n"
           "        K k = heap K();\n"
           "        k.adopt(#a);\n"
           "        return k.names[1].size() + k.names[0].size();\n"
           "    }\n"
           "    public static void keepWindow(^String[] a, ^String w) { a[0] #= w; }\n"
           "}\n"
           "public final class Ut {\n"
           "    public static #String heapString(int32 n) {\n"
           "        int8[] buf = Cajeta.allocBytes((int64) n);\n"
           "        int32 i = 0;\n"
           "        while (i < n) {\n"
           "            buf[i] = (int8) (97 + (i - (i / 26) * 26));\n"
           "            i = i + 1;\n"
           "        }\n"
           "        return heap String(#buf, n);\n"
           "    }\n"
           "    public static String plainString(int32 n) { return Ut.heapString(n); }\n"
           "    public static void churn() {\n"
           "        int64 i = 0;\n"
           "        while (i < 400L) { String j #= Ut.heapString(64); i = i + 1; }\n"
           "    }\n"
           "    public static int32 work() {\n"
           "        " + workBody + "\n"
           "    }\n"
           "    public static int32 run() {\n"
           "        int64 base = Cajeta.liveCount();\n"
           "        int32 t = Ut.work();\n"
           "        int64 leaked = Cajeta.liveCount() - base;\n"
           "        return (int32) (leaked * 100) + t;\n"
           "    }\n"
           "}\n";
}

int32_t runJit(const std::string& workBody) {
    auto jit = CajetaJit::compile(makeSource(workBody), "test.Ut");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

}  // namespace

// 5.2.6: a local array slot borrows a `^String`, and the array never frees it.
TEST(StringArraySlotTests, localSlotBorrowsWithoutCopy) {
    EXPECT_EQ(runJit(
        "String s #= Ut.heapString(40);\n"
        "if (K.localBytes(s) != 0L) { return -1; }\n"
        "Ut.churn();\n"
        "if (s.size() != 40 || s.charAt(0) != (int8) 97) { return -2; }\n"
        "return 1;"), 1);
}

// 5.2.6: a field-held array slot borrows too, and dropping the holder leaves the String intact.
TEST(StringArraySlotTests, fieldHeldSlotBorrowsWithoutCopy) {
    EXPECT_EQ(runJit(
        "String s #= Ut.heapString(40);\n"
        "{\n"
        "    K k = heap K();\n"
        "    int64 b0 = Cajeta.allocatedBytes();\n"
        "    k.put(1, s);\n"
        "    if (Cajeta.allocatedBytes() != b0) { return -1; }\n"
        "}\n"
        "Ut.churn();\n"
        "if (s.size() != 40 || s.charAt(0) != (int8) 97) { return -2; }\n"
        "return 1;"), 1);
}

// The case the sidecar could not carry: a borrowed slot stays a borrow after the array moves into a field.
TEST(StringArraySlotTests, borrowedSlotSurvivesTheArrayMovingIntoAField) {
    EXPECT_EQ(runJit(
        "String s #= Ut.heapString(40);\n"
        "if (K.moveIn(s) != 70L) { return -1; }\n"
        "Ut.churn();\n"
        "if (s.size() != 40 || s.charAt(0) != (int8) 97) { return -2; }\n"
        "return 1;"), 1);
}

// Rule 7: a producer written in place owns its slot, and the array frees it.
TEST(StringArraySlotTests, producerInPlaceOwnsItsSlot) {
    EXPECT_EQ(runJit(
        "String[] a = heap String[2];\n"
        "a[0] = Ut.plainString(40);\n"
        "a[1] #= Ut.heapString(20);\n"
        "Ut.churn();\n"
        "if (a[0].size() != 40 || a[1].size() != 20) { return -1; }\n"
        "return 1;"), 1);
}

// Card 5: `#=` of a lent window into a slot resolves, so the slot outlives the window's owner.
TEST(StringArraySlotTests, sharpStoreOfLentWindowResolves) {
    EXPECT_EQ(runJit(
        "String[] a = heap String[1];\n"
        "{\n"
        "    String big #= Ut.heapString(300);\n"
        "    String win #= big.substring(10, 40);\n"
        "    K.keepWindow(a, win);\n"
        "}\n"
        "Ut.churn();\n"
        "if (a[0].size() != 30 || a[0].charAt(0) != (int8) 107) { return -1; }\n"
        "return 1;"), 1);
}

// `#a[i]` takes an owned slot's title, so the String is freed exactly once.
TEST(StringArraySlotTests, takeMovesTheSlotTitle) {
    EXPECT_EQ(runJit(
        "String[] a = heap String[2];\n"
        "a[0] #= Ut.heapString(40);\n"
        "String t #= a[0];\n"
        "Ut.churn();\n"
        "if (t.size() != 40) { return -1; }\n"
        "return 1;"), 1);
}

// Spec 9.5: a shift within one array spelled `#=` moves the title, and clearing the tail frees nothing live.
TEST(StringArraySlotTests, sharpShiftWithinOneArrayMovesTheTitle) {
    EXPECT_EQ(runJit(
        "String[] a = heap String[3];\n"
        "a[0] #= Ut.heapString(10);\n"
        "a[1] #= Ut.heapString(20);\n"
        "a[2] #= Ut.heapString(30);\n"
        "a[0] #= a[1];\n"
        "a[1] #= a[2];\n"
        "a[2] = null;\n"
        "Ut.churn();\n"
        "if (a[0].size() != 20 || a[1].size() != 30) { return -1; }\n"
        "return 1;"), 1);
}

// An array literal of Strings is freed with its owned elements and never frees a borrowed one.
TEST(StringArraySlotTests, literalArrayElements) {
    EXPECT_EQ(runJit(
        "String s #= Ut.heapString(40);\n"
        "{\n"
        "    String[] a = [s, \"static\", Ut.plainString(12)];\n"
        "    if (a[0].size() != 40 || a[2].size() != 12) { return -1; }\n"
        "}\n"
        "Ut.churn();\n"
        "if (s.size() != 40 || s.charAt(0) != (int8) 97) { return -2; }\n"
        "return 1;"), 1);
}

// Twin: a slot that lends a frame-owned local cannot leave the frame, as for any element type.
TEST(StringArraySlotTests, slotLendingAnOwnedLocalCannotEscape) {
    std::string src = makeSource(
        "String s #= Ut.heapString(40);\n"
        "String[] a = heap String[2];\n"
        "a[0] = s;\n"
        "K k = heap K();\n"
        "k.adopt(#a);\n"
        "return 1;");
    try {
        CajetaJit::compile(src, "test.Ut");
        ADD_FAILURE() << "expected CAJETA_ERROR_ARRAY_SLOT_BORROWS_LOCAL";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_ARRAY_SLOT_BORROWS_LOCAL") << e.getMessage();
    }
}
