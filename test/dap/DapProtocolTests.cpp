//
// dap/DapProtocol.cpp — the Content-Length stdio framing under DapServer's
// serve loop. Only IDE clients ever drove it, so it was 0% covered. Pure
// stream round-trips: writeMessage emits the header+body frame, readMessage
// parses it back, and the lenient/refusal arms (bare-\n headers, lone \r,
// case-insensitive Content-Length, missing length, truncated body, invalid
// JSON) are each pinned.
//
#include "gtest/gtest.h"

#include "cajeta/dap/DapProtocol.h"
#include "cajeta/dap/Json.h"

#include <sstream>
#include <string>

using cajeta::dap::Json;
using cajeta::dap::readMessage;
using cajeta::dap::writeMessage;

namespace {

Json sample() {
    Json j = Json::object();
    j["seq"] = 7;
    j["type"] = std::string("request");
    j["command"] = std::string("initialize");
    return j;
}

} // namespace

TEST(DapProtocolTests, writeThenReadRoundTrips) {
    std::stringstream wire;
    ASSERT_TRUE(writeMessage(wire, sample()));

    // The frame is exactly Content-Length + CRLFCRLF + the dumped body.
    std::string frame = wire.str();
    std::string body = sample().dump();
    EXPECT_EQ(frame,
              "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n"
                  + body);

    Json got;
    ASSERT_TRUE(readMessage(wire, &got));
    EXPECT_EQ(got.at("seq").asInt(), 7);
    EXPECT_EQ(got.at("command").asString(), "initialize");
}

TEST(DapProtocolTests, readsConsecutiveFramesFromOneStream) {
    std::stringstream wire;
    Json a = sample();
    Json b = sample();
    b["seq"] = 8;
    ASSERT_TRUE(writeMessage(wire, a));
    ASSERT_TRUE(writeMessage(wire, b));

    Json g1, g2;
    ASSERT_TRUE(readMessage(wire, &g1));
    ASSERT_TRUE(readMessage(wire, &g2));
    EXPECT_EQ(g1.at("seq").asInt(), 7);
    EXPECT_EQ(g2.at("seq").asInt(), 8);
    EXPECT_FALSE(readMessage(wire, nullptr));  // stream drained → EOF refusal
}

TEST(DapProtocolTests, contentLengthIsCaseInsensitiveAndExtraHeadersSkip) {
    std::string body = "{\"seq\":1,\"type\":\"request\"}";
    std::stringstream wire;
    wire << "content-LENGTH:   " << body.size() << "\r\n"
         << "Content-Type: application/vscode-jsonrpc\r\n"
         << "not-a-header-line\r\n"
         << "\r\n"
         << body;
    Json got;
    ASSERT_TRUE(readMessage(wire, &got));
    EXPECT_EQ(got.at("seq").asInt(), 1);
}

TEST(DapProtocolTests, bareNewlineHeadersAccepted) {
    std::string body = "{\"seq\":2}";
    std::stringstream wire;
    wire << "Content-Length: " << body.size() << "\n\n" << body;
    Json got;
    ASSERT_TRUE(readMessage(wire, &got));
    EXPECT_EQ(got.at("seq").asInt(), 2);
}

TEST(DapProtocolTests, loneCarriageReturnStaysInHeaderLine) {
    // A \r not followed by \n is part of the line (lenient), so this header
    // never matches Content-Length and the read fails for want of one.
    std::stringstream wire;
    wire << "Content\r-Length: 2\r\n\r\n{}";
    EXPECT_FALSE(readMessage(wire, nullptr));
}

TEST(DapProtocolTests, missingContentLengthRefused) {
    std::stringstream wire;
    wire << "Content-Type: application/vscode-jsonrpc\r\n\r\n{}";
    EXPECT_FALSE(readMessage(wire, nullptr));
}

TEST(DapProtocolTests, truncatedBodyRefused) {
    std::stringstream wire;
    wire << "Content-Length: 100\r\n\r\n{\"seq\":1}";
    EXPECT_FALSE(readMessage(wire, nullptr));
}

TEST(DapProtocolTests, invalidJsonBodyRefused) {
    std::string body = "not json at all";
    std::stringstream wire;
    wire << "Content-Length: " << body.size() << "\r\n\r\n" << body;
    EXPECT_FALSE(readMessage(wire, nullptr));
}

TEST(DapProtocolTests, eofMidHeaderRefused) {
    std::stringstream wire;
    wire << "Content-Length: 9";   // no terminator, no blank line, no body
    EXPECT_FALSE(readMessage(wire, nullptr));
}
