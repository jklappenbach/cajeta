// stdlib-ownership-convention U2 (spec 2.2, 2.5; plan 2.2.3) —
// `Optional.take()`, the owned counterpart to `get()`.
//
// `get()` returns a BORROW: ownership stays with the Optional. `#` on a
// borrow FORWARDS the mode it was handed rather than forcing a title
// (5.2.4), so `#opt.get()` is a transfer that quietly does nothing — the
// caller who needed the value to outlive the Optional does not get it.
//
// It is NOT a double free. An earlier revision of this file claimed one;
// OwnershipRuntimeProbeTests.classBorrowSurrenderedAtArgument measures the
// class shape as balanced, because a call-result local's drop entry is
// armed from the actual return flag. The defect is a silently-ineffective
// `#`, which is worth catching on its own terms.
//
// Two stdlib sites did exactly that and were invisible until the
// transfer-of-a-borrow check learned to read call ARGUMENTS:
//
//   * SharedPoolServer.worker — `TcpStream conn = next.get();` then
//     `runTurn(handler, inflight, #conn)`, where `Channel.receive` had
//     transferred the stream INTO the Optional.
//   * FilterStream.next — `T v = o.get();` then
//     `stack Optional<T>(true, #v)`, where ownership stays with the
//     source. That one wanted a LEND, and now takes one.
//
// Spec §2.5 says the safe spelling is the unmarked one and the sharp
// spelling is explicitly named, so the title-extracting variant is a
// separate method rather than a mode on `get()`.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

#include "cajeta/error/Exception.h"

using cajeta_test::CajetaJit;

namespace {

std::string makeSource(const std::string& body) {
    return "package test;\n"
           "import cajeta.lang.Optional;\n"
           "import cajeta.error.Throwable;\n"
           "import cajeta.error.NoOptionalValueException;\n"
           "public final class Cell {\n"
           "    public int32 n;\n"
           "    public Cell(int32 v) { this.n = v; }\n"
           "}\n"
           "public final class Holder {\n"
           "    Cell held;\n"
           "    public Holder(#Cell c) { this.held #= c; }\n"
           "    public int32 value() { return this.held.n; }\n"
           "}\n"
           "public final class Ut {\n"
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

}  // namespace

// take() on a present Optional yields the value, and the title comes with
// it: the result is accepted by a `#Cell` formal, which `#opt.get()` must
// not be. Single owner, no double free.
TEST(OptionalTakeTests, takeYieldsValueWithTitle) {
    EXPECT_EQ(runJit(
        "Optional<Cell> o = stack Optional<Cell>(true, #heap Cell(41));\n"
        "        Cell c #= o.take();\n"
        "        Holder h = heap Holder(#c);\n"
        "        return h.value() + 1;"), 42);
}

// The whole point of the new method: `#` on a `get()` result is rejected,
// while the same shape through `take()` compiles. This is the pair that
// makes the convention legible at the call site.
TEST(OptionalTakeTests, transferOfGetResultRejected) {
    std::string src = makeSource(
        "Optional<Cell> o = stack Optional<Cell>(true, #heap Cell(41));\n"
        "        Cell c = o.get();\n"
        "        Holder h = heap Holder(#c);\n"
        "        return h.value();");
    try {
        CajetaJit::compile(src, "test.Ut");
        ADD_FAILURE() << "expected `#` on a get() result to be rejected";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_MOVE_OF_BORROW");
        EXPECT_NE(e.getMessage().find("get()"), std::string::npos)
            << "message should name the borrow origin: " << e.getMessage();
    }
}

// take() clears `present`, so the Optional reports empty afterwards — the
// slot it no longer owns is not readable as if it did.
TEST(OptionalTakeTests, takeLeavesTheOptionalEmpty) {
    EXPECT_EQ(runJit(
        "Optional<Cell> o = stack Optional<Cell>(true, #heap Cell(7));\n"
        "        Cell c #= o.take();\n"
        "        if (o.isPresent()) { return -1; }\n"
        "        return c.n;"), 7);
}

// take() on empty throws NoOptionalValueException, like get(), and the
// message names the method that was called.
TEST(OptionalTakeTests, takeOnEmptyThrows) {
    EXPECT_EQ(runJit(
        "Optional<Cell> o = stack Optional<Cell>(false);\n"
        "        try {\n"
        "            Cell c #= o.take();\n"
        "            return -1;\n"
        "        } catch (NoOptionalValueException e) {\n"
        "            return 5;\n"
        "        }"), 5);
}

// get() is unchanged and still lends: reading through it, without `#`,
// keeps working and leaves the Optional present.
TEST(OptionalTakeTests, getStillLends) {
    EXPECT_EQ(runJit(
        "Optional<Cell> o = stack Optional<Cell>(true, #heap Cell(9));\n"
        "        int32 a = o.get().n;\n"
        "        if (!o.isPresent()) { return -1; }\n"
        "        return a + o.get().n;"), 18);
}
