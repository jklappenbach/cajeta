//
// Gap 4 (MemoryModel.md § Known gaps): alias-mutation invalidates
// outstanding borrows.
//
// Cajeta's path-based borrow tracker today catches use-after-move
// (PathBorrowTests covers that). What it does NOT catch:
//
//     Person p = heap Person { name: "Bob" };
//     String  n = p.name;              // borrow into p.name
//     p.name  = #"Charlie";            // mutates the borrowed slot
//     // `n` now dangles — accepted today, should be rejected.
//
// Fix outline: extend Scope's path machinery with a second set
// tracking LIVE READ-BORROWS. On a borrowing init (`Foo b = a.f`),
// record the borrowed path. On a move-assign through a path
// (`a.f = #other`), reject if any live borrow's path prefix-matches.
//
// These tests are  until the live-borrow pass lands —
// enabling them now would flag CI red on a gap that's queued for its
// own session. Once the live-borrow tracker exists, drop the
//  prefix.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {

void expectAliasMutationError(const std::string& source) {
    try {
        CajetaJit::compile(source, "test.A");
        FAIL() << "expected alias-mutation error but compile succeeded";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_USE_AFTER_MOVE");
    } catch (std::exception& e) {
        FAIL() << "expected cajeta::Exception, got std::exception: " << e.what();
    }
}

} // namespace

// A live String borrow into p.name; then p.name is reassigned via
// move. The borrow's referent is gone — accepting this leaves a
// dangling alias. The compile should reject the write.
TEST(AliasMutationBorrowTests, writeThroughAliasInvalidatesLiveBorrow) {
    auto src =
        "package test;\n"
        "public class Person { String name; "
        "  public Person(String n) { this.name = n; } }\n"
        "public final class A {\n"
        "    public static int32 run() {\n"
        "        Person p = heap Person(\"Bob\");\n"
        "        String alias = p.name;\n"        // live read-borrow on "p.name"
        "        p.name = #\"Charlie\";\n"       // invalidates alias
        "        return 0;\n"
        "    }\n"
        "}\n";
    expectAliasMutationError(src);
}

// Same shape but the write goes through a prefix of the borrowed
// path. Reassigning the whole `p` clobbers `p.name` too, so any
// live borrow rooted at `p.*` is invalidated.
TEST(AliasMutationBorrowTests, writeToPrefixInvalidatesNestedBorrow) {
    auto src =
        "package test;\n"
        "public class Person { String name; "
        "  public Person(String n) { this.name = n; } }\n"
        "public final class A {\n"
        "    public static int32 run() {\n"
        "        Person p = heap Person(\"Bob\");\n"
        "        String alias = p.name;\n"        // borrows p.name
        "        p = heap Person(\"Charlie\");\n" // prefix write
        "        return 0;\n"
        "    }\n"
        "}\n";
    expectAliasMutationError(src);
}

// Negative case (should compile): write to an UNRELATED path while a
// borrow into a different path is live. p.age and p.name are
// disjoint, so a write to one doesn't affect a borrow of the other.
// (Pin once the live-borrow tracker lands — guards against
// over-zealous invalidation.)
TEST(AliasMutationBorrowTests, writeToDisjointPathLeavesBorrowIntact) {
    auto src =
        "package test;\n"
        "public class Person { String name; int32 age; "
        "  public Person(String n) { this.name = n; this.age = 0; } }\n"
        "public final class A {\n"
        "    public static int32 run() {\n"
        "        Person p = heap Person(\"Bob\");\n"
        "        String alias = p.name;\n"        // borrows p.name
        "        p.age = 42;\n"                   // disjoint write
        "        return p.age;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.A");
    auto run = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(run(), 42);
}
