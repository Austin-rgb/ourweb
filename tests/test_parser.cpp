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

// ---------------------------------------------------------------------------
// Hardening tests
// ---------------------------------------------------------------------------

namespace {

ParseResult parse(std::string_view raw, ParserLimits limits = {}) {
    return parse_http_request(raw, limits);
}

void expect_error(std::string_view raw, HttpStatus status, ParserLimits limits = {}) {
    auto r = parse(raw, limits);
    EXPECT_TRUE(r.error);
    EXPECT_EQ(static_cast<int>(r.error_status), static_cast<int>(status));
    EXPECT_FALSE(r.complete);
}

} // namespace

TEST(ParserIncremental, ByteByByteMatchesOneShot) {
    const std::string body = R"({"a":1})";
    const std::string raw = "POST /x?y=1 HTTP/1.1\r\nHost: h\r\nContent-Length: " +
                            std::to_string(body.size()) + "\r\n\r\n" + body;
    RequestParser p;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        ASSERT_FALSE(p.state() == RequestParser::State::Complete);
        p.feed(std::string_view(raw).substr(i, 1));
    }
    ASSERT_TRUE(p.state() == RequestParser::State::Complete);
    auto r = p.take_result();
    EXPECT_EQ(r.request.path(), "/x");
    EXPECT_EQ(r.request.body(), body);
    EXPECT_EQ(*r.request.query_param("y"), "1");
}

TEST(ParserIncremental, IncompleteReportsNotComplete) {
    auto r = parse("GET / HTTP/1.1\r\nHost: x\r\n");
    EXPECT_FALSE(r.complete);
    EXPECT_FALSE(r.error);
    auto r2 = parse("POST / HTTP/1.1\r\nContent-Length: 10\r\n\r\nabc");
    EXPECT_FALSE(r2.complete);
    EXPECT_FALSE(r2.error);
}

TEST(ParserIncremental, BytesAfterBodyAreIgnored) {
    auto r = parse("POST / HTTP/1.1\r\nContent-Length: 3\r\n\r\nabcGET /evil HTTP/1.1\r\n\r\n");
    ASSERT_TRUE(r.complete);
    EXPECT_EQ(r.request.body(), "abc");
}

TEST(ParserLimitsTest, HeaderBlockTooLargeRejectedBeforeTerminator) {
    ParserLimits lim;
    lim.max_header_bytes = 256;
    // No CRLFCRLF ever arrives: must still fail instead of buffering forever.
    RequestParser p(lim);
    p.feed("GET / HTTP/1.1\r\n");
    for (int i = 0; i < 10 && p.state() == RequestParser::State::Head; ++i) {
        p.feed("X-Pad: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\r\n");
    }
    ASSERT_TRUE(p.state() == RequestParser::State::Failed);
    EXPECT_EQ(static_cast<int>(p.take_result().error_status), 431);
}

TEST(ParserLimitsTest, TooManyHeaders) {
    ParserLimits lim;
    lim.max_header_count = 3;
    expect_error("GET / HTTP/1.1\r\nA: 1\r\nB: 2\r\nC: 3\r\nD: 4\r\n\r\n",
                 HttpStatus::RequestHeaderFieldsTooLarge, lim);
    EXPECT_TRUE(parse("GET / HTTP/1.1\r\nA: 1\r\nB: 2\r\nC: 3\r\n\r\n", lim).complete);
}

TEST(ParserLimitsTest, TargetTooLong) {
    ParserLimits lim;
    lim.max_target_bytes = 16;
    expect_error("GET /" + std::string(40, 'a') + " HTTP/1.1\r\n\r\n", HttpStatus::UriTooLong, lim);
}

TEST(ParserLimitsTest, BodyAtLimitOkAndOverLimitRejectedFromHeadersAlone) {
    EXPECT_TRUE(parse("POST / HTTP/1.1\r\nContent-Length: 5\r\n\r\nabcde", ParserLimits{5}).complete);
    // 413 must be decided from the headers, without waiting for the body.
    expect_error("POST / HTTP/1.1\r\nContent-Length: 6\r\n\r\n", HttpStatus::RequestEntityTooLarge,
                 ParserLimits{5});
}

