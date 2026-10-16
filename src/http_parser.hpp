#pragma once

#include "ourweb/request.hpp"
#include "ourweb/http.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ourweb {

// Very simple HTTP/1.1 request parser for the initial implementation.
// Not a full RFC-compliant parser; sufficient for the scaffold and tests.
struct ParseResult {
    bool complete = false;
    bool error = false;
    std::string error_message;
    Request request;
    std::size_t consumed = 0;
};

// Returns a ParseResult. If the buffer does not yet contain a full request,
// complete == false and consumed indicates how much was examined.
ParseResult parse_http_request(std::string_view data, std::size_t max_body_size);

// Percent-decode a string (query / path segments). Returns nullopt on malformed.
std::optional<std::string> percent_decode(std::string_view in);

// Parse query string into key-value map (last value wins for duplicates).
std::unordered_map<std::string, std::string> parse_query_string(std::string_view qs);

} // namespace ourweb
