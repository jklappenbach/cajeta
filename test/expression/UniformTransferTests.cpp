// uniform-transfer-semantics Unit 1 (spec 2.1) — a field behaves like a
// variable: `x #= obj.field` TRANSFERS the title, exactly as from a local.
//
// These pin behaviour the compiler already has. `#=` wraps its RHS in an
// implicit Move, so `x #= f` reaches the same field-extraction path as an
// explicit `#f` (Expression.cpp §6.3): it takes the title and clears the
// source's ownership bit. An explicit `#` on the right makes it a DOUBLE
// move, which forwards the slot's mode verbatim instead — that is the
// "fused claim", and Unit 3 makes it an error.
//
// Written before Unit 2 so the container work has a baseline that proves
// 2.1 is not the part that needs changing.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

#include "cajeta/error/Exception.h"

using cajeta_test::CajetaJit;

namespace {

// A holder with an OWNED class-typed field. Liveness is measured with
// `Cajeta.liveCount()` — the oracle the drop suites already trust. An
// earlier draft used a `finalize()` counter, which the drop machinery
// never calls: the test then measured nothing and "failed" for a reason
// that had no bearing on transfer semantics.
const char* kSrc =
    "package test;\n"
    "public class Cell {\n"
    "    public int32 n;\n"
    "    public Cell(int32 nn) { this.n = nn; }\n"
    "}\n"
    "public class Holder {\n"
    "    public Cell c;\n"
    "    public Holder() { this.c = null; }\n"
    "    public void put(Cell v) { this.c #= v; }\n"
    "}\n";

int32_t runI32(const std::string& src, const char* entryClass = "test.D") {
    auto jit = CajetaJit::compile(src, entryClass);
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

}  // namespace

// 1.1.1 — `T t #= holder.field` from an OWNED slot yields the value.
TEST(UniformTransferTests, sharpStoreFromOwnedFieldTransfers) {
    EXPECT_EQ(runI32(std::string(kSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Holder h = heap Holder();\n"
        "        h.put(heap Cell(7));\n"
        "        Cell t #= h.c;\n"
        "        return t.n;\n"
        "    }\n"
        "}\n"), 7);
}

// 1.1.4 — the transfer CLEARS the source's ownership bit, so the cell is
// freed exactly once across the whole scope: the taker drops it and the
// holder's own drop must not free it again. liveCount returning to its
// baseline rules out BOTH a leak and a double free.
TEST(UniformTransferTests, transferClearsSourceBitSoHolderDropIsNeutral) {
    EXPECT_EQ(runI32(std::string(kSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        {\n"
        "            Holder h = heap Holder();\n"
        "            h.put(heap Cell(7));\n"
        "            Cell t #= h.c;\n"
        "            if (t.n != 7) { return -1; }\n"
        "        }\n"
        "        int64 after = Cajeta.liveCount();\n"
        "        if (after != base) { return -2; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n"), 1);
}

// 1.1.1 twin — the same store from an array ELEMENT slot.
TEST(UniformTransferTests, sharpStoreFromOwnedElementTransfers) {
    EXPECT_EQ(runI32(std::string(kSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cell[] xs = heap Cell[2];\n"
        "        xs[0] #= heap Cell(9);\n"
        "        Cell t #= xs[0];\n"
        "        return t.n;\n"
        "    }\n"
        "}\n"), 9);
}

// ---- Unit 2 (spec 2.3): containers own their elements ------------------

std::string compileExpectError(const std::string& src,
                               const std::string& expectCode) {
    try {
        CajetaJit::compile(src, "test.D");
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), expectCode);
        return e.getMessage();
    } catch (const std::exception& e) {
        return e.what();
    }
    ADD_FAILURE() << "expected a compile error";
    return "";
}

// 2.1.1 — a plain owned local into a container is now an error naming `#v`.
TEST(UniformTransferTests, plainAddOfOwnedLocalIsTransferRequired) {
    std::string src = std::string(kSrc) +
        "import cajeta.collection.ArrayList;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        ArrayList<Cell> xs = heap ArrayList<Cell>();\n"
        "        Cell c = heap Cell(3);\n"
        "        xs.add(c);\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    std::string msg = compileExpectError(src, "CAJETA_ERROR_TRANSFER_REQUIRED");
    EXPECT_NE(msg.find("#c"), std::string::npos) << msg;
}

// 2.1.2 — the surrendered spelling compiles.
TEST(UniformTransferTests, sharpAddOfOwnedLocalCompiles) {
    EXPECT_EQ(runI32(std::string(kSrc) +
        "import cajeta.collection.ArrayList;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        ArrayList<Cell> xs = heap ArrayList<Cell>();\n"
        "        Cell c = heap Cell(3);\n"
        "        xs.add(#c);\n"
        "        return xs.get(0).n;\n"
        "    }\n"
        "}\n"), 3);
}

// 2.1.3 — a primitive element still needs no `#`: an int32 local has no drop
// entry, so the `#T` check never fires. This is the exemption that keeps the
// change from breaking every `ArrayList<int32>` in existence.
TEST(UniformTransferTests, primitiveElementAddNeedsNoSharp) {
    EXPECT_EQ(runI32(
        "package test;\n"
        "import cajeta.collection.ArrayList;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        ArrayList<int32> xs = heap ArrayList<int32>();\n"
        "        int32 n = 4;\n"
        "        xs.add(n);\n"
        "        xs.add(7);\n"
        "        return xs.get(0) + xs.get(1);\n"
        "    }\n"
        "}\n"), 11);
}

// The baseline Unit 3 will overturn: the DOUBLE sharp is still accepted
// today for a field source (it forwards the slot's mode verbatim). When
// 3.2.1 widens the diagnostic this must flip to a compile error — the test
// is kept and inverted there, not deleted, so the change is deliberate.
TEST(UniformTransferTests, doubleSharpFromFieldIsStillAcceptedBeforeUnit3) {
    EXPECT_EQ(runI32(std::string(kSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Holder h = heap Holder();\n"
        "        h.put(heap Cell(7));\n"
        "        Cell t #= #h.c;\n"
        "        return t.n;\n"
        "    }\n"
        "}\n"), 7);
}
