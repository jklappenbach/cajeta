// Regression tests for the JSONC preprocessor and parser used by the
// cajeta build-tool manifest loader. See
// src/cajeta/buildtool/JsonC.h and plans/buildtool/build-tool-plan.md Phase 0.

#include "cajeta/buildtool/JsonC.h"

#include <gtest/gtest.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>

#include <string>

using cajeta::buildtool::parseJsonC;
using cajeta::buildtool::preprocessJsonC;

// --- preprocessor: comments + trailing commas -----------------------------







// --- preprocessor: must NOT mangle string content -------------------------

TEST(JsonCTests, doesNotStripCommentMarkersInsideStrings) {
    auto v = parseJsonC(R"({ "url": "http://example.com/path" })");
    ASSERT_TRUE((bool)v);
    auto* obj = v->getAsObject();
    ASSERT_NE(obj, nullptr);
    auto s = obj->getString("url");
    ASSERT_TRUE((bool)s);
    EXPECT_EQ(s->str(), "http://example.com/path");
}

TEST(JsonCTests, doesNotStripBlockCommentMarkersInsideStrings) {
    auto v = parseJsonC(R"({ "k": "value with /* embedded */ marker" })");
    ASSERT_TRUE((bool)v);
    auto* obj = v->getAsObject();
    ASSERT_NE(obj, nullptr);
    auto s = obj->getString("k");
    ASSERT_TRUE((bool)s);
    EXPECT_EQ(s->str(), "value with /* embedded */ marker");
}

TEST(JsonCTests, doesNotStripCommaInsideStringContent) {
    auto v = parseJsonC(R"({ "k": "a, b, c, " })");
    ASSERT_TRUE((bool)v);
    auto* obj = v->getAsObject();
    ASSERT_NE(obj, nullptr);
    auto s = obj->getString("k");
    ASSERT_TRUE((bool)s);
    EXPECT_EQ(s->str(), "a, b, c, ");
}

TEST(JsonCTests, handlesEscapedQuoteInStrings) {
    auto v = parseJsonC(R"({ "k": "has \"quote\" inside, ok" })");
    ASSERT_TRUE((bool)v);
    auto* obj = v->getAsObject();
    ASSERT_NE(obj, nullptr);
    auto s = obj->getString("k");
    ASSERT_TRUE((bool)s);
    EXPECT_EQ(s->str(), R"(has "quote" inside, ok)");
}

// --- preprocessor: position preservation ----------------------------------

TEST(JsonCTests, preservesSourceLengthAfterPreprocessing) {
    std::string in =
        "{ // comment\n"
        "  /* block */\n"
        "  \"k\": 1,\n"
        "}";
    auto cleaned = preprocessJsonC(in);
    EXPECT_EQ(cleaned.size(), in.size());
}

TEST(JsonCTests, preservesNewlinesInBlockComments) {
    std::string in = "/*\n\n\n*/{}";
    auto cleaned = preprocessJsonC(in);
    EXPECT_EQ(cleaned.size(), in.size());
    // Three newlines stay as three newlines so line numbers downstream
    // are unchanged.
    int newlines = 0;
    for (char c : cleaned) if (c == '\n') ++newlines;
    EXPECT_EQ(newlines, 3);
}

// --- compound: realistic manifest fragment --------------------------------


// --- malformed input ------------------------------------------------------


