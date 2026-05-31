//
// Pure tests for DAP Content-Length framing (CP4), over stringstreams. Pins
// the wire format and the read/write round-trip the stdio server relies on.
//
#include <gtest/gtest.h>

#include <sstream>

#include "cajeta/dap/DapProtocol.h"
#include "cajeta/dap/Json.h"

using cajeta::dap::Json;
using cajeta::dap::readMessage;
using cajeta::dap::writeMessage;

TEST(DapProtocol, WritesContentLengthHeader) {
    Json msg = Json::object();
    msg["type"] = "response";
    msg["seq"] = 1;
    std::ostringstream out;
    ASSERT_TRUE(writeMessage(out, msg));
    std::string s = out.str();
    std::string body = msg.dump();
    std::string expected =
        "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    EXPECT_EQ(s, expected);
}

TEST(DapProtocol, ReadsFramedMessage) {
    std::string body = "{\"command\":\"initialize\",\"seq\":1}";
    std::string framed =
        "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    std::istringstream in(framed);
    Json msg;
    ASSERT_TRUE(readMessage(in, &msg));
    EXPECT_EQ(msg.at("command").asString(), "initialize");
    EXPECT_EQ(msg.at("seq").asInt(), 1);
}

TEST(DapProtocol, RoundTripsThroughStream) {
    Json original = Json::object();
    original["command"] = "setBreakpoints";
    original["seq"] = 7;
    Json bps = Json::array();
    Json bp = Json::object();
    bp["line"] = 42;
    bps.push_back(bp);
    original["lines"] = bps;

    std::stringstream stream;
    ASSERT_TRUE(writeMessage(stream, original));
    Json read;
    ASSERT_TRUE(readMessage(stream, &read));
    EXPECT_EQ(read.at("command").asString(), "setBreakpoints");
    EXPECT_EQ(read.at("seq").asInt(), 7);
    EXPECT_EQ(read.at("lines")[0].at("line").asInt(), 42);
}

TEST(DapProtocol, ReadsTwoBackToBackMessages) {
    std::stringstream stream;
    Json a = Json::object(); a["seq"] = 1;
    Json b = Json::object(); b["seq"] = 2;
    writeMessage(stream, a);
    writeMessage(stream, b);

    Json ra, rb;
    ASSERT_TRUE(readMessage(stream, &ra));
    ASSERT_TRUE(readMessage(stream, &rb));
    EXPECT_EQ(ra.at("seq").asInt(), 1);
    EXPECT_EQ(rb.at("seq").asInt(), 2);
}

TEST(DapProtocol, ToleratesExtraHeaders) {
    std::string body = "{\"seq\":9}";
    std::string framed =
        "Content-Type: application/vscode-jsonrpc; charset=utf-8\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    std::istringstream in(framed);
    Json msg;
    ASSERT_TRUE(readMessage(in, &msg));
    EXPECT_EQ(msg.at("seq").asInt(), 9);
}

TEST(DapProtocol, ReturnsFalseAtEof) {
    std::istringstream in("");
    Json msg;
    EXPECT_FALSE(readMessage(in, &msg));
}

TEST(DapProtocol, ReturnsFalseOnMissingContentLength) {
    std::istringstream in("X-Foo: bar\r\n\r\n{}");
    Json msg;
    EXPECT_FALSE(readMessage(in, &msg));
}