TEST(ParserFraming, ContentLengthMustBeStrictDigits) {
    for (const char* bad : {"12abc", "-1", "+5", "", " ", "1 2", "0x10", "5,5"}) {
        expect_error(std::string("POST / HTTP/1.1\r\nContent-Length: ") + bad + "\r\n\r\n",
                     HttpStatus::BadRequest);
    }
}

TEST(ParserFraming, HugeContentLengthIsTooLargeNotOverflow) {
    expect_error("POST / HTTP/1.1\r\nContent-Length: 99999999999999999999999\r\n\r\n",
                 HttpStatus::RequestEntityTooLarge);
}

TEST(ParserFraming, DuplicateContentLength) {
    expect_error("POST / HTTP/1.1\r\nContent-Length: 3\r\nContent-Length: 4\r\n\r\nabc",
                 HttpStatus::BadRequest);
    auto same = parse("POST / HTTP/1.1\r\nContent-Length: 3\r\nContent-Length: 3\r\n\r\nabc");
    ASSERT_TRUE(same.complete);
    EXPECT_EQ(same.request.body(), "abc");
}

TEST(ParserFraming, TransferEncodingRejected) {
    // TE + CL is the classic request-smuggling shape.
    expect_error("POST / HTTP/1.1\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\nabc",
                 HttpStatus::BadRequest);
    expect_error("POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n",
                 HttpStatus::NotImplemented);
}

TEST(ParserMalformed, RequestLine) {
    expect_error("GET /\r\n\r\n", HttpStatus::BadRequest);
    expect_error("GET /a b HTTP/1.1\r\n\r\n", HttpStatus::BadRequest);
    expect_error("GET  / HTTP/1.1\r\n\r\n", HttpStatus::BadRequest);   // double space
    expect_error("\r\n\r\n", HttpStatus::BadRequest);
    expect_error("G(T / HTTP/1.1\r\n\r\n", HttpStatus::BadRequest);   // bad method token
    expect_error("GET noslash HTTP/1.1\r\n\r\n", HttpStatus::BadRequest);
    expect_error("GET /a#frag HTTP/1.1\r\n\r\n", HttpStatus::BadRequest);
    expect_error("GET * HTTP/1.1\r\n\r\n", HttpStatus::BadRequest);  // '*' is OPTIONS-only
}

TEST(ParserMalformed, MethodAndVersion) {
    expect_error("BREW /pot HTTP/1.1\r\n\r\n", HttpStatus::NotImplemented);
    expect_error("GET / HTTP/2.0\r\n\r\n", HttpStatus::HttpVersionNotSupported);
    expect_error("GET / HTTP/0.9\r\n\r\n", HttpStatus::HttpVersionNotSupported);
    expect_error("GET / HTTX/1.1\r\n\r\n", HttpStatus::BadRequest);
    expect_error("GET / http/1.1\r\n\r\n", HttpStatus::BadRequest);
    EXPECT_TRUE(parse("GET / HTTP/1.0\r\n\r\n").complete);
    EXPECT_TRUE(parse("OPTIONS * HTTP/1.1\r\n\r\n").complete);
}

TEST(ParserMalformed, LineEndingsAndControlBytes) {
    expect_error("GET / HTTP/1.1\nHost: x\n\n", HttpStatus::BadRequest);          // bare LF
    expect_error("GET / HTTP/1.1\r\nHost: x\rX: y\r\n\r\n", HttpStatus::BadRequest);  // bare CR
    std::string with_nul = "GET / HTTP/1.1\r\nHost: a";
    with_nul.push_back('\0');
    with_nul += "b\r\n\r\n";
    expect_error(with_nul, HttpStatus::BadRequest);
}

