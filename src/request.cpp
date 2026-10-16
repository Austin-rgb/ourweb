#include "ourweb/request.hpp"

#include <algorithm>
#include <cctype>

namespace ourweb {

namespace {

std::string to_lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

} // namespace

Request::Request(HttpMethod method,
                 std::string path,
                 std::unordered_map<std::string, std::string> headers,
                 std::string body,
                 std::unordered_map<std::string, std::string> query)
    : method_(method)
    , path_(std::move(path))
    , headers_(std::move(headers))
    , body_(std::move(body))
    , query_(std::move(query)) {}

std::optional<std::string_view> Request::header(std::string_view name) const {
    auto key = to_lower(name);
    auto it = headers_.find(key);
    if (it == headers_.end()) {
        return std::nullopt;
    }
    return std::string_view(it->second);
}

std::optional<std::string_view> Request::query_param(std::string_view name) const {
    auto it = query_.find(std::string(name));
    if (it == query_.end()) {
        return std::nullopt;
    }
    return std::string_view(it->second);
}

void Request::set_path_params(std::unordered_map<std::string, std::string> params) {
    path_params_ = std::move(params);
}

std::optional<std::string_view> Request::path_param(std::string_view name) const {
    auto it = path_params_.find(std::string(name));
    if (it == path_params_.end()) {
        return std::nullopt;
    }
    return std::string_view(it->second);
}

} // namespace ourweb
