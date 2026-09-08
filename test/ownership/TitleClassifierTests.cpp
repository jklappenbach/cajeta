//
// ownership-title-classifier Unit 1 — the classifier, measured.
//
// spec §2.1 has one row per shape; each row here is a `T x = <shape>;` line
// in a JIT-compiled program, observed by the audit-only hook in
// LocalVariableDeclaration (role Bind) and looked up by its line number. The
// assertion is the STATIC answer (family / answer / flag source / flags) the
// classifier produced for that node in that context — never reasoned from
// the row, always read from a compile.
//
// The policy table (spec §2.3) is a pure function of (shape, role); its
// error rows are asserted directly on hand-built shapes.
//
// Totality (spec §2.1, no Unknown): the switch in TitleClassifier.cpp has no
// default and builds with -Werror=switch, so an ExprKind the classifier does
// not name fails the compiler's build; the test below walks every ExprKind
// value and asserts each has a name, which pins the enumerator list itself.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>
#include <vector>

#include "cajeta/ownership/TitleClassifier.h"

using cajeta_test::CajetaJit;
using namespace cajeta::ownership;
using cajeta::ExprKind;

namespace {

// 1-based line of the first line containing `needle`.
int lineOf(const std::string& src, const std::string& needle) {
    size_t pos = src.find(needle);
    if (pos == std::string::npos) return -1;
    int line = 1;
    for (size_t i = 0; i < pos; ++i) if (src[i] == '\n') ++line;
    return line;
}

const TitleShapeRecord* recordAt(int line, ConsumerRole role = ConsumerRole::Bind) {
    for (const auto& r : TitleShapeAudit::records()) {
        if (r.line == line && r.role == role && r.holder.rfind("test.A", 0) == 0) return &r;
    }
    return nullptr;
}

// Every record, for a failure message.
std::string dumpRecords() {
    std::string out;
    for (const auto& r : TitleShapeAudit::records()) {
        out += "  line " + std::to_string(r.line) + " " + toString(r.kind) + " "
            + toString(r.family) + "/" + toString(r.answer) + "/" + toString(r.source)
            + " role=" + toString(r.role) + " in " + r.holder + "\n";
    }
    return out.empty() ? std::string("  (none)\n") : out;
}

struct AuditScope {
    AuditScope() { TitleShapeAudit::clear(); TitleShapeAudit::setEnabled(true); }
    ~AuditScope() { TitleShapeAudit::setEnabled(false); TitleShapeAudit::clear(); }
};

const char* PRE =
    "package test;\n"
    "import cajeta.lang.Cajeta;\n"
    "import cajeta.lang.String;\n"
    "public final class A {\n"
    "    static class Cell {\n"
    "        public int32 v;\n"
    "        public Cell(int32 v) { this.v = v; return; }\n"
    "    }\n"
    "    static class Holder {\n"
    "        public Cell a;\n"
    "        public Cell b;\n"
    "        public String name;\n"
    "        public Holder() {\n"
    "            this.a #= heap Cell(1);\n"
    "            this.b #= heap Cell(2);\n"
    "            this.name #= \"hello\" + 1;\n"
    "            return;\n"
    "        }\n"
    "        public int32 selfProbe() {\n"
    "            Holder t = this; // ROW this\n"
    "            return t.a.v;\n"
    "        }\n"
    "    }\n"
    "    static #Cell fresh(int32 v) { return heap Cell(v); }\n"
    "    static Cell lend(Holder h) { return h.b; }\n"
    "    static #String mkOwned(int32 n) { return \"s\" + n; }\n";

#define EXPECT_ROW(rec, fam, ans, src_)                                          \
    do {                                                                         \
        ASSERT_NE((rec), nullptr) << "no audit record on that line; records:\n" << dumpRecords();             \
        EXPECT_STREQ(toString((rec)->family), toString(TitleFamily::fam)) << dumpRecords(); \
        EXPECT_STREQ(toString((rec)->answer), toString(TitleAnswer::ans)) << dumpRecords(); \
        EXPECT_STREQ(toString((rec)->source), toString(TitleSource::src_)) << dumpRecords(); \
    } while (0)

} // namespace

