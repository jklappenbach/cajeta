//
// Alias-mutation: reassigning a field that a local still aliases.
//
// HISTORY: this file once asserted that reassigning a borrowed slot
// (`String n = p.name; p.name = #"Charlie";`) was a use-after-move
// error ("Gap 4" live-borrow guard). That guard has been REMOVED.
//
// Rationale: a field reassignment emits no eager drop. A node is freed
// only at the SCOPE BOUNDARY, via its own drop-chain entry — never by
// the act of reassigning a reference to it. So overwriting a borrowed
// slot frees nothing, the alias does not dangle, and the guard was a
// false positive that also blocked valid ownership transfers (tree
// rotations: `y = x.right; x.right = y.left;`) and intentional
// replacement of an owned field. Drop-exactly-once is the scope-exit
// drop chain's job, not a per-write check. Genuine use-after-`#`-move
// READS are still rejected (see PathBorrowTests / UseAfterMoveTests).
//
// These tests now pin the POSITIVE contract: such reassignments
// compile.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {

void expectCompiles(const std::string& source) {
    try {
        auto jit = CajetaJit::compile(source, "test.A");
        SUCCEED();
    } catch (cajeta::Exception& e) {
        FAIL() << "expected successful compile, got "
               << e.getErrorId() << ": " << e.getMessage();
    }
}

} // namespace

// Reassigning a field a local aliases is allowed: the write frees
// nothing (freeing is deferred to the scope boundary), so `alias`
// stays valid until scope exit.
TEST(AliasMutationBorrowTests, writeThroughAliasIsAllowed) {
    auto src =
        "package test;\n"
        "public class Person { String name; "
        "  public Person(String n) { this.name = n; } }\n"
        "public final class A {\n"
        "    public static int32 run() {\n"
        "        Person p = heap Person(\"Bob\");\n"
        "        String alias = p.name;\n"
        "        p.name = #\"Charlie\";\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    expectCompiles(src);
}

// Reassigning a prefix of the borrowed path (the whole `p`) is
// likewise allowed.
TEST(AliasMutationBorrowTests, writeToPrefixIsAllowed) {
    auto src =
        "package test;\n"
        "public class Person { String name; "
        "  public Person(String n) { this.name = n; } }\n"
        "public final class A {\n"
        "    public static int32 run() {\n"
        "        Person p = heap Person(\"Bob\");\n"
        "        String alias = p.name;\n"
        "        p = heap Person(\"Charlie\");\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    expectCompiles(src);
}

// Reassigning a nested-field prefix (`p.addr` while `p.addr.city` is
// aliased) is allowed for the same reason.
TEST(AliasMutationBorrowTests, writeToBorrowedPathPrefixIsAllowed) {
    auto src =
        "package test;\n"
        "public class Address { String city; "
        "  public Address(String c) { this.city = c; } }\n"
        "public class Person { Address addr; "
        "  public Person(Address a) { this.addr = a; } }\n"
        "public final class A {\n"
        "    public static int32 run() {\n"
        "        Person p = heap Person(heap Address(\"NYC\"));\n"
        "        String alias = p.addr.city;\n"
        "        p.addr = #heap Address(\"LA\");\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    expectCompiles(src);
}

// Disjoint write while a borrow is live: always fine, and the program
// runs to return the written value.
TEST(AliasMutationBorrowTests, writeToDisjointPathLeavesBorrowIntact) {
    auto src =
        "package test;\n"
        "public class Person { String name; int32 age; "
        "  public Person(String n) { this.name = n; this.age = 0; } }\n"
        "public final class A {\n"
        "    public static int32 run() {\n"
        "        Person p = heap Person(\"Bob\");\n"
        "        String alias = p.name;\n"
        "        p.age = 42;\n"
        "        return p.age;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.A");
    auto run = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(run(), 42);
}
