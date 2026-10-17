#include "response_writer.hpp"

#include "ourweb/http.hpp"

#include <cctype>
#include <string_view>

namespace ourweb {

namespace {

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

std::string serialize_response(const Response& response, bool is_head) {
    auto code = static_cast<int>(response.status_code());
    if (code < 200 || code > 599) {
        return serialize_response(internal_error(), is_head);
    }
    const auto status = response.status_code();

    const bool bodiless = (code == 204 || code == 304);
    const std::string_view body = bodiless ? std::string_view{} : response.body();

    std::string out;
    out.reserve(256 + (is_head ? 0 : body.size()));
    out += "HTTP/1.1 ";
    out += std::to_string(code);
    out += ' ';
    out += status_reason(status);
    out += "\r\n";

    for (const auto& [name, value] : response.headers()) {
        if (iequals(name, "content-length") || iequals(name, "transfer-encoding") ||
            iequals(name, "connection")) {
            continue;
        }
        out += name;
        out += ": ";
        out += value;
        out += "\r\n";
    }
    if (!bodiless) {
        out += "Content-Length: ";
        out += std::to_string(body.size());
        out += "\r\n";
    }
    out += "Connection: close\r\n\r\n";
    if (!is_head) out += body;
    return out;
}

} // namespace ourweb
