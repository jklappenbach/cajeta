//
// util/Environment.cpp — EnvironmentScope, the launch-environment splice
// behind DapServer's `env`/`inheritSystemEnv` launch args. Direct unit
// coverage of the three arms: overlay (inherit on), whole-environment
// suppression (inherit off), and the ordered restore that makes the FIRST
// snapshot of a twice-touched name win.
//
#include "gtest/gtest.h"

#include "cajeta/util/Environment.h"

#include <cstdlib>
#include <map>
#include <string>

using cajeta::util::EnvironmentScope;
using cajeta::util::setEnvVar;
using cajeta::util::unsetEnvVar;

namespace {

std::string envOr(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : fallback;
}

} // namespace

TEST(EnvironmentScopeTests, overlaySetsAndRestoreputsPriorValuesBack) {
    setEnvVar("CAJETA_ENVSCOPE_A", "original");
    unsetEnvVar("CAJETA_ENVSCOPE_B");

    {
        EnvironmentScope scope;
        scope.apply({{"CAJETA_ENVSCOPE_A", "overlaid"},
                     {"CAJETA_ENVSCOPE_B", "fresh"}},
                    /*inheritParent=*/true);
        EXPECT_EQ(envOr("CAJETA_ENVSCOPE_A", "<unset>"), "overlaid");
        EXPECT_EQ(envOr("CAJETA_ENVSCOPE_B", "<unset>"), "fresh");
        scope.restore();
    }

    // A returns to its prior value; B (previously unset) is unset again.
    EXPECT_EQ(envOr("CAJETA_ENVSCOPE_A", "<unset>"), "original");
    EXPECT_EQ(envOr("CAJETA_ENVSCOPE_B", "<unset>"), "<unset>");
    unsetEnvVar("CAJETA_ENVSCOPE_A");
}

TEST(EnvironmentScopeTests, suppressionUnsetsUndeclaredAndRestoreRecovers) {
    setEnvVar("CAJETA_ENVSCOPE_KEEP", "declared");
    setEnvVar("CAJETA_ENVSCOPE_DROP", "undeclared");

    {
        EnvironmentScope scope;
        scope.apply({{"CAJETA_ENVSCOPE_KEEP", "declared-new"}},
                    /*inheritParent=*/false);
        // The declared name carries the configured value; the undeclared one
        // is suppressed along with the rest of the parent environment.
        EXPECT_EQ(envOr("CAJETA_ENVSCOPE_KEEP", "<unset>"), "declared-new");
        EXPECT_EQ(envOr("CAJETA_ENVSCOPE_DROP", "<unset>"), "<unset>");
        scope.restore();
    }

    // Everything is back: the suppressed var, and ambient basics like PATH.
    EXPECT_EQ(envOr("CAJETA_ENVSCOPE_KEEP", "<unset>"), "declared");
    EXPECT_EQ(envOr("CAJETA_ENVSCOPE_DROP", "<unset>"), "undeclared");
    EXPECT_NE(envOr("PATH", "<unset>"), "<unset>");
    unsetEnvVar("CAJETA_ENVSCOPE_KEEP");
    unsetEnvVar("CAJETA_ENVSCOPE_DROP");
}

TEST(EnvironmentScopeTests, firstSnapshotWinsWhenANameIsTouchedTwice) {
    setEnvVar("CAJETA_ENVSCOPE_TWICE", "true-original");

    EnvironmentScope scope;
    scope.apply({{"CAJETA_ENVSCOPE_TWICE", "first-apply"}},
                /*inheritParent=*/true);
    scope.apply({{"CAJETA_ENVSCOPE_TWICE", "second-apply"}},
                /*inheritParent=*/true);
    EXPECT_EQ(envOr("CAJETA_ENVSCOPE_TWICE", "<unset>"), "second-apply");

    scope.restore();
    EXPECT_EQ(envOr("CAJETA_ENVSCOPE_TWICE", "<unset>"), "true-original");
    unsetEnvVar("CAJETA_ENVSCOPE_TWICE");
}

TEST(EnvironmentScopeTests, restoreOnEmptyScopeIsANoOp) {
    EnvironmentScope scope;
    scope.restore();   // nothing remembered — must not touch anything
    EXPECT_NE(envOr("PATH", "<unset>"), "<unset>");
}
