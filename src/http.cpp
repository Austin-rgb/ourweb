#include "ourweb/http.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace ourweb {

std::string_view to_string(HttpMethod method) noexcept {
    switch (method) {
        case HttpMethod::Get:     return "GET";
        case HttpMethod::Post:    return "POST";
        case HttpMethod::Put:     return "PUT";
        case HttpMethod::Patch:   return "PATCH";
        case HttpMethod::Delete:  return "DELETE";
        case HttpMethod::Head:    return "HEAD";
        case HttpMethod::Options: return "OPTIONS";
        case HttpMethod::Trace:   return "TRACE";
        case HttpMethod::Connect: return "CONNECT";
        default:                  return "UNKNOWN";
    }
}

HttpMethod parse_method(std::string_view method) noexcept {
    if (method == "GET")     return HttpMethod::Get;
    if (method == "POST")    return HttpMethod::Post;
    if (method == "PUT")     return HttpMethod::Put;
    if (method == "PATCH")   return HttpMethod::Patch;
    if (method == "DELETE")  return HttpMethod::Delete;
    if (method == "HEAD")    return HttpMethod::Head;
    if (method == "OPTIONS") return HttpMethod::Options;
    if (method == "TRACE")   return HttpMethod::Trace;
    if (method == "CONNECT") return HttpMethod::Connect;
    return HttpMethod::Unknown;
}

std::string_view status_reason(HttpStatus status) noexcept {
    switch (status) {
        case HttpStatus::Ok:                    return "OK";
        case HttpStatus::Created:               return "Created";
        case HttpStatus::Accepted:              return "Accepted";
        case HttpStatus::NoContent:             return "No Content";
        case HttpStatus::BadRequest:            return "Bad Request";
        case HttpStatus::Unauthorized:          return "Unauthorized";
        case HttpStatus::Forbidden:             return "Forbidden";
        case HttpStatus::NotFound:              return "Not Found";
        case HttpStatus::MethodNotAllowed:      return "Method Not Allowed";
        case HttpStatus::RequestEntityTooLarge: return "Request Entity Too Large";
        case HttpStatus::UnsupportedMediaType:  return "Unsupported Media Type";
        case HttpStatus::InternalServerError:   return "Internal Server Error";
        case HttpStatus::NotImplemented:        return "Not Implemented";
        case HttpStatus::ServiceUnavailable:    return "Service Unavailable";
        default:                                return "Unknown";
    }
}

std::string_view to_string(HttpStatus status) noexcept {
    return status_reason(status);
}

} // namespace ourweb
