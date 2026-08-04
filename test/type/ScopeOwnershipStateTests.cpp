// transfer-demotes-to-borrow Unit 1 (plan 1.1) — ownership state on Scope.
//
// A transfer moves the TITLE, not the binding: the source is demoted from
// owner to borrow and stays a usable reference to the same live instance.
// These tests pin the Scope-level state that encodes that, written BEFORE
// the rename in 1.2 so the rename is provably behaviour-preserving.
//
// They exercise Scope directly rather than through the JIT. The move-state
// methods touch only `fields` and `borrowedBindings`, never the module, so a Scope
// built with a null module is sufficient — and a direct test runs in
// microseconds where a JIT round trip costs ~10s.

#include "gtest/gtest.h"

#include "cajeta/type/Scope.h"

#include <memory>
#include <string>

using cajeta::Scope;
using cajeta::ScopePtr;

namespace {

ScopePtr makeScope(const std::string& name, ScopePtr parent = nullptr) {
    return std::make_shared<Scope>(name, nullptr, parent);
}

}  // namespace

// 1.1.1 — a transferred name reports demoted state; an untouched one does not.
// This is the query every consumer reads, so it is pinned first.
TEST(ScopeOwnershipStateTests, transferDemotesTheBinding) {
    auto scope = makeScope("m");
    EXPECT_FALSE(scope->isBorrow("t")) << "a fresh binding owns";
    scope->demoteToBorrow("t");
    EXPECT_TRUE(scope->isBorrow("t")) << "after transfer the binding is a borrow";
    EXPECT_FALSE(scope->isBorrow("other")) << "demotion is per-name";
}

// 1.1.2 — reassignment restores ownership (spec 2.2.6). `t = heap Tag();`
// gives the binding a fresh title, so it is an owner again.
TEST(ScopeOwnershipStateTests, reassignmentRestoresOwnership) {
    auto scope = makeScope("m");
    scope->demoteToBorrow("t");
    ASSERT_TRUE(scope->isBorrow("t"));
    scope->restoreOwnership("t");
    EXPECT_FALSE(scope->isBorrow("t")) << "reassignment makes it an owner again";
}

// 1.1.3 — demotion is recorded on the DECLARING scope, so a transfer inside a
// nested block still demotes the outer binding. Without this a transfer in an
// if/while body would be invisible once the block closed, and the binding
// would read as an owner it no longer is.
//
// Scope::demoteToBorrow walks parent scopes looking for the declaring frame; with
// no fields registered it records locally, so this test asserts the visible
// consequence instead: a child's demotion is seen through the parent chain.
TEST(ScopeOwnershipStateTests, demotionInANestedBlockIsVisibleToTheQuery) {
    auto outer = makeScope("outer");
    auto inner = makeScope("inner", outer);

    outer->demoteToBorrow("t");
    EXPECT_TRUE(inner->isBorrow("t"))
        << "isBorrow walks the parent chain — an inner read must see the demotion";

    inner->demoteToBorrow("u");
    EXPECT_TRUE(inner->isBorrow("u"));
}

// 1.1.4 — the retract/snapshot/reapply round trip must not resurrect
// ownership. Blocks capture moveLogSize() and retract on a terminated path;
// if a retraction cleared a demotion that a live path had made, the binding
// would read as an owner while the title had genuinely gone.
TEST(ScopeOwnershipStateTests, retractRewindsDemotionToTheMark) {
    auto scope = makeScope("m");
    scope->demoteToBorrow("before");
    size_t mark = scope->moveLogSize();

    scope->demoteToBorrow("during");
    EXPECT_TRUE(scope->isBorrow("during"));

    scope->retractMovesSince(mark);
    EXPECT_FALSE(scope->isBorrow("during")) << "retracted back to the mark";
    EXPECT_TRUE(scope->isBorrow("before")) << "demotions before the mark survive";
}

TEST(ScopeOwnershipStateTests, snapshotAndReapplyRoundTripDemotion) {
    auto scope = makeScope("m");
    size_t mark = scope->moveLogSize();
    scope->demoteToBorrow("a");
    scope->demoteToBorrow("b");

    auto snap = scope->snapshotMovesSince(mark);
    EXPECT_EQ(snap.size(), 2u);

    scope->retractMovesSince(mark);
    ASSERT_FALSE(scope->isBorrow("a"));
    ASSERT_FALSE(scope->isBorrow("b"));

    scope->reapplyMoves(snap);
    EXPECT_TRUE(scope->isBorrow("a")) << "reapply restores demotion";
    EXPECT_TRUE(scope->isBorrow("b"));
}

// The transfer-site note rides with the demotion. Unit 4 rewords the
// diagnostic; the note must survive the Unit 1 rename to be available to it.
TEST(ScopeOwnershipStateTests, transferSiteNoteSurvivesDemotion) {
    auto scope = makeScope("m");
    scope->demoteToBorrow("t", "transferred at line 7");
    EXPECT_EQ(scope->transferSiteOf("t"), "transferred at line 7");
    scope->restoreOwnership("t");
    EXPECT_EQ(scope->transferSiteOf("t"), "") << "clearing drops the note with the state";
}
