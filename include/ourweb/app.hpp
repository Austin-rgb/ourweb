#pragma once

#include "ourweb/error.hpp"
#include "ourweb/extractor.hpp"
#include "ourweb/handler.hpp"
#include "ourweb/http.hpp"
#include "ourweb/json.hpp"
#include "ourweb/middleware.hpp"
#include "ourweb/path.hpp"
#include "ourweb/request.hpp"
#include "ourweb/response.hpp"
#include "ourweb/router.hpp"
#include "ourweb/server_options.hpp"
#include "ourweb/state.hpp"
#include "ourweb/version.hpp"

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ourweb {

namespace detail {
struct RunState;
}

// The application: routes, middleware, state and the HTTP server.
//
// Threading model: configure the App (routes, middleware, state, options) on
// one thread, then call run(). Handlers and middleware are invoked
// concurrently from multiple connection threads, so they - and any state they
// share - must be thread-safe. Configuration is frozen while the server runs:
// registering a route/middleware/state or changing options then throws
// std::logic_error (FR-01.7).
//
// Lifetime: run() blocks until stop(). When it returns, every connection
// thread has been joined; nothing keeps running in the background. Destroying
// an App that is running (from another thread) stops it and waits for run() to
// return. Do not move a running App.
class App {
public:
    App();
    ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;
    App(App&&) noexcept;
    App& operator=(App&&) noexcept;

    // --- Route registration -------------------------------------------------
    //
    // `handler` may be any supported handler signature (see ourweb/handler.hpp):
    // Response(), Response(const Request&), Response(Request), or any mix of
    // Request / Path<T> / Json<T> parameters.
    //
    // Throws RouteError for an invalid pattern, a duplicate route (same method
    // and shape, even with different parameter names), or a handler with more
    // Path<> parameters than the pattern has "{param}" segments.

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
        AdaptedHandler adapted = adapt_handler(std::forward<Handler>(handler));
        ensure_configurable("register a route");
        router_.add(method, std::move(path), std::move(adapted.fn), adapted.path_params);
        return *this;
    }

    // --- Application state --------------------------------------------------

    // Register state by value (T must be move-constructible).
    template <typename T>
        requires std::move_constructible<T>
    App& state(T value) {
        ensure_configurable("register state");
        state_registry_.set(std::move(value));
        return *this;
    }

    // Register state that is already owned by a shared_ptr. Use this for types
    // that cannot be moved, e.g. a struct containing a std::mutex:
    //   app.state(std::make_shared<AppState>());
    template <typename T>
    App& state(std::shared_ptr<T> ptr) {
        ensure_configurable("register state");
        state_registry_.set(std::move(ptr));
        return *this;
    }

    // Construct state in place: app.emplace_state<AppState>(args...)
    template <typename T, typename... Args>
    App& emplace_state(Args&&... args) {
        ensure_configurable("register state");
        state_registry_.emplace<T>(std::forward<Args>(args)...);
        return *this;
    }

    // --- Middleware ---------------------------------------------------------
    // Execution order and guarantees are documented in ourweb/middleware.hpp.

    App& wrap(std::shared_ptr<Middleware> mw);
    App& wrap(MiddlewareFn fn);

    // Convenience: Logger
    App& use_logger(std::string prefix = "[ourweb]");

    // --- Configuration ------------------------------------------------------

    // Replace all server options (limits and timeouts).
    App& options(ServerOptions opts);
    [[nodiscard]] const ServerOptions& options() const noexcept { return options_; }

    // Shortcut for options().max_body_size (default 1 MiB; larger -> 413).
    App& max_body_size(std::size_t bytes);
    [[nodiscard]] std::size_t max_body_size() const noexcept { return options_.max_body_size; }

    // --- Lifecycle ----------------------------------------------------------

    // Bind and run the server. Blocks until stop(); on return all connection
    // threads have been joined. address: "127.0.0.1:8080", "0.0.0.0:8080",
    // "localhost:8080", "[::1]:8080"; port 0 picks a free port (see
    // local_port()). Throws std::runtime_error with a descriptive message when
    // the address is invalid or cannot be bound, std::logic_error if already
    // running.
    void run(std::string_view address);

    // Request graceful shutdown of a running server; no-op if not running.
    // Thread-safe, idempotent, and safe to call from a handler. NOT
    // async-signal-safe: from a signal handler, set a flag and call stop() from
    // a normal thread. In-flight requests finish (see ServerOptions::
    // shutdown_grace); new connections are refused.
    void stop();

    // True between the start and the end of run().
    [[nodiscard]] bool is_running() const;
    // Port the running server is bound to (0 if not running).
    [[nodiscard]] std::uint16_t local_port() const;
    // Block until the server is accepting connections. Returns false on
    // timeout, or if run() failed or ended first. Handy for tests:
    //   std::thread t([&] { app.run("127.0.0.1:0"); });
    //   app.wait_until_ready(std::chrono::seconds(5));
    bool wait_until_ready(std::chrono::milliseconds timeout = std::chrono::seconds(5));

    // Process one request through routing, middleware and the handler. Never
    // throws; handler/middleware exceptions become 500 responses. Used by the
    // server and by in-process tests.
    [[nodiscard]] Response handle_request(Request req) const;

    [[nodiscard]] const StateRegistry& state_registry() const noexcept { return state_registry_; }
    [[nodiscard]] StateRegistry& state_registry() noexcept { return state_registry_; }

private:
    void ensure_configurable(const char* what) const;

    Router router_;
    StateRegistry state_registry_;
    std::vector<std::shared_ptr<Middleware>> middlewares_;
    ServerOptions options_;
    // Shared (not unique) so the thread inside run() can keep it alive until it
    // has finished touching it, even if ~App() has already been released.
    std::shared_ptr<detail::RunState> run_;
};

} // namespace ourweb
