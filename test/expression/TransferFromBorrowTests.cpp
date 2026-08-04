// transfer-demotes-to-borrow Unit 3 (plan 3.1) — transferring FROM a borrow.
//
// After a transfer the source is a borrow (Unit 2), and you cannot transfer
// ownership out of a borrow. Transferring twice IS transferring from a
// borrow, so both cases are one error: CAJETA_ERROR_MOVE_OF_BORROW.
//
// Unit 2 deleted the read rejections, and the old double-transfer diagnostic
// came from one of them. These tests establish whether double transfer is
// currently rejected at all — a silent acceptance here would be a hole Unit 2
// opened, and closing it is what Unit 3 is for.
//
// 3.1.6 is the load-bearing one: `#=` from a plain FORMAL must keep
// compiling. A formal's ownership is fixed by the CALL SITE, so `#=` there is
// conditional acquisition, not a statically-known borrow. The whole stdlib
// depends on it (spec 1.4).

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

const char* kDecls =
    "package test;\n"
    "import cajeta.collection.HashMap;\n"
    "public class Tag {\n"
    "    public int32 n;\n"
    "    public Tag(int32 v) { this.n = v; }\n"
    "}\n"
    "public class H {\n"
    "    public Tag c;\n"
    "    public H() { this.c = null; }\n"
    "    public void keep(Tag v) { this.c #= v; }\n"
    "    public void own(#Tag v) { this.c #= v; }\n"
    "}\n";

// Returns the error id, or "" when the source compiles.
std::string errorOf(const std::string& body) {
    std::string src = std::string(kDecls) +
        "public final class D {\n"
        "    public static int32 run() {\n" + body +
        "        return 1;\n"
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        return "";
    } catch (cajeta::Exception& e) {
        return e.getErrorId();
    } catch (const std::exception&) {
        return "<non-cajeta-exception>";
    }
}

std::string messageOf(const std::string& body) {
    std::string src = std::string(kDecls) +
        "public final class D {\n"
        "    public static int32 run() {\n" + body +
        "        return 1;\n"
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        return "";
    } catch (cajeta::Exception& e) {
        return e.getMessage();
    } catch (const std::exception& e) {
        return e.what();
    }
}

}  // namespace

// 3.1.1 — the headline case. Transferring twice is transferring from a borrow.
TEST(TransferFromBorrowTests, secondTransferOfALocalIsMoveOfBorrow) {
    EXPECT_EQ(errorOf(
        "        Tag t = heap Tag(1);\n"
        "        Tag a #= t;\n"
        "        Tag b #= t;\n"), "CAJETA_ERROR_MOVE_OF_BORROW");
}

// 3.1.2 — same through a container mutator (spec 3.2.3).
TEST(TransferFromBorrowTests, secondSurrenderOfAKeyIsMoveOfBorrow) {
    EXPECT_EQ(errorOf(
        "        HashMap<Tag, int32> m = heap HashMap<Tag, int32>();\n"
        "        Tag t = heap Tag(1);\n"
        "        m.put(#t, 10);\n"
        "        m.put(#t, 99);\n"), "CAJETA_ERROR_MOVE_OF_BORROW");
}

// 3.1.3 — `#t` as a call argument after the local was transferred.
TEST(TransferFromBorrowTests, sharpArgumentAfterTransferIsMoveOfBorrow) {
    EXPECT_EQ(errorOf(
        "        Tag t = heap Tag(1);\n"
        "        Tag a #= t;\n"
        "        H h = heap H();\n"
        "        h.own(#t);\n"), "CAJETA_ERROR_MOVE_OF_BORROW");
}

// 3.1.4 — the PRE-EXISTING case must not regress: transferring from an alias
// still raises, and still names the actual owner.
TEST(TransferFromBorrowTests, transferFromAnAliasStillNamesTheOwner) {
    EXPECT_EQ(errorOf(
        "        Tag t = heap Tag(1);\n"
        "        Tag alias = t;\n"
        "        Tag x #= alias;\n"), "CAJETA_ERROR_MOVE_OF_BORROW");
    EXPECT_NE(messageOf(
        "        Tag t = heap Tag(1);\n"
        "        Tag alias = t;\n"
        "        Tag x #= alias;\n").find("`t`"), std::string::npos)
        << "the diagnostic must still name the owner";
}

// 3.1.5 — a demoted binding as a `#=` source: statically a borrow.
TEST(TransferFromBorrowTests, sharpAssignFromDemotedBindingIsMoveOfBorrow) {
    EXPECT_EQ(errorOf(
        "        Tag t = heap Tag(1);\n"
        "        Tag a #= t;\n"
        "        H h = heap H();\n"
        "        h.c #= t;\n"), "CAJETA_ERROR_MOVE_OF_BORROW");
}

