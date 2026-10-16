#pragma once

#include "ourweb/http.hpp"
#include "ourweb/response.hpp"

#include <string>
#include <string_view>

namespace ourweb {

struct Error {
    HttpStatus status = HttpStatus::InternalServerError;
    std::string code;
    std::string message;
};

[[nodiscard]] Response error_response(const Error& err);
[[nodiscard]] Response error_response(HttpStatus status, std::string_view code, std::string_view message);

// Common error helpers
[[nodiscard]] Response invalid_parameter(std::string_view message);
[[nodiscard]] Response missing_parameter(std::string_view name);
[[nodiscard]] Response invalid_json(std::string_view message = "Invalid JSON body");
[[nodiscard]] Response body_too_large(std::string_view message = "Request body too large");
[[nodiscard]] Response unsupported_media_type(std::string_view message = "Unsupported Media Type");

} // namespace ourweb
