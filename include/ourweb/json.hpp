#pragma once

#include "ourweb/error.hpp"
#include "ourweb/extractor.hpp"
#include "ourweb/request.hpp"
#include "ourweb/response.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace ourweb {

// Serialize any type that nlohmann/json can handle
template <typename T>
[[nodiscard]] Response json(const T& value, HttpStatus status = HttpStatus::Ok) {
    nlohmann::json j = value;
    return json_raw(j.dump(), status);
}

// JSON request-body extractor. Use as a handler parameter:
//
//     app.post("/users", [](Json<CreateUser> in) { ... in->name ... });
//
// T is deserialised with nlohmann/json's `from_json` customisation point. The
// request is rejected and the handler is NOT called when:
//   * Content-Type is not application/json (or application/*+json)  -> 415
//   * the body is empty or not valid JSON                          -> 400
//   * the JSON does not fit T (from_json throws)                   -> 400
// Error messages are generic; parser/exception details are never sent to the
// client. The body size limit is enforced earlier, by the HTTP parser (413).
template <typename T>
class Json {
public:
    explicit Json(T value) : value_(std::move(value)) {}

    const T& operator*() const noexcept { return value_; }
    T& operator*() noexcept { return value_; }
    const T* operator->() const noexcept { return &value_; }
    T* operator->() noexcept { return &value_; }

    const T& value() const noexcept { return value_; }
    T& value() noexcept { return value_; }

private:
    T value_;
};

namespace detail {

// "application/json", "Application/JSON; charset=utf-8", "application/vnd.api+json"
inline bool is_json_content_type(std::optional<std::string_view> header) {
    if (!header) return false;
    std::string_view v = *header;
    if (const auto semi = v.find(';'); semi != std::string_view::npos) v = v.substr(0, semi);
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.remove_prefix(1);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.remove_suffix(1);
    std::string lower(v);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower == "application/json") return true;
    constexpr std::string_view prefix = "application/";
    constexpr std::string_view suffix = "+json";
    return lower.size() > prefix.size() + suffix.size() && lower.compare(0, prefix.size(), prefix) == 0 &&
           lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) == 0;
}

} // namespace detail

// Lenient helper (no Content-Type check); returns nullopt on any failure.
template <typename T>
[[nodiscard]] std::optional<Json<T>> parse_json(const Request& req) {
    try {
        auto j = nlohmann::json::parse(req.body());
        return Json<T>{j.get<T>()};
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

template <typename T>
struct FromRequest<Json<T>> {
    static Extraction<Json<T>> extract(ExtractContext& ctx) {
        const Request& req = ctx.request;
        if (!detail::is_json_content_type(req.header("content-type"))) {
            return Extraction<Json<T>>::fail(
                unsupported_media_type("Expected Content-Type: application/json"));
        }
        if (req.body().empty()) {
            return Extraction<Json<T>>::fail(invalid_json("Request body is empty"));
        }

        nlohmann::json parsed;
        try {
            parsed = nlohmann::json::parse(req.body());
        } catch (const std::exception&) {
            return Extraction<Json<T>>::fail(invalid_json("Request body is not valid JSON"));
        }
        try {
            return Extraction<Json<T>>::ok(Json<T>{parsed.get<T>()});
        } catch (const std::exception&) {
            // from_json may throw nlohmann exceptions or the application's own.
            return Extraction<Json<T>>::fail(
                invalid_json("Request body does not have the expected structure"));
        }
    }
};

} // namespace ourweb
