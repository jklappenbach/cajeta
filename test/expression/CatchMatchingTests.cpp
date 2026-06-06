//
// Tests for multi-clause try/catch type dispatch (RTTI catch-matching).
//
// Before the fix, a try with multiple catch clauses ran catchClauses[0]
// unconditionally — so a sibling/unrelated catch listed first wrongly caught a
// leaf it shouldn't, and a non-matching clause swallowed the exception instead
// of letting it propagate. Now each clause is tested in source order via a
// runtime vtable parent-chain walk (__cajeta_exc_matches); the first is-a match
// binds + runs, and if none match the exception is re-thrown to the enclosing
// frame.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// A small hierarchy: AppError <- {NotFound, Denied} under RecoverableException.
const char* HIER =
    "package test;\n"
    "public class AppError extends RecoverableException {\n"
    "    public AppError() { this.message = \"app\"; }\n"
    "}\n"
    "public class NotFound extends AppError {\n"
    "    public NotFound() { this.message = \"nf\"; }\n"
    "}\n"
    "public class Denied extends AppError {\n"
    "    public Denied() { this.message = \"dn\"; }\n"
    "}\n";

int32_t runBody(const std::string& body) {
    std::string src = std::string(HIER) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        " + body + "\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// A sibling catch listed FIRST must not catch a different leaf; the matching
// clause (listed second) runs.
TEST(CatchMatchingTests, siblingCatchDoesNotMatch) {
    EXPECT_EQ(runBody(
        "try {\n"
        "    throw new NotFound();\n"
        "} catch (Denied d) {\n"
        "    return 2;\n"
        "} catch (NotFound n) {\n"
        "    return 1;\n"
        "} catch (AppError e) {\n"
        "    return 3;\n"
        "}\n"
        "return 0;"), 1);
}

// A supertype clause catches a subtype throw (is-a walk up the chain).
TEST(CatchMatchingTests, supertypeCatchMatches) {
    EXPECT_EQ(runBody(
        "try {\n"
        "    throw new NotFound();\n"
        "} catch (AppError e) {\n"
        "    return 3;\n"
        "}\n"
        "return 0;"), 3);
}

// First matching clause wins (exact leaf before supertype).
TEST(CatchMatchingTests, firstMatchingClauseWins) {
    EXPECT_EQ(runBody(
        "try {\n"
        "    throw new NotFound();\n"
        "} catch (NotFound n) {\n"
        "    return 1;\n"
        "} catch (AppError e) {\n"
        "    return 3;\n"
        "}\n"
        "return 0;"), 1);
}

// The other leaf routes to its own clause, not the sibling's.
TEST(CatchMatchingTests, otherLeafRoutesToOwnClause) {
    EXPECT_EQ(runBody(
        "try {\n"
        "    throw new Denied();\n"
        "} catch (NotFound n) {\n"
        "    return 1;\n"
        "} catch (Denied d) {\n"
        "    return 2;\n"
        "}\n"
        "return 0;"), 2);
}

// No clause matches the inner throw → it propagates to the outer handler.
TEST(CatchMatchingTests, noMatchPropagatesToOuter) {
    EXPECT_EQ(runBody(
        "try {\n"
        "    try {\n"
        "        throw new NotFound();\n"
        "    } catch (Denied d) {\n"
        "        return 2;\n"
        "    }\n"
        "} catch (AppError e) {\n"
        "    return 8;\n"
        "}\n"
        "return 0;"), 8);
}

// A supertype-typed catch binding still works (the value is the thrown leaf).
TEST(CatchMatchingTests, exactLeafBeatsSiblingThenSupertype) {
    EXPECT_EQ(runBody(
        "try {\n"
        "    throw new Denied();\n"
        "} catch (NotFound n) {\n"
        "    return 1;\n"
        "} catch (AppError e) {\n"
        "    return 3;\n"
        "}\n"
        "return 0;"), 3);
}
