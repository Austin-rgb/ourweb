#pragma once

#include "ourweb/http.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ourweb {

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
    [[nodiscard]] std::string_view path() const noexcept { return path_; }
    [[nodiscard]] std::string_view body() const noexcept { return body_; }

    // Case-insensitive header lookup
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const;

    // All headers (keys stored lower-case)
    [[nodiscard]] const std::unordered_map<std::string, std::string>& headers() const noexcept {
        return headers_;
    }

    // Query parameters (percent-decoded)
    [[nodiscard]] const std::unordered_map<std::string, std::string>& query() const noexcept {
        return query_;
    }

    [[nodiscard]] std::optional<std::string_view> query_param(std::string_view name) const;

    // Path parameters captured during routing (set by router)
    void set_path_params(std::unordered_map<std::string, std::string> params);
    [[nodiscard]] const std::unordered_map<std::string, std::string>& path_params() const noexcept {
        return path_params_;
    }
    [[nodiscard]] std::optional<std::string_view> path_param(std::string_view name) const;

private:
    HttpMethod method_ = HttpMethod::Unknown;
    std::string path_;
    std::unordered_map<std::string, std::string> headers_; // keys lower-case
    std::string body_;
    std::unordered_map<std::string, std::string> query_;
    std::unordered_map<std::string, std::string> path_params_;
};

} // namespace ourweb
