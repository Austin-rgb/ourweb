#include <gtest/gtest.h>
#include <ourweb/response.hpp>
#include <ourweb/json.hpp>

using namespace ourweb;

TEST(Response, TextConvenience) {
    auto r = text("hello");
    EXPECT_EQ(r.status_code(), HttpStatus::Ok);
    EXPECT_EQ(r.body(), "hello");
    auto ct = r.headers().find("Content-Type");
    ASSERT_NE(ct, r.headers().end());
    EXPECT_TRUE(ct->second.find("text/plain") != std::string::npos);
}

TEST(Response, JsonConvenience) {
    struct Point { int x; int y; };
    // Use nlohmann directly via json()
    nlohmann::json j = {{"x", 1}, {"y", 2}};
    auto r = json(j);
    EXPECT_EQ(r.status_code(), HttpStatus::Ok);
    EXPECT_TRUE(r.body().find("\"x\":1") != std::string::npos ||
                r.body().find("\"x\": 1") != std::string::npos);
}

TEST(Response, StatusAndHeader) {
    auto r = Response{}
        .status(HttpStatus::Created)
        .header("Location", "/users/1")
        .body("Created");
    EXPECT_EQ(r.status_code(), HttpStatus::Created);
    EXPECT_EQ(r.body(), "Created");
    auto loc = r.headers().find("Location");
    ASSERT_NE(loc, r.headers().end());
    EXPECT_EQ(loc->second, "/users/1");
}

TEST(Response, InvalidHeaderThrows) {
    Response r;
    EXPECT_THROW(r.header("Bad\nName", "value"), std::invalid_argument);
    EXPECT_THROW(r.header("Ok", "bad\r\nvalue"), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// Header casing and wire framing
// ---------------------------------------------------------------------------

#include "../src/response_writer.hpp"

TEST(Response, HeaderReplacementIsCaseInsensitive) {
    Response r;
    r.header("content-type", "a/b").header("Content-Type", "c/d");
    EXPECT_EQ(r.headers().size(), 1u);
    EXPECT_EQ(r.headers().begin()->second, "c/d");
    r.content_type("e/f");  // must not override an existing one
    EXPECT_EQ(r.headers().begin()->second, "c/d");
}

namespace {
bool contains(const std::string& s, const std::string& sub) { return s.find(sub) != std::string::npos; }
}

TEST(Serialize, ContentLengthAlwaysMatchesBodyAndAppHeadersCannotBreakFraming) {
    Response r = text("hello");
    r.header("Content-Length", "9999").header("transfer-encoding", "chunked").header("Connection", "keep-alive");
    auto wire = serialize_response(r, false);
    EXPECT_TRUE(contains(wire, "HTTP/1.1 200 OK\r\n"));
    EXPECT_TRUE(contains(wire, "Content-Length: 5\r\n"));
    EXPECT_FALSE(contains(wire, "9999"));
    EXPECT_FALSE(contains(wire, "chunked"));
    EXPECT_FALSE(contains(wire, "keep-alive"));
    EXPECT_TRUE(contains(wire, "Connection: close\r\n\r\nhello"));
}

TEST(Serialize, HeadHasLengthButNoBody) {
    auto wire = serialize_response(text("hello"), true);
    EXPECT_TRUE(contains(wire, "Content-Length: 5\r\n"));
    EXPECT_TRUE(wire.size() >= 4 && wire.substr(wire.size() - 4) == "\r\n\r\n");
}

TEST(Serialize, NoContentAndNotModifiedCarryNoBodyOrLength) {
    for (auto st : {HttpStatus::NoContent, HttpStatus::NotModified}) {
        Response r(st);
        r.body("must not be sent");
        auto wire = serialize_response(r, false);
        EXPECT_FALSE(contains(wire, "Content-Length"));
        EXPECT_FALSE(contains(wire, "must not"));
    }
}

TEST(Serialize, InvalidStatusBecomes500) {
    for (std::uint16_t code : {std::uint16_t{0}, std::uint16_t{99}, std::uint16_t{100}, std::uint16_t{600}, std::uint16_t{1000}}) {
        Response r;
        r.status(code);
        EXPECT_TRUE(contains(serialize_response(r, false), "HTTP/1.1 500 "));
    }
}
