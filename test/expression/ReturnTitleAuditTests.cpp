// stdlib-ownership-convention Unit 8 (8.1.1) — enumerate the ride-through
// sites: plain-return methods whose result actually carries a title.
//
// Unit 8 asks whether the RETURN still carries the ownership decision, and
// 8.1.1 is the number that decision turns on. The evidence that the signal is
// weak is already on the record: a plain return can carry a title (2.2.6),
// every `#`-returning interface method had `returnsOwnership == false` and
// nothing broke (2.2.5), and four `@Native` `String` methods declared plain
// returns while transferring (2.2.1). If ride-through is COMMON the
// producer/view contract is already fiction; if it is RARE those sites are
// bugs and Unit 4 fixes them.
//
// The instrument is the compiler's own decision, not a source-shape guess:
// `ReturnStatement::generateCode` computes `returnTitleFlag` for every return,
// and that value IS the title the caller will see. CLAUDE.md §5 — ownership
// behaviour is measured, never reasoned about — and 3.3.3 is the standing
// proof that a static audit keys on shapes it recognises while the compiler
// sees every one.
//
// Both directions are asserted, per CLAUDE.md §5: tests that the audit FIRES
// and tests that it does NOT. A predicate that silently disabled a whole check
// read as a clean run for an hour once already.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <string>
#include <vector>

#include "cajeta/ownership/ReturnTitleAudit.h"

using cajeta::ownership::ReturnTitleAudit;
using cajeta::ownership::ReturnTitleRecord;
using cajeta::ownership::TitleCarry;
using cajeta::ownership::TitleVia;
using cajeta_test::CajetaJit;

namespace {

const char* kCellSrc =
    "package test;\n"
    "public class Cell {\n"
    "    public int32 n;\n"
    "    public Cell(int32 nn) { this.n = nn; }\n"
    "}\n";

// Compile with the audit on, and hand back only the records for the test's own
// classes — a compile drags in whatever stdlib bodies it touches, and those are
// the sweep's subject, not this test's.
std::vector<ReturnTitleRecord> auditCompile(const std::string& src) {
    ReturnTitleAudit::setEnabled(true);
    ReturnTitleAudit::clear();
    try {
        CajetaJit::compile(src, "test.D");
    } catch (...) {
        ReturnTitleAudit::setEnabled(false);
        throw;
    }
    std::vector<ReturnTitleRecord> mine;
    for (const auto& r : ReturnTitleAudit::records()) {
        if (r.className.rfind("test.", 0) == 0) mine.push_back(r);
    }
    ReturnTitleAudit::setEnabled(false);
    return mine;
}

const ReturnTitleRecord* find(const std::vector<ReturnTitleRecord>& recs,
                              const std::string& cls,
                              const std::string& method) {
    for (const auto& r : recs) {
        if (r.className == cls && r.methodName == method) return &r;
    }
    return nullptr;
}

}  // namespace

// The `viaPlain` shape, from SignatureAbiTests.tailCallThroughPlainReturnKeepsTitle:
// a plain-return wrapper tail-calling a `#` method rides the inner flag out. The
// caller of `viaPlain` receives a TITLE from a signature that says borrow.
TEST(ReturnTitleAuditTests, plainReturnTailCallIsEnumerated) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static #Cell fresh() { return heap Cell(7); }\n"
        "    public static Cell viaPlain() { return D.fresh(); }\n"
        "    public static int32 run() {\n"
        "        Cell m = viaPlain();\n"
        "        return m.n;\n"
        "    }\n"
        "}\n";
    auto recs = auditCompile(src);
    const ReturnTitleRecord* r = find(recs, "test.D", "viaPlain");
    ASSERT_NE(r, nullptr) << "the ride-through site was not enumerated";
    EXPECT_EQ(r->carry, TitleCarry::RuntimeFlag);
    EXPECT_EQ(r->via, TitleVia::CallRide);
    // WHICH callee is ridden is the difference between "a title escapes a
    // plain signature" and "the decision moved one frame along", so the count
    // is only as good as this field.
    EXPECT_EQ(r->calleeKey, "test.D.fresh");
    EXPECT_TRUE(r->calleeOwned);
}

// The other side of that split, and the reason it exists: a fluent builder
// chaining `return this.other(...)` also rides a flag, but the callee is
// itself plain — nothing has escaped, the decision is just deferred. Counting
// these beside the `viaPlain` shape would inflate 8.1.1 with the single most
// common idiom in `JsonWriter`.
TEST(ReturnTitleAuditTests, chainedPlainCalleeIsMarkedNotOwning) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static Cell inner(Cell c) { return c; }\n"
        "    public static Cell outer(Cell c) { return D.inner(c); }\n"
        "    public static int32 run() {\n"
        "        Cell m = heap Cell(3);\n"
        "        Cell got = D.outer(m);\n"
        "        return got.n;\n"
        "    }\n"
        "}\n";
    auto recs = auditCompile(src);
    const ReturnTitleRecord* r = find(recs, "test.D", "outer");
    ASSERT_NE(r, nullptr) << "the chained call was not enumerated";
    EXPECT_EQ(r->via, TitleVia::CallRide);
    EXPECT_EQ(r->calleeKey, "test.D.inner");
    EXPECT_FALSE(r->calleeOwned)
        << "a plain callee must not be counted as handing out a title";
}