TEST(ParserMalformed, Headers) {
    expect_error("GET / HTTP/1.1\r\nHost : x\r\n\r\n", HttpStatus::BadRequest);       // space before ':'
    expect_error("GET / HTTP/1.1\r\nA: 1\r\n  folded\r\n\r\n", HttpStatus::BadRequest);  // obs-fold
    expect_error("GET / HTTP/1.1\r\nNoColonHere\r\n\r\n", HttpStatus::BadRequest);
    expect_error("GET / HTTP/1.1\r\n: empty-name\r\n\r\n", HttpStatus::BadRequest);
    expect_error("GET / HTTP/1.1\r\nX: a\x01" "b\r\n\r\n", HttpStatus::BadRequest);
    expect_error("GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", HttpStatus::BadRequest);
}

TEST(ParserHeaders, ValuesTrimmedDuplicatesJoinedLookupCaseInsensitive) {
    auto r = parse("GET / HTTP/1.1\r\nX-Pad:   spaced \t \r\nAccept: a\r\nACCEPT: b\r\n"
                   "Cookie: x=1\r\nCookie: y=2\r\n\r\n");
    ASSERT_TRUE(r.complete);
    EXPECT_EQ(*r.request.header("x-pad"), "spaced");
    EXPECT_EQ(*r.request.header("AcCePt"), "a, b");
    EXPECT_EQ(*r.request.header("cookie"), "x=1; y=2");
}

TEST(ParserTarget, MalformedEncodingsRejected) {
    expect_error("GET /a%zz HTTP/1.1\r\n\r\n", HttpStatus::BadRequest);
    expect_error("GET /a%2 HTTP/1.1\r\n\r\n", HttpStatus::BadRequest);
    expect_error("GET /a%00b HTTP/1.1\r\n\r\n", HttpStatus::BadRequest);       // NUL
    expect_error("GET /s?q=%zz HTTP/1.1\r\n\r\n", HttpStatus::BadRequest);     // not silently dropped
    expect_error("GET /s?a=1&q=%4 HTTP/1.1\r\n\r\n", HttpStatus::BadRequest);
}

TEST(ParserTarget, PathKeptRawQueryDecoded) {
    auto r = parse("GET /a%20b/c+d?x=a+b&y=%41&flag&&z=1&z=2 HTTP/1.1\r\n\r\n");
    ASSERT_TRUE(r.complete);
    EXPECT_EQ(r.request.path(), "/a%20b/c+d");
    EXPECT_EQ(*r.request.query_param("x"), "a b");   // '+' is a space in queries
    EXPECT_EQ(*r.request.query_param("y"), "A");
    EXPECT_EQ(*r.request.query_param("flag"), "");
    EXPECT_EQ(*r.request.query_param("z"), "2");      // last wins
    EXPECT_EQ(r.request.query().size(), 4u);
}

TEST(ParserTarget, AbsoluteFormReducedToPath) {
    auto r = parse("GET http://example.com/p/q?x=1 HTTP/1.1\r\n\r\n");
    ASSERT_TRUE(r.complete);
    EXPECT_EQ(r.request.path(), "/p/q");
    EXPECT_EQ(*r.request.query_param("x"), "1");
    auto root = parse("GET https://example.com HTTP/1.1\r\n\r\n");
    ASSERT_TRUE(root.complete);
    EXPECT_EQ(root.request.path(), "/");
}

TEST(PercentDecode, PathKeepsPlusAndRejectsNul) {
    EXPECT_EQ(*percent_decode_path("a+b%20c"), "a+b c");
    EXPECT_EQ(*percent_decode("a+b%20c"), "a b c");
    EXPECT_FALSE(percent_decode_path("a%00b"));
    EXPECT_FALSE(percent_decode("%"));
}

TEST(ParserExpect, ContinueReportedOnceWhileBodyPending) {
    RequestParser p;
    p.feed("POST / HTTP/1.1\r\nExpect: 100-continue\r\nContent-Length: 4\r\n\r\n");
    ASSERT_TRUE(p.state() == RequestParser::State::Body);
    EXPECT_TRUE(p.take_continue());
    EXPECT_FALSE(p.take_continue());
    p.feed("data");
    EXPECT_TRUE(p.state() == RequestParser::State::Complete);
    expect_error("POST / HTTP/1.1\r\nExpect: something-else\r\nContent-Length: 1\r\n\r\n",
                 HttpStatus::ExpectationFailed);
}
