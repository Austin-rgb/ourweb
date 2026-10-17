#pragma once

// Handler adaptation: turns ordinary functions and lambdas into HandlerFn.
//
// Supported handler signatures
// ----------------------------
//   Response f();
//   Response f(const Request&);           // borrow (zero-copy; preferred)
//   Response f(Request);                  // owning copy
//   Response f(Path<int> id);             // typed path parameter(s)
//   Response f(Json<CreateUser> body);    // typed JSON body
//   Response f(const Request&, Path<int>, Json<Body>);   // any mix, any order
//
// where parameters are `Request`, `const Request&`, or any type with a
// FromRequest<T> specialisation (see extractor.hpp), taken by value, `const&`
// or `&&`; and the return type is Response or convertible to it.
//
// Not supported (and rejected at compile time with a message): generic lambdas
// and overload sets, non-const `operator()` / `mutable` lambdas (handlers run
// concurrently), non-copyable callables, and non-const lvalue-reference
// parameters.
//
// Extractor failures (bad path parameter, invalid JSON, wrong Content-Type,
// ...) short-circuit with a 4xx error response; the handler is not called.
// Exceptions thrown by handlers are turned into 500 responses by the framework.

#include "ourweb/extractor.hpp"
#include "ourweb/request.hpp"
#include "ourweb/response.hpp"
#include "ourweb/router.hpp"

#include <cstddef>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

namespace ourweb {

struct AdaptedHandler {
    HandlerFn fn;
    std::size_t path_params = 0;  // number of Path<> extractors in the signature
};

namespace detail {

template <typename... A>
struct type_list {};

template <typename R, typename... A>
struct SigInfo {
    static constexpr bool supported = true;
    using Ret = R;
    using Args = type_list<A...>;
};

struct Unsupported {
    static constexpr bool supported = false;
    static constexpr bool non_const = false;
};
struct NonConstCall : Unsupported {
    static constexpr bool non_const = true;
};

template <typename T>
struct MemberSig : Unsupported {};
template <typename C, typename R, typename... A, bool N>
struct MemberSig<R (C::*)(A...) const noexcept(N)> : SigInfo<R, A...> {
    static constexpr bool non_const = false;
};
template <typename C, typename R, typename... A, bool N>
struct MemberSig<R (C::*)(A...) noexcept(N)> : NonConstCall {};

template <typename T>
struct FnSig : Unsupported {};
template <typename R, typename... A, bool N>
struct FnSig<R (*)(A...) noexcept(N)> : SigInfo<R, A...> {
    static constexpr bool non_const = false;
};

template <typename H>
struct HandlerSig : Unsupported {};
template <typename H>
    requires std::is_pointer_v<H> && std::is_function_v<std::remove_pointer_t<H>>
struct HandlerSig<H> : FnSig<H> {};
template <typename H>
    requires std::is_class_v<H> && requires { &H::operator(); }
struct HandlerSig<H> : MemberSig<decltype(&H::operator())> {};

template <typename A>
inline constexpr bool is_borrowed_request_v = std::is_same_v<A, const Request&>;

template <typename A>
inline constexpr bool is_value_extractor_v =
    !std::is_lvalue_reference_v<A> || std::is_const_v<std::remove_reference_t<A>>;

template <typename A>
using Decayed = std::remove_cvref_t<A>;

template <typename A>
using Slot = std::conditional_t<is_borrowed_request_v<A>, std::monostate, std::optional<Decayed<A>>>;

template <typename E>
constexpr std::size_t path_param_count() {
    if constexpr (requires { FromRequest<E>::path_params; }) {
        return FromRequest<E>::path_params;
    } else {
        return 0;
    }
}

template <typename A>
void extract_into(Slot<A>& slot, ExtractContext& ctx, std::optional<Response>& failure) {
    if constexpr (is_borrowed_request_v<A>) {
        (void)slot;
        (void)ctx;
        (void)failure;
    } else {
        auto extracted = FromRequest<Decayed<A>>::extract(ctx);
        if (extracted) {
            slot.emplace(std::move(extracted).take());
        } else {
            failure = std::move(extracted).take_error();
        }
    }
}

template <typename A>
decltype(auto) pass_arg(Slot<A>& slot, const Request& req) {
    if constexpr (is_borrowed_request_v<A>) {
        (void)slot;
        return (req);
    } else {
        return std::move(*slot);
    }
}

template <typename H, typename... Args>
AdaptedHandler build(H handler, type_list<Args...>) {
    static_assert(((is_borrowed_request_v<Args> || is_value_extractor_v<Args>) && ...),
                  "ourweb: handler parameters must not be non-const lvalue references");
    static_assert(((is_borrowed_request_v<Args> || Extractor<Decayed<Args>>) && ...),
                  "ourweb: every handler parameter must be Request, const Request&, or a type "
                  "with a FromRequest<T> specialisation (e.g. Path<T>, Json<T>); see "
                  "ourweb/extractor.hpp");

    AdaptedHandler out;
    out.path_params = (std::size_t{0} + ... + path_param_count<Decayed<Args>>());
    out.fn = [handler = std::move(handler)](const Request& req) -> Response {
        ExtractContext ctx{req};
        std::tuple<Slot<Args>...> slots;
        std::optional<Response> failure;

        [&]<std::size_t... I>(std::index_sequence<I...>) {
            // Left to right; stop at the first failure.
            ((failure ? void() : extract_into<Args>(std::get<I>(slots), ctx, failure)), ...);
        }(std::index_sequence_for<Args...>{});
        if (failure) return std::move(*failure);

        return [&]<std::size_t... I>(std::index_sequence<I...>) -> Response {
            (void)req;
            return handler(pass_arg<Args>(std::get<I>(slots), req)...);
        }(std::index_sequence_for<Args...>{});
    };
    return out;
}

} // namespace detail

template <typename F>
AdaptedHandler adapt_handler(F&& f) {
    using H = std::decay_t<F>;
    using Sig = detail::HandlerSig<H>;

    static_assert(!Sig::non_const,
                  "ourweb: handler operator() must be const (handlers are invoked concurrently); "
                  "remove `mutable` or keep mutable state behind a shared_ptr + mutex");
    static_assert(Sig::supported,
                  "ourweb: unsupported handler signature. Handlers must be plain functions or "
                  "non-generic, non-overloaded callables with a const call operator, returning "
                  "Response. See ourweb/handler.hpp for the supported signatures.");
    if constexpr (Sig::supported) {
        static_assert(std::is_copy_constructible_v<H>,
                      "ourweb: handlers must be copy-constructible; capture move-only state via "
                      "std::shared_ptr");
        static_assert(std::is_convertible_v<typename Sig::Ret, Response>,
                      "ourweb: handlers must return ourweb::Response (or a type convertible to it)");
        return detail::build<H>(H(std::forward<F>(f)), typename Sig::Args{});
    } else {
        return {};  // unreachable: the static_asserts above already failed
    }
}

} // namespace ourweb
