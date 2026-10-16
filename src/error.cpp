#include "ourweb/error.hpp"
#include "ourweb/json.hpp"

#include <nlohmann/json.hpp>

namespace ourweb {

Response error_response(const Error& err) {
    nlohmann::json j;
    j["error"]["code"] = err.code;
    j["error"]["message"] = err.message;
    return json_raw(j.dump(), err.status);
}

Response error_response(HttpStatus status, std::string_view code, std::string_view message) {
    return error_response(Error{status, std::string(code), std::string(message)});
}

Response invalid_parameter(std::string_view message) {
    return error_response(HttpStatus::BadRequest, "invalid_parameter", message);
}

Response missing_parameter(std::string_view name) {
    return error_response(HttpStatus::BadRequest, "missing_parameter",
                          std::string("Missing required parameter: ") + std::string(name));
}

Response invalid_json(std::string_view message) {
    return error_response(HttpStatus::BadRequest, "invalid_json", message);
}

Response body_too_large(std::string_view message) {
    return error_response(HttpStatus::RequestEntityTooLarge, "body_too_large", message);
}

Response unsupported_media_type(std::string_view message) {
    return error_response(HttpStatus::UnsupportedMediaType, "unsupported_media_type", message);
}

} // namespace ourweb
