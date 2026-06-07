//
// Pure tests for the DAP JSON value model + parser/serializer (CP4). No JIT —
// microsecond tests pinning the round-trips and edge cases the DAP wire relies
// on (object/array nesting, string escaping, int-vs-float formatting, parse
// errors).
//
#include <gtest/gtest.h>

#include "cajeta/dap/Json.h"

using cajeta::dap::Json;

TEST(Json, BuildsAndDumpsObject) {
    Json j = Json::object();
    j["seq"] = 1;
    j["type"] = "response";
    j["success"] = true;
    std::string s = j.dump();
    // Keys are emitted in sorted order (std::map).
    EXPECT_EQ(s, "{\"seq\":1,\"success\":true,\"type\":\"response\"}");
}

TEST(Json, IntegersHaveNoDecimalPoint) {
    Json j = 42;
    EXPECT_EQ(j.dump(), "42");
    Json neg = -7;
    EXPECT_EQ(neg.dump(), "-7");
}

TEST(Json, EscapesStrings) {
    Json j = std::string("a\"b\\c\nd\te");
    EXPECT_EQ(j.dump(), "\"a\\\"b\\\\c\\nd\\te\"");
}

TEST(Json, ArrayRoundTrip) {
    Json a = Json::array();
    a.push_back(1);
    a.push_back(2);
    a.push_back(3);
    EXPECT_EQ(a.dump(), "[1,2,3]");
    EXPECT_EQ(a.size(), 3u);
    EXPECT_EQ(a[1].asInt(), 2);
}

TEST(Json, ParsesObject) {
    bool ok = false;
    Json j = Json::parse(
        "{\"command\":\"setBreakpoints\",\"seq\":5,\"args\":{\"line\":42}}", &ok);
    ASSERT_TRUE(ok);
    EXPECT_TRUE(j.isObject());
    EXPECT_EQ(j.at("command").asString(), "setBreakpoints");
    EXPECT_EQ(j.at("seq").asInt(), 5);
    EXPECT_EQ(j.at("args").at("line").asInt(), 42);
}

TEST(Json, ParsesNestedArrayOfObjects) {
    bool ok = false;
    Json j = Json::parse(
        "{\"breakpoints\":[{\"line\":3},{\"line\":7}]}", &ok);
    ASSERT_TRUE(ok);
    const Json& bps = j.at("breakpoints");
    ASSERT_TRUE(bps.isArray());
    ASSERT_EQ(bps.size(), 2u);
    EXPECT_EQ(bps[0].at("line").asInt(), 3);
    EXPECT_EQ(bps[1].at("line").asInt(), 7);
}

TEST(Json, ParsesScalarsAndEscapes) {
    bool ok = false;
    Json j = Json::parse("{\"s\":\"a\\nb\",\"b\":true,\"n\":null,\"f\":1.5}", &ok);
    ASSERT_TRUE(ok);
    EXPECT_EQ(j.at("s").asString(), "a\nb");
    EXPECT_TRUE(j.at("b").asBool());
    EXPECT_TRUE(j.at("n").isNull());
    EXPECT_DOUBLE_EQ(j.at("f").asNumber(), 1.5);
}

TEST(Json, RoundTripsThroughParseAndDump) {
    std::string original =
        "{\"a\":[1,2],\"b\":{\"c\":\"x\"},\"d\":false}";
    bool ok = false;
    Json j = Json::parse(original, &ok);
    ASSERT_TRUE(ok);
    // Re-dump should be byte-identical (keys already sorted in `original`).
    EXPECT_EQ(j.dump(), original);
}

TEST(Json, RejectsMalformed) {
    bool ok = true;
    Json::parse("{\"a\":}", &ok);
    EXPECT_FALSE(ok);

    ok = true;
    Json::parse("[1,2", &ok);
    EXPECT_FALSE(ok);

    ok = true;
    Json::parse("{\"a\":1} trailing", &ok);
    EXPECT_FALSE(ok);

    ok = true;
    Json::parse("\"unterminated", &ok);
    EXPECT_FALSE(ok);
}

TEST(Json, MissingKeyReturnsNullSentinel) {
    Json j = Json::object();
    j["present"] = 1;
    EXPECT_FALSE(j.has("absent"));
    EXPECT_TRUE(j.at("absent").isNull());
    EXPECT_EQ(j.at("absent").asInt(99), 99);  // default flows through
}
