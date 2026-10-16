#pragma once

#include "ourweb/http.hpp"
#include "ourweb/request.hpp"
#include "ourweb/response.hpp"
#include "ourweb/router.hpp"
#include "ourweb/state.hpp"
#include "ourweb/middleware.hpp"
#include "ourweb/json.hpp"
#include "ourweb/path.hpp"
#include "ourweb/error.hpp"
#include "ourweb/version.hpp"

#include <concepts>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ourweb {

class Server; // forward

class App {
public:
    App();
    ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;
    App(App&&) noexcept;
    App& operator=(App&&) noexcept;

    // --- Route registration -------------------------------------------------

    template <typename Handler>
    App& get(std::string path, Handler&& handler) {
        return route(HttpMethod::Get, std::move(path), std::forward<Handler>(handler));
    }

    template <typename Handler>
    App& post(std::string path, Handler&& handler) {
        return route(HttpMethod::Post, std::move(path), std::forward<Handler>(handler));
    }

    template <typename Handler>
    App& put(std::string path, Handler&& handler) {
        return route(HttpMethod::Put, std::move(path), std::forward<Handler>(handler));
    }

    template <typename Handler>
    App& patch(std::string path, Handler&& handler) {
        return route(HttpMethod::Patch, std::move(path), std::forward<Handler>(handler));
    }

    template <typename Handler>
    App& del(std::string path, Handler&& handler) { // 'delete' is a keyword
        return route(HttpMethod::Delete, std::move(path), std::forward<Handler>(handler));
    }

    template <typename Handler>
    App& head(std::string path, Handler&& handler) {
        return route(HttpMethod::Head, std::move(path), std::forward<Handler>(handler));
    }

    // Generic route
    template <typename Handler>
    App& route(HttpMethod method, std::string path, Handler&& handler) {
        router_.add(method, std::move(path), make_handler(std::forward<Handler>(handler)));
        return *this;
    }

    // --- Application state --------------------------------------------------

    // Register state by value (T must be move-constructible).
    template <typename T>
        requires std::move_constructible<T>
    App& state(T value) {
        state_registry_.set(std::move(value));
        return *this;
    }

    // Register state that is already owned by a shared_ptr. Use this for types
    // that cannot be moved, e.g. a struct containing a std::mutex:
    //   app.state(std::make_shared<AppState>());
    template <typename T>
    App& state(std::shared_ptr<T> ptr) {
        state_registry_.set(std::move(ptr));
        return *this;
    }

    // Construct state in place: app.emplace_state<AppState>(args...)
    template <typename T, typename... Args>
    App& emplace_state(Args&&... args) {
        state_registry_.emplace<T>(std::forward<Args>(args)...);
        return *this;
    }

    // --- Middleware ---------------------------------------------------------

    App& wrap(std::shared_ptr<Middleware> mw);
    App& wrap(MiddlewareFn fn);

    // Convenience: Logger
    App& use_logger(std::string prefix = "[ourweb]");

    // --- Configuration ------------------------------------------------------

    App& max_body_size(std::size_t bytes) {
        max_body_size_ = bytes;
        return *this;
    }

    [[nodiscard]] std::size_t max_body_size() const noexcept { return max_body_size_; }

    // --- Lifecycle ----------------------------------------------------------

    // Bind and run the server. Blocks until shutdown.
    // address format: "127.0.0.1:8080" or "0.0.0.0:8080"
    void run(std::string_view address);

    // Request graceful shutdown (can be called from another thread / signal)
    void stop();

    // Internal: process a single request (used by Server)
    [[nodiscard]] Response handle_request(Request req) const;

    [[nodiscard]] const StateRegistry& state_registry() const noexcept { return state_registry_; }
    [[nodiscard]] StateRegistry& state_registry() noexcept { return state_registry_; }

private:
    // Handler adaptation: turn various callable signatures into HandlerFn
    template <typename Handler>
    HandlerFn make_handler(Handler&& h);

    // Specializations / overloads for common signatures are implemented via
    // SFINAE / if constexpr in the .cpp-friendly way, but since templates
    // must be in header we put the logic here.

    Router router_;
    StateRegistry state_registry_;
    std::vector<std::shared_ptr<Middleware>> middlewares_;
    std::size_t max_body_size_ = 1024 * 1024; // 1 MiB default
    std::unique_ptr<Server> server_;
};

// ---------------------------------------------------------------------------
// Handler adaptation helpers (header-only for template flexibility)
// ---------------------------------------------------------------------------

namespace detail {

// Detect if callable takes zero arguments
template <typename F>
concept ZeroArg = requires(F f) {
    { f() } -> std::convertible_to<Response>;
};

template <typename F>
concept RequestArg = requires(F f, Request r) {
    { f(std::move(r)) } -> std::convertible_to<Response>;
};

} // namespace detail

template <typename Handler>
HandlerFn App::make_handler(Handler&& h) {
    using H = std::decay_t<Handler>;

    // Case 1: zero-argument handler returning Response
    if constexpr (detail::ZeroArg<H>) {
        return [handler = std::forward<Handler>(h)](Request) -> Response {
            return handler();
        };
    }
    // Case 2: single Request argument
    else if constexpr (detail::RequestArg<H>) {
        return [handler = std::forward<Handler>(h)](Request req) -> Response {
            return handler(std::move(req));
        };
    }
    // Anything already usable as Response(Request) (e.g. a std::function)
    else if constexpr (std::is_constructible_v<HandlerFn, Handler>) {
        return HandlerFn(std::forward<Handler>(h));
    }
    else {
        static_assert(detail::ZeroArg<H> || detail::RequestArg<H>,
                      "ourweb: unsupported handler signature. Handlers must be "
                      "callable as Response() or Response(Request), and must "
                      "not be generic lambdas or overload sets.");
        return {};
    }
}

} // namespace ourweb
