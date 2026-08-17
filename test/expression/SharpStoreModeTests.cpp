//
// stdlib-ownership-convention Unit 8 (plan 8.2.16-8.2.23) — what mode does a
// `#=` store RECORD?
//
// §4.6 made `#=` mandatory to receive a `#T` result, and enforcing it exposed
// five defects that all share one root: `#=` is a MODE-CARRYING store — it
// records whatever mode the source actually holds — but every consumer of the
// recorded flag defaults a NULL flag to const-1 (owned). Any source shape whose
// mode was not computed therefore claimed a title it did not have, and the
// receipt freed memory somebody else still owned.
//
// Each test here is the probe that found one of those defects, so a regression
// re-opens as a wrong RETURN VALUE (freed memory read back), not a crash.
//
// The last two go the other way: `#= obj.field` and `#= arr[i]` are guarded
// DETACHES and must stay that way. They are pinned because the obvious "fix"
// for the tests above — make every unproven source record a borrow — silently
// turns the stdlib's extraction idioms (Pair.takeFirst, ArrayList.remove,
// LinkedList.removeFirst, the HashMap rehash) into leaks.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

#include "cajeta/error/Exception.h"

using cajeta_test::CajetaJit;

namespace {

// A `Cell` whose `n` is the oracle: read it back through a path that a false
// title claim would have freed. 7 means the object is still alive; anything
// else is recycled heap.
const char* kCell =
    "package test;\n"
    "public final class Cell {\n"
    "    public int32 n;\n"
    "    public Cell(int32 v) { this.n = v; }\n"
    "}\n";

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

// Returns the error id, or "" when the source compiles.
std::string errorOf(const std::string& src) {
    try {
        CajetaJit::compile(src, "test.D");
        return "";
    } catch (cajeta::Exception& e) {
        return e.getErrorId();
    } catch (const std::exception&) {
        return "<non-cajeta-exception>";
    }
}

}  // namespace

// 8.2.16 — receiving a `#T` result whose T is an INTERFACE. An interface is a
// 24-byte fat-pointer body inline in a field or array element, but a plain
// pointer-sized slot as a local. The receipt stored the whole body through the
// slot pointer, scribbling over two neighbouring slots. Conforming code; it
// crashed.
TEST(SharpStoreModeTests, interfaceOwnedResultFitsItsLocalSlot) {
    EXPECT_EQ(runI32(
        "package test;\n"
        "public interface Shape {\n"
        "    public int32 area();\n"
        "}\n"
        "public final class Square implements Shape {\n"
        "    public int32 side;\n"
        "    public Square(int32 s) { this.side = s; }\n"
        "    public int32 area() { return this.side * this.side; }\n"
        "}\n"
        "public final class Maker {\n"
        "    public Maker() { }\n"
        "    public #Shape make(int32 s) { return #heap Square(s); }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Maker m #= heap Maker();\n"
        "        int32 guardA = 11;\n"
        "        Shape s #= m.make(3);\n"
        "        int32 guardB = 13;\n"
        "        return s.area() + guardA + guardB;\n"
        "    }\n"
        "}\n"), 9 + 11 + 13);
}

// 8.2.18 — `#=` off a BORROW ALIAS. The alias reaches neither the drop-entry
// branch (it has no entry) nor the formal branch (it is not a parameter), so
// the flag stayed null and the slot claimed the object outright. This is the
// shape behind the DnsCache SIGSEGV: `Cache.linkAtHead` stored a lent node into
// three slots, each recorded OWNED.
TEST(SharpStoreModeTests, sharpStoreFromABorrowAliasRecordsABorrow) {
    EXPECT_EQ(runI32(std::string(kCell) +
        "public final class Sink {\n"
        "    public Cell a;\n"
        "    public Sink() { }\n"
        "    public void keep(Cell maybe) { this.a #= maybe; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cell owner #= heap Cell(7);\n"
        "        {\n"
        "            Sink s #= heap Sink();\n"
        "            s.keep(owner);\n"
        "        }\n"
        "        return owner.n;\n"
        "    }\n"
        "}\n"), 7);
}

// 8.2.22 — 8.2.18's twin for CALL RESULTS. A call result has no scope entry to
// read a mode off, so the flag stayed null and a receipt of a BORROW-returning
// call claimed a title over memory the callee's receiver still owns. The
// callee's declared return stance is the answer: `#T` transfers, plain `T`
// lends.
TEST(SharpStoreModeTests, sharpStoreOfABorrowReturningCallRecordsABorrow) {
    EXPECT_EQ(runI32(std::string(kCell) +
        "public final class Holder {\n"
        "    public Cell c;\n"
        "    public Holder(int32 v) { this.c #= heap Cell(v); }\n"
        "    public Cell peek() { return this.c; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Holder h #= heap Holder(7);\n"
        "        { Cell borrowed #= h.peek(); }\n"
        "        return h.c.n;\n"
        "    }\n"
        "}\n"), 7);
}

