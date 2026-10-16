#pragma once

#include "ourweb/response.hpp"
#include "ourweb/request.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace ourweb {

// Serialize any type that nlohmann/json can handle
template <typename T>
[[nodiscard]] Response json(const T& value, HttpStatus status = HttpStatus::Ok) {
    nlohmann::json j = value;
    return json_raw(j.dump(), status);
}

// Json extractor for handlers
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

// Helper to parse Json<T> from request body; returns nullopt on failure
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

} // namespace ourweb