// The read shapes: literal, bare local (with and without an entry), formal,
// `this`, field, element, and a reference cast peeled to the field read.
TEST(TitleClassifierTests, readShapesAreBorrows) {
    AuditScope audit;
    std::string src = std::string(PRE) +
        "    static int32 probe(Holder h, Cell p, Cell[] arr) {\n"
        "        String lit = \"lit\"; // ROW literal\n"
        "        Cell k0 = h.a; // ROW field\n"
        "        Cell k1 = k0; // ROW local-noentry\n"
        "        Cell o = heap Cell(9); // ROW heap\n"
        "        Cell k2 = o; // ROW local-entry\n"
        "        Cell k3 = p; // ROW formal\n"
        "        Cell k4 = arr[0]; // ROW element\n"
        "        Cell k5 = (Cell) h.b; // ROW cast\n"
        "        int32 n = 1; // ROW scalar\n"
        "        return k1.v + k2.v + k3.v + k4.v + k5.v + n + lit.byteLength();\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Holder h = heap Holder();\n"
        "        Cell[] arr = [heap Cell(3)];\n"
        "        return A.probe(h, h.a, arr);\n"
        "    }\n"
        "}\n";
    ASSERT_NE(CajetaJit::compile(src.c_str(), "test.A"), nullptr);
    EXPECT_ROW(recordAt(lineOf(src, "ROW literal")), Literal, Borrow, None);
    EXPECT_TRUE(recordAt(lineOf(src, "ROW literal"))->flags & TitleShape::kString);
    EXPECT_ROW(recordAt(lineOf(src, "ROW field")), FieldRead, Borrow, None);
    {
        auto* r = recordAt(lineOf(src, "ROW local-noentry"));
        EXPECT_ROW(r, LocalRead, Borrow, None);
        EXPECT_FALSE(r->flags & TitleShape::kHasEntry);
    }
    EXPECT_ROW(recordAt(lineOf(src, "ROW heap")), Fresh, Owned, None);
    {
        auto* r = recordAt(lineOf(src, "ROW local-entry"));
        EXPECT_ROW(r, LocalRead, Borrow, None);
        EXPECT_TRUE(r->flags & TitleShape::kHasEntry);
    }
    {
        auto* r = recordAt(lineOf(src, "ROW formal"));
        EXPECT_ROW(r, LocalRead, Borrow, None);
        EXPECT_TRUE(r->flags & TitleShape::kIsParam);
        EXPECT_FALSE(r->flags & TitleShape::kTransferredParam);
    }
    EXPECT_ROW(recordAt(lineOf(src, "ROW element")), ElementRead, Borrow, None);
    EXPECT_ROW(recordAt(lineOf(src, "ROW cast")), FieldRead, Borrow, None);
    EXPECT_ROW(recordAt(lineOf(src, "ROW this")), ThisRead, Borrow, None);
    EXPECT_ROW(recordAt(lineOf(src, "ROW scalar")), Scalar, Scalar, None);
}

// The producing shapes: heap / stack construction, a concat, an owned call,
// a plain call (rides its flag), a map literal.
TEST(TitleClassifierTests, producingShapesAreOwnedOrRuntime) {
    AuditScope audit;
    std::string src = std::string(PRE) +
        "    static int32 probe(Holder h) {\n"
        "        Cell n = heap Cell(2); // ROW heap\n"
        "        Cell s = stack Cell(3); // ROW stack\n"
        "        Cell fr #= A.fresh(4); // ROW owned-call\n"
        "        Cell pl = A.lend(h); // ROW plain-call\n"
        "        String cc = \"a\" + n.v; // ROW concat\n"
        "        return n.v + s.v + fr.v + pl.v + cc.byteLength();\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Holder h = heap Holder();\n"
        "        return A.probe(h);\n"
        "    }\n"
        "}\n";
    ASSERT_NE(CajetaJit::compile(src.c_str(), "test.A"), nullptr);
    EXPECT_ROW(recordAt(lineOf(src, "ROW heap")), Fresh, Owned, None);
    {
        auto* r = recordAt(lineOf(src, "ROW stack"));
        EXPECT_ROW(r, Fresh, StackBound, None);
        EXPECT_TRUE(r->flags & TitleShape::kStack);
    }
    // `#=` wraps the call in a move; the move forwards the call's answer,
    // which is the return flag even for a `#R` callee (it may `return #=`).
    {
        auto* r = recordAt(lineOf(src, "ROW owned-call"));
        EXPECT_ROW(r, Move, Runtime, ReturnFlag);
        EXPECT_TRUE(r->flags & TitleShape::kOwnedDecl);
    }
    EXPECT_ROW(recordAt(lineOf(src, "ROW plain-call")), CallResult, Runtime, ReturnFlag);
    {
        auto* r = recordAt(lineOf(src, "ROW concat"));
        ASSERT_NE(r, nullptr);
        EXPECT_EQ(toString(r->family), toString(TitleFamily::Concat));
        EXPECT_TRUE(r->flags & TitleShape::kString);
        // Arena or heap is the declaration's decision; both are concat rows.
        EXPECT_TRUE(r->answer == TitleAnswer::Owned || r->answer == TitleAnswer::StackBound);
        if (r->answer == TitleAnswer::StackBound) EXPECT_TRUE(r->flags & TitleShape::kArena);
    }
}

