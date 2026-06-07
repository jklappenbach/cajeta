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

TEST(JsonCTests, stripsLineComments) {
    std::string in =
        "{ \"k\": 1, // trailing comment\n"
        "  \"j\": 2 }";
    auto cleaned = preprocessJsonC(in);
    // Comment chars become spaces; the newline is preserved so line
    // numbers in downstream errors stay correct.
    EXPECT_EQ(cleaned.size(), in.size());
    EXPECT_EQ(cleaned.find("trailing comment"), std::string::npos);
    // And it parses.
    auto v = parseJsonC(in);
    ASSERT_TRUE((bool)v);
    auto* obj = v->getAsObject();
    ASSERT_NE(obj, nullptr);
    EXPECT_EQ(obj->getInteger("k"), 1);
    EXPECT_EQ(obj->getInteger("j"), 2);
}

TEST(JsonCTests, stripsBlockComments) {
    std::string in =
        "{\n"
        "  /* multi\n"
        "     line\n"
        "     block */\n"
        "  \"k\": 1\n"
        "}";
    auto v = parseJsonC(in);
    ASSERT_TRUE((bool)v);
    auto* obj = v->getAsObject();
    ASSERT_NE(obj, nullptr);
    EXPECT_EQ(obj->getInteger("k"), 1);
}

TEST(JsonCTests, stripsInlineBlockComments) {
    auto v = parseJsonC(R"({ "k" /* inline */ : 1, "j" : /* and here */ 2 })");
    ASSERT_TRUE((bool)v);
    auto* obj = v->getAsObject();
    ASSERT_NE(obj, nullptr);
    EXPECT_EQ(obj->getInteger("k"), 1);
    EXPECT_EQ(obj->getInteger("j"), 2);
}

TEST(JsonCTests, stripsTrailingCommaInObject) {
    auto v = parseJsonC(R"({ "k": 1, "j": 2, })");
    ASSERT_TRUE((bool)v);
    auto* obj = v->getAsObject();
    ASSERT_NE(obj, nullptr);
    EXPECT_EQ(obj->getInteger("k"), 1);
    EXPECT_EQ(obj->getInteger("j"), 2);
}

TEST(JsonCTests, stripsTrailingCommaInArray) {
    auto v = parseJsonC(R"({ "a": [1, 2, 3, ] })");
    ASSERT_TRUE((bool)v);
    auto* obj = v->getAsObject();
    ASSERT_NE(obj, nullptr);
    auto* arr = obj->getArray("a");
    ASSERT_NE(arr, nullptr);
    EXPECT_EQ(arr->size(), 3u);
}

TEST(JsonCTests, stripsTrailingCommaWithInterveningWhitespace) {
    // Trailing comma followed by whitespace + closer.
    auto v = parseJsonC("{ \"k\": 1, \n\n  }");
    ASSERT_TRUE((bool)v);
}

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

TEST(JsonCTests, parsesRealisticManifestFragment) {
    // Trailing commas + comments + escaped strings + nested structures.
    auto v = parseJsonC(R"(
        {
            // identity
            "details": {
                "name":    "com.example.foo",
                "version": "0.1.0",
            },
            // properties used below
            "properties": {
                "stack-version": "1.4.7",
            },
            "settings": {
                "dependencies": {
                    "cajeta.lang": "${stack-version}",
                },
            },
        }
    )");
    ASSERT_TRUE((bool)v);
    auto* obj = v->getAsObject();
    ASSERT_NE(obj, nullptr);
    EXPECT_NE(obj->getObject("details"), nullptr);
    EXPECT_NE(obj->getObject("properties"), nullptr);
    EXPECT_NE(obj->getObject("settings"), nullptr);
}

// --- malformed input ------------------------------------------------------

TEST(JsonCTests, rejectsMalformedJson) {
    auto v = parseJsonC("{ not even close to JSON");
    EXPECT_FALSE((bool)v);
    if (!v) consumeError(v.takeError());
}

TEST(JsonCTests, rejectsUnterminatedString) {
    auto v = parseJsonC(R"({ "k": "unterminated)");
    EXPECT_FALSE((bool)v);
    if (!v) consumeError(v.takeError());
}
