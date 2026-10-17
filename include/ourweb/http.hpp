#pragma once

#include <string>
#include <string_view>
#include <cstdint>

namespace ourweb {

enum class HttpMethod : std::uint8_t {
    Get,
    Post,
    Put,
    Patch,
    Delete,
    Head,
    Options,
    Trace,
    Connect,
    Unknown
};

[[nodiscard]] std::string_view to_string(HttpMethod method) noexcept;
[[nodiscard]] HttpMethod parse_method(std::string_view method) noexcept;

enum class HttpStatus : std::uint16_t {
    Continue = 100,
    Ok = 200,
    Created = 201,
    Accepted = 202,
    NoContent = 204,
    NotModified = 304,
    BadRequest = 400,
    Unauthorized = 401,
    Forbidden = 403,
    NotFound = 404,
    MethodNotAllowed = 405,
    RequestTimeout = 408,
    RequestEntityTooLarge = 413,
    UriTooLong = 414,
    UnsupportedMediaType = 415,
    ExpectationFailed = 417,
    RequestHeaderFieldsTooLarge = 431,
    InternalServerError = 500,
    NotImplemented = 501,
    ServiceUnavailable = 503,
    HttpVersionNotSupported = 505
};

[[nodiscard]] std::string_view status_reason(HttpStatus status) noexcept;
[[nodiscard]] std::string_view to_string(HttpStatus status) noexcept;

} // namespace ourweb
