#pragma once

#include "ourweb/request.hpp"

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace ourweb {

template <typename T>
class Path {
public:
    explicit Path(T value) : value_(std::move(value)) {}

    const T& operator*() const noexcept { return value_; }
    T& operator*() noexcept { return value_; }
    const T* operator->() const noexcept { return &value_; }
    T* operator->() noexcept { return &value_; }

    const T& value() const noexcept { return value_; }
    T& value() noexcept { return value_; }

private:
    T value_;
};

// Conversion helpers
namespace detail {

template <typename T>
struct path_converter;

template <>
struct path_converter<std::string> {
    static std::optional<std::string> convert(std::string_view s) {
        return std::string(s);
    }
};

// Generic integral converter — works for int, long, long long, std::int32_t,
// std::int64_t, etc. without colliding when those types are aliases of each other.
template <typename T>
    requires std::is_integral_v<T> && (!std::is_same_v<T, bool>)
struct path_converter<T> {
    static std::optional<T> convert(std::string_view s) {
        T value{};
        auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
        if (ec == std::errc{} && ptr == s.data() + s.size()) {
            return value;
        }
        return std::nullopt;
    }
};

} // namespace detail

template <typename T>
[[nodiscard]] std::optional<Path<T>> extract_path(const Request& req, std::string_view name) {
    auto it = req.path_params().find(std::string(name));
    if (it == req.path_params().end()) {
        return std::nullopt;
    }
    auto converted = detail::path_converter<T>::convert(it->second);
    if (!converted) {
        return std::nullopt;
    }
    return Path<T>{std::move(*converted)};
}

} // namespace ourweb