// Moves: the inner shape decides the runtime source — a local's entry, a
// formal's word bit, a field's slot; a `#=` of an entry-less local records a
// borrow.
TEST(TitleClassifierTests, movesForwardTheInnerSource) {
    AuditScope audit;
    std::string src = std::string(PRE) +
        "    static int32 probe(Holder h, Cell p, String s) {\n"
        "        Cell o = heap Cell(9);\n"
        "        Cell m1 #= o; // ROW move-local\n"
        "        Cell m2 #= p; // ROW move-formal\n"
        "        String ms #= s; // ROW move-string-formal\n"
        "        Cell m3 #= h.a; // ROW move-field\n"
        "        Cell m5 #= A.lend(h); // ROW move-call\n"
        "        int8[] mb #= s.toBytes(); // ROW move-native\n"
        "        return m1.v + m2.v + m3.v + m5.v + ms.byteLength() + (int32) mb.count();\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Holder h = heap Holder();\n"
        "        return A.probe(h, h.a, h.name);\n"
        "    }\n"
        "}\n";
    ASSERT_NE(CajetaJit::compile(src.c_str(), "test.A"), nullptr);
    EXPECT_ROW(recordAt(lineOf(src, "ROW move-local")), Move, Runtime, DropEntry);
    // A class formal has its own drop entry (armed from the word at entry), so
    // the move reads THAT; a String formal has none, so it reads the word bit.
    EXPECT_ROW(recordAt(lineOf(src, "ROW move-formal")), Move, Runtime, DropEntry);
    EXPECT_ROW(recordAt(lineOf(src, "ROW move-string-formal")), Move, Runtime, TransferWord);
    EXPECT_ROW(recordAt(lineOf(src, "ROW move-field")), Move, Runtime, Slot);
    EXPECT_ROW(recordAt(lineOf(src, "ROW move-call")), Move, Runtime, ReturnFlag);
    // A `` (body-less) callee stores no return flag: its declared
    // stance is the answer, statically — never a read of a stale TLS.
    EXPECT_ROW(recordAt(lineOf(src, "ROW move-native")), Move, Owned, None);
}

// Conditionals: equal static arms fold; mixed arms are decided by the phi.
TEST(TitleClassifierTests, conditionalsJoinTheirArms) {
    AuditScope audit;
    std::string src = std::string(PRE) +
        "    static int32 probe(Holder h, boolean c) {\n"
        "        Cell cb = c ? h.a : h.b; // ROW both-borrow\n"
        "        Cell cm = c ? heap Cell(5) : h.a; // ROW mixed\n"
        "        Cell co = c ? heap Cell(6) : heap Cell(7); // ROW both-owned\n"
        "        return cb.v + cm.v + co.v;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Holder h = heap Holder();\n"
        "        return A.probe(h, true);\n"
        "    }\n"
        "}\n";
    ASSERT_NE(CajetaJit::compile(src.c_str(), "test.A"), nullptr);
    EXPECT_ROW(recordAt(lineOf(src, "ROW both-borrow")), Conditional, Borrow, None);
    EXPECT_ROW(recordAt(lineOf(src, "ROW mixed")), Conditional, Runtime, ArmPhi);
    EXPECT_ROW(recordAt(lineOf(src, "ROW both-owned")), Conditional, Owned, None);
}

