#pragma once

#include "ourweb/request.hpp"
#include "ourweb/response.hpp"

#include <concepts>
#include <cstddef>
#include <utility>
#include <variant>

namespace ourweb {

// State threaded through the extractors of ONE handler invocation, in
// parameter order.
struct ExtractContext {
    const Request& request;
    // Index of the next unconsumed route parameter. Each Path<T> extractor
    // consumes one, so the k-th Path<> parameter of a handler binds to the k-th
    // "{param}" of the route pattern.
    std::size_t next_path_param = 0;
};

// Result of one extraction: either a value, or the Response to send instead of
// calling the handler (typically a 400/415 error).
template <typename T>
class Extraction {
public:
    static Extraction ok(T value) {
        return Extraction(std::variant<T, Response>(std::in_place_index<0>, std::move(value)));
    }
    static Extraction fail(Response error) {
        return Extraction(std::variant<T, Response>(std::in_place_index<1>, std::move(error)));
    }

    [[nodiscard]] bool has_value() const noexcept { return state_.index() == 0; }
    explicit operator bool() const noexcept { return has_value(); }

    // Precondition: has_value()
    T take() && { return std::move(std::get<0>(state_)); }
    // Precondition: !has_value()
    Response take_error() && { return std::move(std::get<1>(state_)); }

private:
    explicit Extraction(std::variant<T, Response> state) : state_(std::move(state)) {}
    std::variant<T, Response> state_;
};

// Customisation point: specialise FromRequest<T> to make T usable as a handler
// parameter. Required member:
//
//     static Extraction<T> extract(ExtractContext& ctx);
//
// and optionally `static constexpr std::size_t path_params = N;` if the
// extractor consumes N route parameters (checked against the route pattern when
// the route is registered).
//
// Built in: Request, Path<T>, Json<T>.
//
// Extractors run in parameter order, left to right, and stop at the first
// failure: its Response is returned to the client and the handler is NOT
// called. Extractors must not expose internal error details to clients.
template <typename T>
struct FromRequest;

template <typename T>
concept Extractor = requires(ExtractContext& ctx) {
    { FromRequest<T>::extract(ctx) } -> std::same_as<Extraction<T>>;
};

// Handlers may take a copy of the request...
template <>
struct FromRequest<Request> {
    static Extraction<Request> extract(ExtractContext& ctx) {
        return Extraction<Request>::ok(ctx.request);
    }
};
// ...or, to avoid the copy, borrow it as `const Request&` (handled directly by
// the handler adapter).

} // namespace ourweb
