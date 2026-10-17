#pragma once

#include "ourweb/error.hpp"
#include "ourweb/extractor.hpp"
#include "ourweb/request.hpp"

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace ourweb {

// Typed path parameter. Use as a handler parameter:
//
//     app.get("/users/{id}", [](Path<int> id) { ... *id ... });
//     app.get("/a/{x}/b/{y}", [](Path<int> x, Path<std::string> y) { ... });
//
// The k-th Path<> parameter of the handler binds to the k-th "{param}" of the
// route pattern (by position, like Actix). The value is percent-decoded. A
// value that does not convert (e.g. "abc" for Path<int>, or an out-of-range
// number) is rejected with 400 and the handler is not called. Registration
// fails if the handler has more Path<> parameters than the pattern has
// "{param}" segments.
//
// Supported T: std::string and all integral types except bool. Add more by
// specialising ourweb::detail::path_converter<T>.
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
// Rejects empty input, trailing garbage ("12abc"), a leading '+', and overflow.
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

// Name-based extraction, for use outside the handler adapter.
template <typename T>
[[nodiscard]] std::optional<Path<T>> extract_path(const Request& req, std::string_view name) {
    auto value = req.path_param(name);
    if (!value) {
        return std::nullopt;
    }
    auto converted = detail::path_converter<T>::convert(*value);
    if (!converted) {
        return std::nullopt;
    }
    return Path<T>{std::move(*converted)};
}

template <typename T>
    requires requires(std::string_view s) { detail::path_converter<T>::convert(s); }
struct FromRequest<Path<T>> {
    static constexpr std::size_t path_params = 1;

    static Extraction<Path<T>> extract(ExtractContext& ctx) {
        const auto param = ctx.request.path_param_at(ctx.next_path_param++);
        if (!param) {
            // Only reachable if the route was registered without validation.
            return Extraction<Path<T>>::fail(missing_parameter("path parameter"));
        }
        auto converted = detail::path_converter<T>::convert(param->second);
        if (!converted) {
            // The parameter *name* comes from the developer's route pattern; the
            // client-supplied value is deliberately not echoed back.
            return Extraction<Path<T>>::fail(invalid_parameter(
                "Invalid value for path parameter '" + std::string(param->first) + "'"));
        }
        return Extraction<Path<T>>::ok(Path<T>{std::move(*converted)});
    }
};

} // namespace ourweb
