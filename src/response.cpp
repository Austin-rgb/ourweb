#include "ourweb/response.hpp"

#include <cctype>
#include <stdexcept>

namespace ourweb {

namespace {

bool is_valid_header_name(std::string_view name) {
    if (name.empty()) return false;
    for (char c : name) {
        // RFC 7230 token
        if (!(std::isalnum(static_cast<unsigned char>(c)) ||
              c == '!' || c == '#' || c == '$' || c == '%' || c == '&' ||
              c == '\'' || c == '*' || c == '+' || c == '-' || c == '.' ||
              c == '^' || c == '_' || c == '`' || c == '|' || c == '~')) {
            return false;
        }
    }
    return true;
}

bool is_valid_header_value(std::string_view value) {
    for (char c : value) {
        unsigned char uc = static_cast<unsigned char>(c);
        // No control characters except HTAB
        if (uc < 0x20 && c != '\t') return false;
        if (uc == 0x7F) return false;
    }
    return true;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

} // namespace

Response& Response::header(std::string name, std::string value) {
    if (!is_valid_header_name(name)) {
        throw std::invalid_argument("Invalid HTTP header name");
    }
    if (!is_valid_header_value(value)) {
        throw std::invalid_argument("Invalid HTTP header value");
    }
    // Header names are case-insensitive: setting "content-type" must replace an
    // existing "Content-Type" rather than emit both on the wire. The most
    // recently supplied casing is kept.
    for (auto it = headers_.begin(); it != headers_.end();) {
        if (iequals(it->first, name)) {
            it = headers_.erase(it);
        } else {
            ++it;
        }
    }
    headers_[std::move(name)] = std::move(value);
    return *this;
}

Response& Response::content_type(std::string_view type) {
    // Only set if not already present (case-insensitive check simplified)
    for (const auto& [k, v] : headers_) {
        std::string lower = k;
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower == "content-type") {
            return *this;
        }
    }
    headers_["Content-Type"] = std::string(type);
    return *this;
}

Response text(std::string content, HttpStatus status) {
    Response r(status);
    r.content_type("text/plain; charset=utf-8");
    r.body(std::move(content));
    return r;
}

Response html(std::string content, HttpStatus status) {
    Response r(status);
    r.content_type("text/html; charset=utf-8");
    r.body(std::move(content));
    return r;
}

Response json_raw(std::string content, HttpStatus status) {
    Response r(status);
    r.content_type("application/json");
    r.body(std::move(content));
    return r;
}

Response no_content() {
    return Response(HttpStatus::NoContent);
}

Response not_found(std::string message) {
    return text(std::move(message), HttpStatus::NotFound);
}

Response bad_request(std::string message) {
    return text(std::move(message), HttpStatus::BadRequest);
}

Response method_not_allowed(std::string message) {
    return text(std::move(message), HttpStatus::MethodNotAllowed);
}

Response internal_error(std::string message) {
    return text(std::move(message), HttpStatus::InternalServerError);
}

} // namespace ourweb
