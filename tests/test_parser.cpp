#include <gtest/gtest.h>

// http_parser.hpp is internal; for unit tests we include it directly
#include "../src/http_parser.hpp"

using namespace ourweb;

TEST(Parser, SimpleGet) {
    std::string raw =
        "GET /hello HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "\r\n";

    auto r = parse_http_request(raw, 1024);
    ASSERT_TRUE(r.complete);
    ASSERT_FALSE(r.error);
    EXPECT_EQ(r.request.method(), HttpMethod::Get);
    EXPECT_EQ(r.request.path(), "/hello");
    EXPECT_TRUE(r.request.body().empty());
}

TEST(Parser, WithQuery) {
    std::string raw =
        "GET /search?q=hello%20world&page=1 HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "\r\n";

    auto r = parse_http_request(raw, 1024);
    ASSERT_TRUE(r.complete);
    auto q = r.request.query_param("q");
    ASSERT_TRUE(q);
    EXPECT_EQ(*q, "hello world");
    auto page = r.request.query_param("page");
    ASSERT_TRUE(page);
    EXPECT_EQ(*page, "1");
}

TEST(Parser, PostWithBody) {
    std::string body = R"({"name":"Alice"})";
    std::string raw =
        "POST /users HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "\r\n" + body;

    auto r = parse_http_request(raw, 1024);
    ASSERT_TRUE(r.complete);
    ASSERT_FALSE(r.error);
    EXPECT_EQ(r.request.method(), HttpMethod::Post);
    EXPECT_EQ(r.request.body(), body);
}

TEST(Parser, BodyTooLarge) {
    std::string body(100, 'x');
    std::string raw =
        "POST / HTTP/1.1\r\n"
        "Content-Length: 100\r\n"
        "\r\n" + body;

    auto r = parse_http_request(raw, 50);
    EXPECT_TRUE(r.error);
    EXPECT_EQ(r.error_message, "Request body too large");
}

TEST(PercentDecode, Basic) {
    auto d = percent_decode("hello%20world");
    ASSERT_TRUE(d);
    EXPECT_EQ(*d, "hello world");
}

TEST(PercentDecode, Malformed) {
    EXPECT_FALSE(percent_decode("bad%2"));
    EXPECT_FALSE(percent_decode("bad%zz"));
}