// The control the enumeration lives or dies by: a view return (`return this.c`)
// is the CONFORMING plain return — interior state, flag always borrow. If this
// were reported the count would be the whole library and mean nothing.
TEST(ReturnTitleAuditTests, viewReturnIsNotEnumerated) {
    std::string src = std::string(kCellSrc) +
        "public class Bank {\n"
        "    public Cell c;\n"
        "    public Bank(#Cell v) { this.c #= v; }\n"
        "    public Cell get() { return this.c; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cell v = heap Cell(4);\n"
        "        Bank b = heap Bank(#v);\n"
        "        Cell got = b.get();\n"
        "        return got.n;\n"
        "    }\n"
        "}\n";
    auto recs = auditCompile(src);
    EXPECT_EQ(find(recs, "test.Bank", "get"), nullptr)
        << "a view return was counted as a ride-through";
}

// A `#`-returning method is not the subject: it DECLARES the transfer, so the
// signal is doing its job. Only plain returns are enumerated.
TEST(ReturnTitleAuditTests, ownedReturnIsNotEnumerated) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static #Cell fresh() { return heap Cell(7); }\n"
        "    public static int32 run() {\n"
        "        Cell m = D.fresh();\n"
        "        return m.n;\n"
        "    }\n"
        "}\n";
    auto recs = auditCompile(src);
    EXPECT_EQ(find(recs, "test.D", "fresh"), nullptr)
        << "a declared `#` return was counted";
}

// A returned FORMAL is the pass-through shape (Statement.cpp:2239): the
// parameter's runtime flag rides out, so what the caller gets depends on what
// the caller of the wrapper handed IN. Runtime-variable by construction.
TEST(ReturnTitleAuditTests, returnedFormalIsEnumerated) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static Cell pass(Cell c) { return c; }\n"
        "    public static int32 run() {\n"
        "        Cell m = heap Cell(5);\n"
        "        Cell got = D.pass(m);\n"
        "        return got.n;\n"
        "    }\n"
        "}\n";
    auto recs = auditCompile(src);
    const ReturnTitleRecord* r = find(recs, "test.D", "pass");
    ASSERT_NE(r, nullptr) << "the formal pass-through was not enumerated";
    EXPECT_EQ(r->carry, TitleCarry::RuntimeFlag);
    EXPECT_EQ(r->via, TitleVia::FormalPassThrough);
}

// `return #x` under a PLAIN return type: a surrender from a signature that
// promises a borrow.
//
// MEASURED CORRECTION — this test was written expecting a STATIC title (the
// constant-1 fallback at Statement.cpp:2710) and the compiler answered
// `carry=runtime`: `return #m` forwards the local's own drop-entry flag, so
// `#` no more asserts a title in the return position than it does in the
// argument position (CLAUDE.md §2.2). The expectation was reasoned; the
// compiler was measured; the compiler wins. It also showed the classifier
// keying on the runtime helper alone would file every `return #x` as a formal
// pass-through, which is why the spelling is now checked first.
TEST(ReturnTitleAuditTests, moveReturnUnderPlainTypeIsEnumerated) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static Cell viaMove() {\n"
        "        Cell m = heap Cell(9);\n"
        "        return #m;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Cell got = D.viaMove();\n"
        "        return got.n;\n"
        "    }\n"
        "}\n";
    auto recs = auditCompile(src);
    const ReturnTitleRecord* r = find(recs, "test.D", "viaMove");
    ASSERT_NE(r, nullptr) << "`return #x` under a plain return was not enumerated";
    EXPECT_EQ(r->carry, TitleCarry::RuntimeFlag);
    EXPECT_EQ(r->via, TitleVia::Move);
}

// The audit is OFF by default: an instrument that costs a record per return on
// every build is one nobody leaves in. Also the null-result validation — a
// silent audit must mean "disabled", not "broken", so the same source that
// reports above reports nothing here.
TEST(ReturnTitleAuditTests, disabledAuditRecordsNothing) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static #Cell fresh() { return heap Cell(7); }\n"
        "    public static Cell viaPlain() { return D.fresh(); }\n"
        "    public static int32 run() {\n"
        "        Cell m = viaPlain();\n"
        "        return m.n;\n"
        "    }\n"
        "}\n";
    ReturnTitleAudit::setEnabled(false);
    ReturnTitleAudit::clear();
    CajetaJit::compile(src, "test.D");
    EXPECT_TRUE(ReturnTitleAudit::records().empty());
}
