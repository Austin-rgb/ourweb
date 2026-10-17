#pragma once

// Internal helpers for percent-decoding and query-string parsing.

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace ourweb {

using QueryMap = std::unordered_map<std::string, std::string>;

// Percent-decode a query component: "%XX" becomes a byte and '+' becomes a
// space. Returns nullopt on malformed escapes ("%", "%2", "%zz") and on any
// input that would yield an embedded NUL byte (raw or "%00").
std::optional<std::string> percent_decode(std::string_view in);

// Percent-decode a URL path segment: like percent_decode, but '+' is a literal
// plus sign (it only means "space" inside query strings).
std::optional<std::string> percent_decode_path(std::string_view in);

// Parse "a=1&b=two%20words" into a map. Empty pairs ("a=1&&b=2") are skipped;
// a key without '=' maps to the empty string; a repeated key keeps its LAST
// value. Returns nullopt if ANY pair has a malformed percent-encoding, so that
// malformed input is rejected rather than silently partially applied.
std::optional<QueryMap> parse_query_string(std::string_view qs);

} // namespace ourweb
