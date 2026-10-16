#pragma once

#include "ourweb/http.hpp"

#include <concepts>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ourweb {

class Response {
public:
    Response() = default;
    explicit Response(HttpStatus status) : status_(status) {}

    Response& status(HttpStatus s) {
        status_ = s;
        return *this;
    }

    Response& status(std::uint16_t code) {
        status_ = static_cast<HttpStatus>(code);
        return *this;
    }

    Response& header(std::string name, std::string value);

    Response& body(std::string content) {
        body_ = std::move(content);
        return *this;
    }

    [[nodiscard]] HttpStatus status_code() const noexcept { return status_; }
    [[nodiscard]] const std::unordered_map<std::string, std::string>& headers() const noexcept {
        return headers_;
    }
    [[nodiscard]] std::string_view body() const noexcept { return body_; }
    [[nodiscard]] bool empty_body() const noexcept { return body_.empty(); }

    // Convenience: set Content-Type if not already present
    Response& content_type(std::string_view type);

private:
    HttpStatus status_ = HttpStatus::Ok;
    std::unordered_map<std::string, std::string> headers_;
    std::string body_;
};

// Convenience response builders
[[nodiscard]] Response text(std::string content, HttpStatus status = HttpStatus::Ok);

// Accepts std::string_view (which does not convert implicitly to std::string).
// Constrained so string literals and std::string still pick the overload above.
template <typename S>
    requires(std::convertible_to<const S&, std::string_view> &&
             !std::convertible_to<S, std::string>)
[[nodiscard]] Response text(const S& content, HttpStatus status = HttpStatus::Ok) {
    return text(std::string(std::string_view(content)), status);
}
[[nodiscard]] Response html(std::string content, HttpStatus status = HttpStatus::Ok);
[[nodiscard]] Response json_raw(std::string content, HttpStatus status = HttpStatus::Ok);
[[nodiscard]] Response no_content();
[[nodiscard]] Response not_found(std::string message = "Not Found");
[[nodiscard]] Response bad_request(std::string message = "Bad Request");
[[nodiscard]] Response method_not_allowed(std::string message = "Method Not Allowed");
[[nodiscard]] Response internal_error(std::string message = "Internal Server Error");

} // namespace ourweb