// The policy table's error rows (spec §2.3), on hand-built shapes.
TEST(TitleClassifierTests, policyRejectsWhatEachRoleMustReject) {
    auto shape = [](TitleFamily f, TitleAnswer a, uint32_t flags = 0) {
        TitleShape s;
        s.family = f; s.answer = a; s.flags = flags; s.label = labelOfFamily(f);
        return s;
    };
    // `#T` return: a field read, a literal, an element read, `this`, a stack
    // value, a borrow-origin local are rejected; an owned local forwards its
    // entry; a formal its word bit; a fresh value passes.
    EXPECT_STREQ(policy(shape(TitleFamily::FieldRead, TitleAnswer::Borrow), ConsumerRole::ReturnOwned).error,
                 "CAJETA_ERROR_OWNED_RETURN_OF_BORROW");
    EXPECT_STREQ(policy(shape(TitleFamily::Literal, TitleAnswer::Borrow), ConsumerRole::ReturnOwned).error,
                 "CAJETA_ERROR_OWNED_RETURN_OF_BORROW");
    EXPECT_STREQ(policy(shape(TitleFamily::ThisRead, TitleAnswer::Borrow), ConsumerRole::ReturnOwned).error,
                 "CAJETA_ERROR_OWNED_RETURN_OF_BORROWED_THIS");
    EXPECT_STREQ(policy(shape(TitleFamily::Fresh, TitleAnswer::StackBound, TitleShape::kStack), ConsumerRole::ReturnOwned).error,
                 "CAJETA_ERROR_STACK_RETURN_ESCAPES");
    EXPECT_STREQ(policy(shape(TitleFamily::LocalRead, TitleAnswer::Borrow, TitleShape::kBorrowOrigin), ConsumerRole::ReturnOwned).error,
                 "CAJETA_ERROR_OWNED_RETURN_OF_BORROW");
    // Unit 6 rows: a static owner's entry is the constant title; an entry
    // armed at run time (kRuntimeOwner) is read; a `#` formal holds the
    // frame's title; a plain formal WITHOUT an entry (a String) holds
    // nothing to transfer; a plain formal with one forwards its entry.
    {
        auto v = policy(shape(TitleFamily::LocalRead, TitleAnswer::Borrow, TitleShape::kHasEntry), ConsumerRole::ReturnOwned);
        EXPECT_EQ(v.error, nullptr);
        EXPECT_EQ(v.answer, TitleAnswer::Owned);
    }
    {
        auto v = policy(shape(TitleFamily::LocalRead, TitleAnswer::Borrow,
                              TitleShape::kHasEntry | TitleShape::kRuntimeOwner), ConsumerRole::ReturnOwned);
        EXPECT_EQ(v.error, nullptr);
        EXPECT_EQ(v.answer, TitleAnswer::Runtime);
        EXPECT_EQ(v.source, TitleSource::DropEntry);
    }
    {
        auto v = policy(shape(TitleFamily::LocalRead, TitleAnswer::Borrow,
                              TitleShape::kIsParam | TitleShape::kTransferredParam), ConsumerRole::ReturnOwned);
        EXPECT_EQ(v.error, nullptr);
        EXPECT_EQ(v.answer, TitleAnswer::Owned);
    }
    EXPECT_STREQ(policy(shape(TitleFamily::LocalRead, TitleAnswer::Borrow, TitleShape::kIsParam), ConsumerRole::ReturnOwned).error,
                 "CAJETA_ERROR_BORROW_PARAM_ESCAPES");
    {
        auto v = policy(shape(TitleFamily::LocalRead, TitleAnswer::Borrow,
                              TitleShape::kIsParam | TitleShape::kHasEntry | TitleShape::kRuntimeOwner), ConsumerRole::ReturnOwned);
        EXPECT_EQ(v.error, nullptr);
        EXPECT_EQ(v.source, TitleSource::DropEntry);
    }
    EXPECT_STREQ(policy(shape(TitleFamily::LocalRead, TitleAnswer::Borrow, TitleShape::kStack | TitleShape::kHasEntry), ConsumerRole::ReturnOwned).error,
                 "CAJETA_ERROR_STACK_RETURN_ESCAPES");
    EXPECT_STREQ(policy(shape(TitleFamily::LocalRead, TitleAnswer::Borrow), ConsumerRole::ReturnOwned).error,
                 "CAJETA_ERROR_OWNED_RETURN_OF_BORROW");   // no entry, no origin: a lend
    EXPECT_EQ(policy(shape(TitleFamily::Literal, TitleAnswer::Borrow, TitleShape::kString), ConsumerRole::ReturnOwned).error,
              nullptr);                                    // a String literal is adopted (static wrapper)
    EXPECT_EQ(policy(shape(TitleFamily::Fresh, TitleAnswer::Owned), ConsumerRole::ReturnOwned).error, nullptr);
    // Plain return: a fresh value and an owned local are rejected; a call
    // rides; a borrow is a borrow.
    EXPECT_STREQ(policy(shape(TitleFamily::Fresh, TitleAnswer::Owned), ConsumerRole::ReturnPlain).error,
                 "CAJETA_ERROR_FRESH_RETURN_NEEDS_TRANSFER");
    EXPECT_STREQ(policy(shape(TitleFamily::LocalRead, TitleAnswer::Borrow, TitleShape::kHasEntry), ConsumerRole::ReturnPlain).error,
                 "CAJETA_ERROR_FRESH_RETURN_NEEDS_TRANSFER");
    EXPECT_EQ(policy(shape(TitleFamily::CallResult, TitleAnswer::Runtime), ConsumerRole::ReturnPlain).error, nullptr);
    EXPECT_EQ(policy(shape(TitleFamily::FieldRead, TitleAnswer::Borrow), ConsumerRole::ReturnPlain).error, nullptr);
    // `#T` formal: proven borrows are rejected (5.8 includes an entry-less
    // local and a borrowed parameter); owned and runtime values pass.
    EXPECT_STREQ(policy(shape(TitleFamily::FieldRead, TitleAnswer::Borrow), ConsumerRole::ArgOwned).error,
                 "CAJETA_ERROR_TRANSFER_REQUIRED");
    EXPECT_STREQ(policy(shape(TitleFamily::LocalRead, TitleAnswer::Borrow), ConsumerRole::ArgOwned).error,
                 "CAJETA_ERROR_TRANSFER_REQUIRED");
    EXPECT_STREQ(policy(shape(TitleFamily::LocalRead, TitleAnswer::Borrow, TitleShape::kIsParam), ConsumerRole::ArgOwned).error,
                 "CAJETA_ERROR_TRANSFER_REQUIRED");
    EXPECT_STREQ(policy(shape(TitleFamily::CallResult, TitleAnswer::Borrow, TitleShape::kView), ConsumerRole::ArgOwned).error,
                 "CAJETA_ERROR_TRANSFER_REQUIRED");
    EXPECT_EQ(policy(shape(TitleFamily::CallResult, TitleAnswer::Runtime), ConsumerRole::ArgOwned).error, nullptr);
    EXPECT_EQ(policy(shape(TitleFamily::Fresh, TitleAnswer::Owned), ConsumerRole::ArgOwned).error, nullptr);
    EXPECT_EQ(policy(shape(TitleFamily::Move, TitleAnswer::Runtime), ConsumerRole::ArgOwned).error, nullptr);
    // Roles with no rejection row pass everything through.
    EXPECT_EQ(policy(shape(TitleFamily::FieldRead, TitleAnswer::Borrow), ConsumerRole::Bind).error, nullptr);
    EXPECT_EQ(policy(shape(TitleFamily::Fresh, TitleAnswer::Owned), ConsumerRole::StoreSlot).error, nullptr);
    // A scalar has no title in any role.
    for (int r = 0; r < (int) ConsumerRole::Count; ++r) {
        EXPECT_EQ(policy(shape(TitleFamily::Scalar, TitleAnswer::Scalar), (ConsumerRole) r).error, nullptr);
    }
}

