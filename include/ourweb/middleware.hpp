#pragma once

#include "ourweb/request.hpp"
#include "ourweb/response.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace ourweb {

namespace detail {
class ChainState;
}

// Middleware semantics
// --------------------
// Middleware runs in registration order on the way in (the first registered
// middleware is the OUTERMOST layer) and in reverse order on the way out:
//
//     app.wrap(A); app.wrap(B);   =>   A-before, B-before, handler, B-after, A-after
//
// A middleware either
//   * calls next() exactly once and may inspect/modify the Response it
//     returns, or
//   * returns its own Response without calling next() to short-circuit
//     (e.g. 401/403); inner middleware and the handler then do not run.
//
// Guarantees:
//   * The `const Request&` is the very same object seen by inner layers and the
//     handler: no copies are made, and it remains valid and unmodified after
//     next() returns (so a logger can read req.path() afterwards).
//   * next() never throws. If an inner layer or the handler throws, the
//     exception is logged and next() returns a 500 response, so outer
//     middleware always observes a Response.
//   * Calling next() a second time throws std::logic_error (the chain runs at
//     most once per request). Use the result of the first call instead.
//   * `next` and the request are only valid for the duration of invoke(); do
//     not store them or call next() from another thread.
//   * Routing has already happened when middleware runs, so path parameters are
//     available; 404/405 responses are produced after the innermost layer and
//     are therefore visible to middleware.
//
// Ownership: the App owns registered middleware (shared_ptr) for its whole
// lifetime. Middleware objects are invoked concurrently from multiple
// connection threads and must therefore be thread-safe (stateless, or
// internally synchronised).
class MiddlewareNext {
public:
    // Wrap an arbitrary callable. Mainly useful for unit-testing a middleware
    // in isolation: `mw.invoke(req, [] { return text("ok"); })`.
    template <typename F>
        requires(!std::is_same_v<std::remove_cvref_t<F>, MiddlewareNext> &&
                 std::is_invocable_r_v<Response, F&>)
    MiddlewareNext(F&& f)  // NOLINT(google-explicit-constructor)
        : fn_(std::make_shared<std::function<Response()>>(std::forward<F>(f))) {}

    Response operator()() const;

private:
    friend class detail::ChainState;
    MiddlewareNext(detail::ChainState* chain, std::size_t index) noexcept
        : chain_(chain), index_(index) {}

    std::shared_ptr<std::function<Response()>> fn_;  // standalone callable, or
    detail::ChainState* chain_ = nullptr;            // position in a live chain
    std::size_t index_ = 0;
};

using MiddlewareFn = std::function<Response(const Request&, MiddlewareNext)>;

class Middleware {
public:
    virtual ~Middleware() = default;

    // See the semantics above. Must be thread-safe.
    virtual Response invoke(const Request& req, MiddlewareNext next) = 0;
};

// Simple request-logging middleware: "<prefix> METHOD /path -> status (N ms)".
// Each line is written atomically.
class LoggerMiddleware : public Middleware {
public:
    explicit LoggerMiddleware(std::string prefix = "[ourweb]");

    Response invoke(const Request& req, MiddlewareNext next) override;

private:
    std::string prefix_;
};

// Function-based middleware adapter
class FunctionMiddleware : public Middleware {
public:
    explicit FunctionMiddleware(MiddlewareFn fn) : fn_(std::move(fn)) {}

    Response invoke(const Request& req, MiddlewareNext next) override {
        return fn_(req, std::move(next));
    }

private:
    MiddlewareFn fn_;
};

} // namespace ourweb
