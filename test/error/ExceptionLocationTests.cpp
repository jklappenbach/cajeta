// located-semantic-diagnostics (Phase 1): cajeta::Exception carries an optional
// source span (file/line/column) so semantic errors can be placed precisely.

#include <gtest/gtest.h>

#include "cajeta/error/Exception.h"

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
