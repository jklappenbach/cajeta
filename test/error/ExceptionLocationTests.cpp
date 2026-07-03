// located-semantic-diagnostics (Phase 1): cajeta::Exception carries an optional
// source span (file/line/column) so semantic errors can be placed precisely.

#include <gtest/gtest.h>

#include "cajeta/error/Exception.h"
#include "cajeta/error/Diagnostics.h"

using cajeta::Exception;

TEST(ExceptionLocation, CarriesSpanWhenBuiltWithOne) {
    Exception e("bad type", "CJ_X", "/src/A.cajeta", 12, 5);
    EXPECT_EQ(e.getErrorId(), "CJ_X");
    EXPECT_EQ(e.getMessage(), "bad type");
    EXPECT_EQ(e.getFile(), "/src/A.cajeta");
    EXPECT_EQ(e.getLine(), 12);
    EXPECT_EQ(e.getColumn(), 5);
    EXPECT_TRUE(e.hasLocation());
}

TEST(ExceptionLocation, ReportsNoLocationForTheTwoArgForm) {
    Exception e("oops", "CJ_Y");
    EXPECT_FALSE(e.hasLocation());
    EXPECT_LE(e.getLine(), 0);
    EXPECT_LE(e.getColumn(), 0);
    EXPECT_TRUE(e.getFile().empty());
}

// The node-based overload (used at AST-node throw sites, e.g. aggregate-init):
// it stamps the explicit 1-based line/column; file comes from the active module
// (none in a unit test → empty).
TEST(ExceptionLocation, NodeOverloadStampsExplicitLineColumn) {
    Exception e = cajeta::locatedException(12, 5, "msg", "CJ_Z");
    EXPECT_EQ(e.getLine(), 12);
    EXPECT_EQ(e.getColumn(), 5);
    EXPECT_TRUE(e.hasLocation());
    EXPECT_EQ(e.getErrorId(), "CJ_Z");
}
