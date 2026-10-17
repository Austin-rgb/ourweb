#pragma once

// Internal: strict, incremental HTTP/1.1 request parser.
//
// Security posture (see tests/test_parser.cpp):
//  * all sizes are bounded *before* buffering (head, target, header count, body)
//  * bare LF / stray CR, NUL bytes, obs-fold, and "Name : value" are rejected
//  * ambiguous framing is rejected: conflicting/duplicate Content-Length,
//    Content-Length together with Transfer-Encoding, any Transfer-Encoding
//    (chunked request bodies are not supported -> 501)
//  * the HTTP version, method token, request-target and percent-encodings are
//    validated; malformed query strings are an error, not silently dropped

#include "ourweb/http.hpp"
#include "ourweb/request.hpp"
#include "url.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ourweb {

struct ParserLimits {
    std::size_t max_body_size = 1024 * 1024;
    std::size_t max_header_bytes = 16 * 1024;   // request line + headers + CRLFCRLF
    std::size_t max_target_bytes = 8 * 1024;
    std::size_t max_header_count = 100;
};

struct ParseResult {
    bool complete = false;
    bool error = false;
    std::string error_message;
    HttpStatus error_status = HttpStatus::BadRequest;
    std::string error_code = "bad_request";
    Request request;
    std::size_t consumed = 0;
};

class RequestParser {
public:
    enum class State { Head, Body, Complete, Failed };

    explicit RequestParser(ParserLimits limits = {});

    // Feed more bytes from the socket. Bytes after the end of the request are
    // ignored (the server handles exactly one request per connection).
    State feed(std::string_view chunk);

    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] bool received_any() const noexcept { return received_ > 0; }

    // True exactly once if the client sent "Expect: 100-continue" and is
    // waiting for the interim response before sending the body.
    [[nodiscard]] bool take_continue() noexcept;

    // Valid once state() is Complete or Failed.
    [[nodiscard]] ParseResult take_result();

private:
    void fail(HttpStatus status, std::string code, std::string message);
    bool scan_head();
    void parse_head();
    void finish();

    ParserLimits limits_;
    State state_ = State::Head;

    std::string head_;
    std::size_t scan_ = 0;
    std::size_t head_end_ = 0;
    std::size_t received_ = 0;

    HttpMethod method_ = HttpMethod::Unknown;
    std::string path_;
    std::unordered_map<std::string, std::string> headers_;
    QueryMap query_;
    std::size_t content_length_ = 0;
    std::string body_;

    bool want_continue_ = false;
    ParseResult result_;
};

// One-shot convenience wrapper (used by tests): parse a complete buffer.
// If the buffer does not yet contain a full request, complete == false.
ParseResult parse_http_request(std::string_view data, std::size_t max_body_size);
ParseResult parse_http_request(std::string_view data, const ParserLimits& limits);

} // namespace ourweb