// The other half of 8.2.22: a `#T`-returning call must still TRANSFER. A fix
// that made every call result record a borrow would leak instead of crash, and
// no runtime read would notice.
TEST(SharpStoreModeTests, sharpStoreOfAnOwnedReturningCallStillTransfers) {
    EXPECT_EQ(runI32(std::string(kCell) +
        "public final class Maker {\n"
        "    public Maker() { }\n"
        "    public #Cell make(int32 v) { return #heap Cell(v); }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cell kept;\n"
        "        {\n"
        "            Maker m #= heap Maker();\n"
        "            kept #= m.make(7);\n"
        "        }\n"
        "        return kept.n;\n"
        "    }\n"
        "}\n"), 7);
}

// 8.2.23 — the LOCAL twin of TransferFromBorrowTests 3.1.6. Whether a `#=`
// CONSUMES its source is a fact about the SOURCE, so it cannot depend on which
// side of the `#=` the destination sits. The demote was gated on the assignment
// spelling alone, so one dual-role formal compiled as a field store and was
// rejected as a local declaration.
TEST(SharpStoreModeTests, sharpDeclarationFromAPlainFormalStillCompiles) {
    EXPECT_EQ(errorOf(std::string(kCell) +
        "public final class Reader {\n"
        "    public Reader() { }\n"
        "    public int32 twice(Cell n) {\n"
        "        Cell x #= n;\n"
        "        Cell y #= n;\n"
        "        return x.n + y.n;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cell owner #= heap Cell(7);\n"
        "        Reader r #= heap Reader();\n"
        "        return r.twice(owner);\n"
        "    }\n"
        "}\n"), "");
}

// 8.2.23, the runtime half: the lent formal survives both stores and the caller
// still owns it afterwards.
TEST(SharpStoreModeTests, sharpDeclarationFromAPlainFormalKeepsTheCallersTitle) {
    EXPECT_EQ(runI32(std::string(kCell) +
        "public final class Reader {\n"
        "    public Reader() { }\n"
        "    public int32 twice(Cell n) {\n"
        "        Cell x #= n;\n"
        "        Cell y #= n;\n"
        "        return x.n + y.n;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cell owner #= heap Cell(7);\n"
        "        Reader r #= heap Reader();\n"
        "        int32 sum = r.twice(owner);\n"
        "        return sum + owner.n - 14;\n"
        "    }\n"
        "}\n"), 7);
}

// GUARD RAIL — `#= obj.field` is a guarded DETACH, not a lend. The field hands
// over the title it holds and gives it up, so the value outlives its former
// owner. Pair.takeFirst, LinkedList.removeFirst and Table's plan hand-offs are
// all this shape; making an unproven source record a borrow would leak them.
TEST(SharpStoreModeTests, sharpStoreFromAFieldDetachesTheTitle) {
    EXPECT_EQ(runI32(std::string(kCell) +
        "public final class Holder {\n"
        "    public Cell c;\n"
        "    public Holder(int32 v) { this.c #= heap Cell(v); }\n"
        "    public #Cell takeCell() { Cell out #= this.c; return #out; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cell kept;\n"
        "        {\n"
        "            Holder h #= heap Holder(7);\n"
        "            kept #= h.takeCell();\n"
        "        }\n"
        "        return kept.n;\n"
        "    }\n"
        "}\n"), 7);
}

// GUARD RAIL — and the detach is GUARDED: off a field that holds only a borrow
// it must not mint a second owner. 7 means the lender's object survived.
TEST(SharpStoreModeTests, sharpStoreFromABorrowingFieldMintsNoSecondOwner) {
    EXPECT_EQ(runI32(std::string(kCell) +
        "public final class Lender {\n"
        "    public Cell c;\n"
        "    public Lender() { }\n"
        "    public void lend(Cell maybe) { this.c #= maybe; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cell owner #= heap Cell(7);\n"
        "        Lender l #= heap Lender();\n"
        "        l.lend(owner);\n"
        "        { Cell b #= l.c; }\n"
        "        return owner.n;\n"
        "    }\n"
        "}\n"), 7);
}

// GUARD RAIL — the array twin. A take off a slot that holds only a borrow must
// not free the lender's object (ArrayList.remove and the BPlusTree splits ride
// on the same take).
TEST(SharpStoreModeTests, sharpStoreFromABorrowingSlotMintsNoSecondOwner) {
    EXPECT_EQ(runI32(std::string(kCell) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cell owner #= heap Cell(7);\n"
        "        Cell alias = owner;\n"
        "        Cell[] a = heap Cell[2];\n"
        "        a[0] #= alias;\n"
        "        { Cell b #= a[0]; }\n"
        "        return owner.n;\n"
        "    }\n"
        "}\n"), 7);
}
