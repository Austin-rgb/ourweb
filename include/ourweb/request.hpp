#pragma once

#include "ourweb/http.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ourweb {

// Path parameters captured by the router, in the order they appear in the
// route pattern: {{"id", "42"}, {"post", "7"}} for "/users/{id}/posts/{post}".
using PathParams = std::vector<std::pair<std::string, std::string>>;

// An HTTP request as seen by middleware and handlers.
//
// Ownership/lifetime: the framework owns the Request for the duration of
// App::handle_request. Middleware and handlers taking `const Request&` borrow
// it and must not retain the reference (or any string_view obtained from it)
// after returning. Handlers taking `Request` by value receive an independent
// copy and may keep it.
class Request {
public:
    Request() = default;

    // Constructed by the framework; users typically receive by value or const ref.
    Request(HttpMethod method,
            std::string path,
            std::unordered_map<std::string, std::string> headers,
            std::string body,
            std::unordered_map<std::string, std::string> query);

    [[nodiscard]] HttpMethod method() const noexcept { return method_; }
    // The request path exactly as received (still percent-encoded), without the
    // query string. Path parameters captured by the router are decoded.
    [[nodiscard]] std::string_view path() const noexcept { return path_; }
    [[nodiscard]] std::string_view body() const noexcept { return body_; }

    // Case-insensitive header lookup
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const;

    // All headers (keys stored lower-case)
    [[nodiscard]] const std::unordered_map<std::string, std::string>& headers() const noexcept {
        return headers_;
    }

    // Query parameters (percent-decoded). If a name occurs more than once the
    // LAST occurrence wins.
    [[nodiscard]] const std::unordered_map<std::string, std::string>& query() const noexcept {
        return query_;
    }

    [[nodiscard]] std::optional<std::string_view> query_param(std::string_view name) const;

    // Path parameters captured during routing (set by the router; decoded).
    void set_path_params(PathParams params);
    [[nodiscard]] const PathParams& path_params() const noexcept { return path_params_; }
    [[nodiscard]] std::optional<std::string_view> path_param(std::string_view name) const;
    // Positional access: (name, value) of the index-th parameter in the pattern.
    [[nodiscard]] std::optional<std::pair<std::string_view, std::string_view>>
    path_param_at(std::size_t index) const;

private:
    HttpMethod method_ = HttpMethod::Unknown;
    std::string path_;
    std::unordered_map<std::string, std::string> headers_; // keys lower-case
    std::string body_;
    std::unordered_map<std::string, std::string> query_;
    PathParams path_params_;
};

} // namespace ourweb
