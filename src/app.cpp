#include "ourweb/app.hpp"

#include "chain.hpp"
#include "server.hpp"

#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace ourweb {

namespace detail {

// Run-time state shared between run(), stop() and the accessors. Heap-allocated
// so that App stays movable while the (non-movable) mutex lives here.
struct RunState {
    std::mutex mu;
    std::condition_variable cv;
    std::shared_ptr<Server> server;  // non-null only while run() is active
    bool running = false;
};

} // namespace detail

App::App() : run_(std::make_shared<detail::RunState>()) {}

App::~App() {
    if (!run_) return;  // moved-from
    // If another thread is inside run(), ask it to stop and wait for it so the
    // server never outlives the App it serves.
    stop();
    std::unique_lock<std::mutex> lk(run_->mu);
    run_->cv.wait(lk, [this] { return !run_->running; });
}

App::App(App&&) noexcept = default;
App& App::operator=(App&&) noexcept = default;

void App::ensure_configurable(const char* what) const {
    if (!run_) throw std::logic_error("ourweb: use of a moved-from App");
    std::lock_guard<std::mutex> lk(run_->mu);
    if (run_->running) {
        throw std::logic_error(std::string("ourweb: cannot ") + what +
                               " while the server is running; configure the App before run()");
    }
}

App& App::wrap(std::shared_ptr<Middleware> mw) {
    if (!mw) throw std::invalid_argument("ourweb: App::wrap() requires a non-null middleware");
    ensure_configurable("register middleware");
    middlewares_.push_back(std::move(mw));
    return *this;
}

App& App::wrap(MiddlewareFn fn) {
    if (!fn) throw std::invalid_argument("ourweb: App::wrap() requires a callable middleware");
    return wrap(std::make_shared<FunctionMiddleware>(std::move(fn)));
}

App& App::use_logger(std::string prefix) {
    return wrap(std::make_shared<LoggerMiddleware>(std::move(prefix)));
}

App& App::options(ServerOptions opts) {
    ensure_configurable("change server options");
    options_ = opts;
    return *this;
}

App& App::max_body_size(std::size_t bytes) {
    ensure_configurable("change server options");
    options_.max_body_size = bytes;
    return *this;
}

void App::run(std::string_view address) {
    if (!run_) throw std::logic_error("ourweb: use of a moved-from App");
    // Keep the run-state alive for as long as this thread uses it: ~App() may
    // return (and release its own reference) the moment we publish
    // "not running" below, while we are still inside notify_all().
    const std::shared_ptr<detail::RunState> rs_owner = run_;
    detail::RunState& rs = *rs_owner;

    std::shared_ptr<Server> server;
    {
        std::lock_guard<std::mutex> lk(rs.mu);
        if (rs.running) throw std::logic_error("ourweb: App::run() is already running");
        server = std::make_shared<Server>(*this, options_);
        rs.server = server;
        rs.running = true;
    }
    rs.cv.notify_all();

    // Whatever happens, tear the Server down completely (all workers have been
    // joined by Server::run) BEFORE publishing "not running", so that once
    // ~App()/stop()/wait_until_ready() are released nothing can still touch
    // the Server or the App.
    struct Done {
        detail::RunState& rs;
        std::shared_ptr<Server>& server;
        ~Done() {
            {
                std::lock_guard<std::mutex> lk(rs.mu);
                rs.server.reset();
            }
            server.reset();  // destroys the Server (no other owners remain)
            {
                std::lock_guard<std::mutex> lk(rs.mu);
                rs.running = false;
            }
            rs.cv.notify_all();
        }
    } done{rs, server};

    try {
        server->run(address);
    } catch (const std::exception& e) {
        std::cerr << "[ourweb] server failed: " << e.what() << '\n';
        throw;
    }
}

void App::stop() {
    if (!run_) return;
    std::lock_guard<std::mutex> lk(run_->mu);
    if (run_->server) run_->server->stop();
}

bool App::is_running() const {
    if (!run_) return false;
    std::lock_guard<std::mutex> lk(run_->mu);
    return run_->running;
}

std::uint16_t App::local_port() const {
    if (!run_) return 0;
    std::lock_guard<std::mutex> lk(run_->mu);
    return run_->server ? run_->server->port() : std::uint16_t{0};
}

bool App::wait_until_ready(std::chrono::milliseconds timeout) {
    if (!run_) return false;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::shared_ptr<Server> server;
    {
        std::unique_lock<std::mutex> lk(run_->mu);
        if (!run_->cv.wait_until(lk, deadline, [this] { return run_->server != nullptr; })) {
            return false;
        }
        server = run_->server;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    return server->wait_until_listening(remaining.count() > 0 ? remaining
                                                              : std::chrono::milliseconds(0));
}

Response App::handle_request(Request req) const {
    // Route first (this fills in the path parameters), then run the chain over
    // the one-and-only Request: no copies are made from here on.
    const Router::MatchResult match = router_.match(req.method(), req.path(), req);
    detail::ChainState chain(middlewares_, req, match.handler, match.path_matched,
                             match.allowed_methods);
    return chain.run();
}

} // namespace ourweb