// Totality: every ExprKind enumerator has a name (the list itself is pinned;
// the classifier's switch over it is enforced at build time by -Werror=switch),
// and every family has a label.
TEST(TitleClassifierTests, everyKindAndFamilyIsNamed) {
    for (int k = 0; k < (int) ExprKind::Count; ++k) {
        EXPECT_STRNE(toString((ExprKind) k), "?") << "ExprKind " << k;
    }
    EXPECT_EQ((int) ExprKind::Count, 32) << "a new Expression subclass: add its kind, its case, and its row";
    for (int f = 0; f < (int) TitleFamily::Count; ++f) {
        EXPECT_STRNE(labelOfFamily((TitleFamily) f), "") << "family " << f;
        EXPECT_STRNE(toString((TitleFamily) f), "?") << "family " << f;
    }
}

// The audit is off by default and costs nothing: no records without the
// switch.
TEST(TitleClassifierTests, auditIsOffByDefault) {
    TitleShapeAudit::clear();
    std::string src = std::string(PRE) +
        "    public static int32 run() {\n"
        "        Holder h = heap Holder();\n"
        "        Cell k = h.a;\n"
        "        return k.v;\n"
        "    }\n"
        "}\n";
    ASSERT_NE(CajetaJit::compile(src.c_str(), "test.A"), nullptr);
    EXPECT_TRUE(TitleShapeAudit::records().empty());
}
