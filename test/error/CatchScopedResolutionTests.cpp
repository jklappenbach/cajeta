//
// The catch-tier scoped-resolution bug class: a catch clause's type was
// resolved at PARSE time by a bare global-registry lookup
// (`CajetaType::of(name)` in Statement.cpp's TRY builder) — no module scope,
// no imports, no archive placeholders, no retry at resolveTypes. A class
// declared in a sibling file (or a dependency archive) is not registered at
// that point, so the clause's type came back null and codegen silently
// degraded it TWICE: the catch variable bound as `int64` (ptrtoint of the
// thrown pointer), and the clause matched as a CATCH-ALL — type selection
// quietly vanished, so the WRONG clause could catch. (Dodged in the field by
// catching `RecoverableException`, which the embedded stdlib registers before
// user parse.)
//
// The fix routes catch types through the same tiered, import-aware,
// archive-vouched resolution every declared type uses (CajetaType's named
// core), at TryStatement::resolveTypes when the module is in hand; a name
// that still fails to resolve is a named, located error rather than a silent
// catch-all (the silent-resolution discipline).
//
// The repro rides the multi-source harness: the entry file compiles FIRST
// (the harness rotates it to primary), so its catch clauses parse before the
// sibling file's exception classes exist anywhere but the prescan archive.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <map>
#include <string>

using cajeta_test::CajetaJit;

namespace {

const char* kExceptions =
    "package test.exc;\n"
    "import cajeta.error.RecoverableException;\n"
    "import cajeta.lang.String;\n"
    "public class NotFound extends RecoverableException {\n"
    "    public NotFound(String message) { this.message = message; this.cause = 0; }\n"
    "}\n";

const char* kExceptions2 =
    "package test.exc;\n"
    "import cajeta.error.RecoverableException;\n"
    "import cajeta.lang.String;\n"
    "public class Timeout extends RecoverableException {\n"
    "    public Timeout(String message) { this.message = message; this.cause = 0; }\n"
    "}\n";

int32_t runMulti(const std::string& entryBody) {
    std::map<std::string, std::string> sources;
    sources["test.exc.NotFound"] = kExceptions;
    sources["test.exc.Timeout"] = kExceptions2;
    sources["test.T"] =
        "package test;\n"
        "import test.exc.NotFound;\n"
        "import test.exc.Timeout;\n"
        "public final class T {\n"
        "    public static int32 run() {\n"
        + entryBody +
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(sources, "test.T");
    return jit->lookup<int32_t (*)()>("run")();
}

} // namespace

// The selection half: with two typed clauses, the thrown type must reach ITS
// clause — a Timeout must not be swallowed by the NotFound clause. Under the
// bug both cross-file clauses were catch-alls, so the FIRST one caught (1).
TEST(CatchScopedResolution, crossFileTypeSelectsTheRightClause) {
    EXPECT_EQ(runMulti(
        "        try {\n"
        "            throw heap Timeout(\"t\");\n"
        "        } catch (NotFound e) {\n"
        "            return 1;\n"
        "        } catch (Timeout e) {\n"
        "            return 2;\n"
        "        }\n"
        "        return 0;\n"), 2);
}

// The binding half: the catch variable is bound at the CLASS type, not int64
// — reading a Throwable field through it works. Under the bug `e` was an
// int64 slot holding a pointer's bits.
TEST(CatchScopedResolution, crossFileCatchVarBindsTyped) {
    EXPECT_EQ(runMulti(
        "        try {\n"
        "            throw heap NotFound(\"abcd\");\n"
        "        } catch (NotFound e) {\n"
        "            return (int32) e.message.count();\n"
        "        }\n"
        "        return -1;\n"), 4);
}

// The ordering half of selection: the MATCHING clause first still wins, and
// a non-matching later clause stays untouched — pinning that the fix did not
// change first-match-wins for genuinely matching clauses.
TEST(CatchScopedResolution, firstMatchingClauseStillWins) {
    EXPECT_EQ(runMulti(
        "        try {\n"
        "            throw heap Timeout(\"t\");\n"
        "        } catch (Timeout e) {\n"
        "            return 3;\n"
        "        } catch (NotFound e) {\n"
        "            return 4;\n"
        "        }\n"
        "        return 0;\n"), 3);
}

// The field dodge keeps working: a base-class catch (embedded stdlib,
// registered before user parse) catches a derived cross-file throw.
TEST(CatchScopedResolution, baseClassCatchStillCatchesDerived) {
    std::map<std::string, std::string> sources;
    sources["test.exc.NotFound"] = kExceptions;
    sources["test.exc.Timeout"] = kExceptions2;
    sources["test.T"] =
        "package test;\n"
        "import cajeta.error.RecoverableException;\n"
        "import test.exc.NotFound;\n"
        "public final class T {\n"
        "    public static int32 run() {\n"
        "        try {\n"
        "            throw heap NotFound(\"x\");\n"
        "        } catch (RecoverableException e) {\n"
        "            return 5;\n"
        "        }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(sources, "test.T");
    EXPECT_EQ(jit->lookup<int32_t (*)()>("run")(), 5);
}
