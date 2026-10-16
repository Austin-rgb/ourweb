#include "ourweb/app.hpp"
#include "server.hpp"
#include "ourweb/error.hpp"
#include "ourweb/http.hpp"

#include <iostream>
#include <utility>

namespace ourweb {

App::App() = default;

App::~App() {
    stop();
}

App::App(App&&) noexcept = default;
App& App::operator=(App&&) noexcept = default;

App& App::wrap(std::shared_ptr<Middleware> mw) {
    middlewares_.push_back(std::move(mw));
    return *this;
}

App& App::wrap(MiddlewareFn fn) {
    middlewares_.push_back(std::make_shared<FunctionMiddleware>(std::move(fn)));
    return *this;
}

App& App::use_logger(std::string prefix) {
    return wrap(std::make_shared<LoggerMiddleware>(std::move(prefix)));
}

void App::run(std::string_view address) {
    server_ = std::make_unique<Server>(*this);
    try {
        server_->run(address);
    } catch (const std::exception& e) {
        std::cerr << "[ourweb] failed to start server: " << e.what() << '\n';
        throw;
    }
}

void App::stop() {
    if (server_) {
        server_->stop();
    }
}

Response App::handle_request(Request req) const {
    // Middleware chain: middlewares_[0] is outermost (first registered runs first)
    struct Chain {
        const App* app;
        Request req;
        std::size_t index = 0;

        Response operator()() {
            if (index < app->middlewares_.size()) {
                auto mw = app->middlewares_[index];
                ++index;
                return mw->invoke(req, *this);
            }

            // Final route handler
            auto match = app->router_.match(req.method(), req.path(), req);
            if (match.handler) {
                try {
                    return match.handler(std::move(req));
                } catch (const std::exception& e) {
                    std::cerr << "[ourweb] handler exception: " << e.what() << '\n';
                    return internal_error();
                } catch (...) {
                    return internal_error();
                }
            }

            if (match.path_matched) {
                auto resp = method_not_allowed();
                std::string allow;
                for (std::size_t i = 0; i < match.allowed_methods.size(); ++i) {
                    if (i) allow += ", ";
                    allow += std::string(to_string(match.allowed_methods[i]));
                }
                if (!allow.empty()) {
                    resp.header("Allow", std::move(allow));
                }
                return resp;
            }

            return not_found();
        }
    };

    Chain chain{this, std::move(req), 0};
    return chain();
}

} // namespace ourweb