// 3.1.6 — THE REGRESSION PIN (spec 1.4, 2.2.8).
//
// `#=` from a plain FORMAL is conditional acquisition: ownership is fixed by
// the call site, so the store forwards whatever the caller did. It must keep
// compiling under all three shapes. If 3.2's tightening treats a formal as a
// statically-known borrow, every dual-role store in the stdlib breaks —
// LinkedListNode, CacheNode, and the field stores behind them.
TEST(TransferFromBorrowTests, sharpAssignFromAPlainFormalStillCompiles) {
    EXPECT_EQ(errorOf(                       // caller LENDS
        "        Tag t = heap Tag(1);\n"
        "        H h = heap H();\n"
        "        h.keep(t);\n"), "");
    EXPECT_EQ(errorOf(                       // caller TRANSFERS
        "        Tag t = heap Tag(1);\n"
        "        H h = heap H();\n"
        "        h.keep(#t);\n"), "");
    EXPECT_EQ(errorOf(                       // fresh construction at the call site
        "        H h = heap H();\n"
        "        h.keep(heap Tag(1));\n"), "");
}

// 3.1.7 — a plain argument at a `#T` formal is still TRANSFER_REQUIRED, not a
// borrow error. The diagnostic must name the real problem (spec 2.2.5).
TEST(TransferFromBorrowTests, plainArgAtSharpFormalIsStillTransferRequired) {
    EXPECT_EQ(errorOf(
        "        Tag t = heap Tag(1);\n"
        "        H h = heap H();\n"
        "        h.own(t);\n"), "CAJETA_ERROR_TRANSFER_REQUIRED");
}

// ---- Unit 4 (plan 4.1): diagnostic quality ---------------------------------

// 4.1.3 — when the new owner is a CONTAINER there is no source-level variable
// to name (spec 6.5). The message must degrade gracefully: still identify the
// subject, never emit an empty name or attribute ownership to nothing.
TEST(TransferFromBorrowTests, messageDegradesWhenTheNewOwnerIsAContainer) {
    std::string msg = messageOf(
        "        HashMap<Tag, int32> m = heap HashMap<Tag, int32>();\n"
        "        Tag t = heap Tag(1);\n"
        "        m.put(#t, 10);\n"
        "        m.put(#t, 99);\n");
    EXPECT_NE(msg.find("`t`"), std::string::npos) << msg;
    EXPECT_EQ(msg.find("``"), std::string::npos)
        << "empty name in the diagnostic: " << msg;
    EXPECT_EQ(msg.find("belongs to `."), std::string::npos) << msg;
}

// 4.1.4 — the diagnostic speaks about OWNERSHIP, never about the container
// operation that happened to trigger it. "You cannot insert the same key
// twice" would read as a restriction on duplicate keys, which is not the rule
// and is not true (spec 4.1.2).
TEST(TransferFromBorrowTests, messageDoesNotDescribeTheViolationInContainerTerms) {
    std::string msg = messageOf(
        "        HashMap<Tag, int32> m = heap HashMap<Tag, int32>();\n"
        "        Tag t = heap Tag(1);\n"
        "        m.put(#t, 10);\n"
        "        m.put(#t, 99);\n");
    for (const char* banned : {"same key", "duplicate key", "insert the same",
                               "already in the map", "already contains"}) {
        EXPECT_EQ(msg.find(banned), std::string::npos)
            << "container-flavoured wording `" << banned << "` in: " << msg;
    }
}

// 4.1.1 — every transfer diagnostic states the rule once, in the same words.
TEST(TransferFromBorrowTests, everyTransferDiagnosticStatesTheRule) {
    const char* kRule = "cannot transfer ownership more than once, or from a borrow";
    EXPECT_NE(messageOf(                       // demoted by an earlier transfer
        "        Tag t = heap Tag(1);\n"
        "        Tag a #= t;\n"
        "        Tag b #= t;\n").find(kRule), std::string::npos);
    EXPECT_NE(messageOf(                       // never owned — an alias
        "        Tag t = heap Tag(1);\n"
        "        Tag alias = t;\n"
        "        Tag x #= alias;\n").find(kRule), std::string::npos);
}

// 1.4.1 — `#=` is CONDITIONAL acquisition, not an unconditional transfer
// (spec 1.4): at a plain formal it forwards whatever the caller did. The
// double-sharp diagnostic must not claim otherwise.
TEST(TransferFromBorrowTests, doubleSharpDiagnosticDoesNotClaimUnconditionalTransfer) {
    std::string msg = messageOf(
        "        Tag t = heap Tag(1);\n"
        "        Tag b #= #t;\n");
    EXPECT_EQ(msg.find("already transfers"), std::string::npos)
        << "`#=` is conditional acquisition, not an unconditional transfer: " << msg;
    EXPECT_NE(msg.find("#"), std::string::npos) << msg;
}
